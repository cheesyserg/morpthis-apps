#include "radio_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <morpthis/morpthis.h>

// The whole store is small enough to keep in memory: a hundred favourites, the
// last thirty stations played, fifty added by hand. Writing is line-based, so
// no file-sized buffer is needed at any point.

#define STORE_FILE "stations.txt"
#define STORE_READ_MAX (192 * 1024)
#define LINE_MAX 2048

static radio_station_t favourites[RADIO_FAVOURITE_MAX];
static int favourite_count;
static radio_station_t recents[RADIO_RECENT_MAX];
static int recent_count;
static radio_station_t customs[RADIO_CUSTOM_MAX];
static int custom_count;

static void copy_station(radio_station_t *out, const radio_station_t *in) {
	*out = *in;
}

static int find_by_url(const radio_station_t *list, int count, const char *url) {
	for (int i = 0; i < count; i++) {
		if (strcmp(list[i].url, url) == 0) {
			return i;
		}
	}
	return -1;
}

static void remove_at(radio_station_t *list, int *count, int index) {
	if (index < 0 || index >= *count) {
		return;
	}
	if (index < *count - 1) {
		memmove(&list[index], &list[index + 1],
				sizeof(*list) * (size_t)(*count - index - 1));
	}
	(*count)--;
	memset(&list[*count], 0, sizeof(*list));
}

// ---------------------------------------------------------------------------
// The file
//
// One record per line: kind, then the six station fields. An escape keeps a
// tab, a line break or a backslash in a name from splitting the record; URLs
// cannot contain any of them, but names come from other people's servers.
// ---------------------------------------------------------------------------

static bool append_field(char *out, size_t size, size_t *used, const char *value) {
	for (const char *p = value; *p; p++) {
		const char *escaped = NULL;
		switch (*p) {
		case '\\':
			escaped = "\\\\";
			break;
		case '\t':
			escaped = "\\t";
			break;
		case '\n':
			escaped = "\\n";
			break;
		case '\r':
			escaped = "\\r";
			break;
		default:
			break;
		}
		size_t need = escaped ? 2 : 1;
		if (*used + need + 1 >= size) {
			return false;
		}
		if (escaped) {
			out[(*used)++] = escaped[0];
			out[(*used)++] = escaped[1];
		} else {
			out[(*used)++] = *p;
		}
	}
	out[*used] = '\0';
	return true;
}

static bool append_separator(char *out, size_t size, size_t *used) {
	if (*used + 1 >= size) {
		return false;
	}
	out[(*used)++] = '\t';
	out[*used] = '\0';
	return true;
}

static void unescape_field(char *text) {
	char *read = text;
	char *write = text;
	while (*read) {
		if (*read != '\\') {
			*write++ = *read++;
			continue;
		}
		read++;
		switch (*read) {
		case 't':
			*write++ = '\t';
			read++;
			break;
		case 'n':
			*write++ = '\n';
			read++;
			break;
		case 'r':
			*write++ = '\r';
			read++;
			break;
		default:
			*write++ = *read ? *read++ : '\\';
			break;
		}
	}
	*write = '\0';
}

static bool write_record(morpthis_file_t *file, char kind, const radio_station_t *station) {
	char line[LINE_MAX];
	size_t used = 1;
	line[0] = kind;
	if (!append_separator(line, sizeof(line), &used) ||
		!append_field(line, sizeof(line), &used, station->uuid) ||
		!append_separator(line, sizeof(line), &used) ||
		!append_field(line, sizeof(line), &used, station->name) ||
		!append_separator(line, sizeof(line), &used) ||
		!append_field(line, sizeof(line), &used, station->url) ||
		!append_separator(line, sizeof(line), &used) ||
		!append_field(line, sizeof(line), &used, station->country) ||
		!append_separator(line, sizeof(line), &used) ||
		!append_field(line, sizeof(line), &used, station->codec) ||
		!append_separator(line, sizeof(line), &used)) {
		return false;
	}
	char bitrate[16];
	snprintf(bitrate, sizeof(bitrate), "%d", station->bitrate);
	if (!append_field(line, sizeof(line), &used, bitrate)) {
		return false;
	}
	line[used++] = '\n';
	line[used] = '\0';
	return morpthis_fs_write(file, line, used) == used;
}

static void write_list(morpthis_file_t *file, char kind, const radio_station_t *list, int count) {
	for (int i = 0; i < count; i++) {
		if (!write_record(file, kind, &list[i])) {
			morpthis_logw("radio", "could not write a station to %s", STORE_FILE);
			return;
		}
	}
}

void radio_store_save(void) {
	morpthis_file_t *file = morpthis_fs_open(STORE_FILE, MORPTHIS_FS_WRITE);
	if (!file) {
		morpthis_logw("radio", "cannot write %s in the data folder", STORE_FILE);
		return;
	}
	write_list(file, 'F', favourites, favourite_count);
	write_list(file, 'R', recents, recent_count);
	write_list(file, 'C', customs, custom_count);
	morpthis_fs_close(file);
}

// Splits a record in place. Returns the field count, or 0 when the line is
// malformed.
static int split_record(char *line, char **fields, int max) {
	int count = 0;
	char *p = line;
	while (count < max && p) {
		fields[count++] = p;
		char *tab = strchr(p, '\t');
		if (!tab) {
			break;
		}
		*tab = '\0';
		p = tab + 1;
	}
	return count;
}

static void parse_line(char *line) {
	char kind = line[0];
	if (kind != 'F' && kind != 'R' && kind != 'C') {
		return;
	}
	char *fields[7];
	int count = split_record(line, fields, 7);
	if (count != 7) {
		return;
	}

	radio_station_t station;
	memset(&station, 0, sizeof(station));
	unescape_field(fields[1]);
	unescape_field(fields[2]);
	unescape_field(fields[3]);
	unescape_field(fields[4]);
	unescape_field(fields[5]);
	unescape_field(fields[6]);
	snprintf(station.uuid, sizeof(station.uuid), "%s", fields[1]);
	snprintf(station.name, sizeof(station.name), "%s", fields[2]);
	snprintf(station.url, sizeof(station.url), "%s", fields[3]);
	snprintf(station.country, sizeof(station.country), "%s", fields[4]);
	snprintf(station.codec, sizeof(station.codec), "%s", fields[5]);
	station.bitrate = atoi(fields[6]);
	if (!station.name[0] || !station.url[0]) {
		return;
	}

	if (kind == 'F' && favourite_count < RADIO_FAVOURITE_MAX) {
		copy_station(&favourites[favourite_count++], &station);
	} else if (kind == 'R' && recent_count < RADIO_RECENT_MAX) {
		copy_station(&recents[recent_count++], &station);
	} else if (kind == 'C' && custom_count < RADIO_CUSTOM_MAX) {
		copy_station(&customs[custom_count++], &station);
	}
}

bool radio_store_load(void) {
	favourite_count = 0;
	recent_count = 0;
	custom_count = 0;

	char *text = malloc(STORE_READ_MAX);
	if (!text) {
		return false;
	}
	size_t length = 0;
	long got = morpthis_fs_read_all(STORE_FILE, text, STORE_READ_MAX - 1, &length);
	if (got < 0) {
		free(text);
		return false; // no file yet is the normal first run
	}
	text[length] = '\0';

	// The file was copied from the package or written by us; either way it is
	// lines. Split on the newline and hand each one to the parser.
	char *line = text;
	while (line && *line) {
		char *end = strchr(line, '\n');
		if (end) {
			*end = '\0';
		}
		if (*line) {
			parse_line(line);
		}
		line = end ? end + 1 : NULL;
	}
	free(text);
	return true;
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

static int copy_list(const radio_station_t *list, int count, radio_station_t *out, int max) {
	if (count > max) {
		count = max;
	}
	if (out && count > 0) {
		memcpy(out, list, sizeof(*out) * (size_t)count);
	}
	return count;
}

int radio_store_favourites(radio_station_t *out, int max) {
	return copy_list(favourites, favourite_count, out, max);
}

int radio_store_recent(radio_station_t *out, int max) {
	return copy_list(recents, recent_count, out, max);
}

int radio_store_custom(radio_station_t *out, int max) {
	return copy_list(customs, custom_count, out, max);
}

bool radio_store_is_favourite(const radio_station_t *station) {
	return station && find_by_url(favourites, favourite_count, station->url) >= 0;
}

bool radio_store_toggle_favourite(const radio_station_t *station) {
	if (!station || !station->url[0]) {
		return false;
	}
	int index = find_by_url(favourites, favourite_count, station->url);
	if (index >= 0) {
		remove_at(favourites, &favourite_count, index);
		radio_store_save();
		return false;
	}
	if (favourite_count >= RADIO_FAVOURITE_MAX) {
		return false;
	}
	// Newest first: the star is most often looked at just after it is used.
	memmove(&favourites[1], &favourites[0], sizeof(favourites[0]) * (size_t)favourite_count);
	copy_station(&favourites[0], station);
	favourite_count++;
	radio_store_save();
	return true;
}

void radio_store_note_played(const radio_station_t *station) {
	if (!station || !station->url[0]) {
		return;
	}
	int index = find_by_url(recents, recent_count, station->url);
	if (index >= 0) {
		radio_station_t seen = recents[index];
		remove_at(recents, &recent_count, index);
		memmove(&recents[1], &recents[0], sizeof(recents[0]) * (size_t)recent_count);
		copy_station(&recents[0], &seen);
		recent_count++;
		radio_store_save();
		return;
	}
	if (recent_count >= RADIO_RECENT_MAX) {
		recent_count = RADIO_RECENT_MAX - 1;
	}
	memmove(&recents[1], &recents[0], sizeof(recents[0]) * (size_t)recent_count);
	copy_station(&recents[0], station);
	recent_count++;
	radio_store_save();
}

bool radio_store_add_custom(const radio_station_t *station) {
	if (!station || !station->name[0] || !station->url[0]) {
		return false;
	}
	if (find_by_url(customs, custom_count, station->url) >= 0) {
		return true; // already there, and that is what the user asked for
	}
	if (custom_count >= RADIO_CUSTOM_MAX) {
		return false;
	}
	copy_station(&customs[custom_count++], station);
	radio_store_save();
	return true;
}
