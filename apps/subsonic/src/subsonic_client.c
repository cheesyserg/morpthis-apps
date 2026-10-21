#include "subsonic.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "vendor/http.h"
#include "vendor/json.h"
#include "vendor/md5.h"

// The app's whole API response budget, the same as the player's built-in
// client. A large getArtists response is the worst case; the JSON tree is freed
// as soon as the first page of entries has been copied out.
#define API_BODY_LIMIT (2 * 1024 * 1024)
#define API_TOKEN_LIMIT 65536
#define API_TIMEOUT 12

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static subsonic_credentials_t account;
static subsonic_quality_t quality = {SUBSONIC_FORMAT_MP3, 256};

static void reason(char *out, size_t size, const char *text) {
	if (out && size) {
		snprintf(out, size, "%s", text);
	}
}

static bool valid_text(const char *text, size_t capacity) {
	return text && strnlen(text, capacity) < capacity;
}

static bool normalize(subsonic_credentials_t *c, char *error, size_t size) {
	if (!valid_text(c->server, sizeof(c->server)) || !valid_text(c->username, sizeof(c->username)) ||
		!valid_text(c->password, sizeof(c->password)) || !c->username[0] || !c->password[0]) {
		reason(error, size, "Server, username and password are required.");
		return false;
	}
	bool secure = strncasecmp(c->server, "https://", 8) == 0;
	if (!secure && strncasecmp(c->server, "http://", 7) != 0) {
		reason(error, size, "The server must start with http:// or https://.");
		return false;
	}
	memcpy(c->server, secure ? "https://" : "http://", secure ? 8 : 7);
	const char *authority = c->server + (secure ? 8 : 7);
	if (!*authority || *authority == '/' || strpbrk(authority, "@?#\\")) {
		reason(error, size, "The server address is not a valid URL.");
		return false;
	}
	for (const unsigned char *p = (const unsigned char *)authority; *p; p++) {
		if (isspace(*p) || *p < 32 || *p == 127) {
			reason(error, size, "The server address is not a valid URL.");
			return false;
		}
	}
	const char *end = strchr(authority, '/');
	if (!end) end = authority + strlen(authority);
	const char *colon = memchr(authority, ':', (size_t)(end - authority));
	const char *host_end = colon ? colon : end;
	if (host_end == authority || host_end - authority >= 256) {
		reason(error, size, "The server address is not a valid URL.");
		return false;
	}
	for (const unsigned char *p = (const unsigned char *)authority; p < (const unsigned char *)host_end; p++) {
		if (!isalnum(*p) && *p != '.' && *p != '-' && *p != '_') {
			reason(error, size, "The server address is not a valid URL.");
			return false;
		}
	}
	if (colon) {
		unsigned port = 0;
		for (const char *p = colon + 1; p < end; p++) {
			if (*p < '0' || *p > '9' || (port = port * 10 + (unsigned)(*p - '0')) > 65535) {
				reason(error, size, "The server address has an invalid port.");
				return false;
			}
		}
		if (!port) {
			reason(error, size, "The server address has an invalid port.");
			return false;
		}
	}
	size_t len = strlen(c->server);
	while (len && c->server[len - 1] == '/') {
		c->server[--len] = '\0';
	}
	// Accept a pasted /rest URL as well as the documented server base.
	if (len >= 5 && strcmp(c->server + len - 5, "/rest") == 0) {
		c->server[len - 5] = '\0';
	}
	return true;
}

void subsonic_client_init(void) {
	pthread_mutex_lock(&lock);
	memset(&account, 0, sizeof(account));
	quality.format = SUBSONIC_FORMAT_MP3;
	quality.max_bitrate = 256;
	pthread_mutex_unlock(&lock);
}

bool subsonic_configure(const subsonic_credentials_t *credentials, subsonic_quality_t q,
						char *error, size_t size) {
	reason(error, size, "");
	if (!credentials) {
		reason(error, size, "Server, username and password are required.");
		return false;
	}
	subsonic_credentials_t c = *credentials;
	if (!normalize(&c, error, size)) {
		memset(&c, 0, sizeof(c));
		return false;
	}
	if (q.format < SUBSONIC_FORMAT_MP3 || q.format > SUBSONIC_FORMAT_FLAC) {
		q.format = SUBSONIC_FORMAT_MP3;
	}
	if (q.max_bitrate < 64 || q.max_bitrate > 320) {
		q.max_bitrate = 256;
	}
	pthread_mutex_lock(&lock);
	account = c;
	quality = q;
	pthread_mutex_unlock(&lock);
	memset(&c, 0, sizeof(c));
	return true;
}

bool subsonic_configured(void) {
	pthread_mutex_lock(&lock);
	bool ok = account.server[0] && account.username[0] && account.password[0];
	pthread_mutex_unlock(&lock);
	return ok;
}

void subsonic_disconnect(void) {
	pthread_mutex_lock(&lock);
	memset(&account, 0, sizeof(account));
	pthread_mutex_unlock(&lock);
}

void subsonic_credentials(subsonic_credentials_t *out) {
	pthread_mutex_lock(&lock);
	if (out) {
		*out = account;
	}
	pthread_mutex_unlock(&lock);
}

subsonic_quality_t subsonic_quality(void) {
	pthread_mutex_lock(&lock);
	subsonic_quality_t copy = quality;
	pthread_mutex_unlock(&lock);
	return copy;
}

void subsonic_set_quality(subsonic_quality_t q) {
	if (q.format < SUBSONIC_FORMAT_MP3 || q.format > SUBSONIC_FORMAT_FLAC) {
		q.format = SUBSONIC_FORMAT_MP3;
	}
	if (q.max_bitrate < 64 || q.max_bitrate > 320) {
		q.max_bitrate = 256;
	}
	pthread_mutex_lock(&lock);
	quality = q;
	pthread_mutex_unlock(&lock);
}

void subsonic_abort(void) { http_abort_all(); }

static bool request_url(const char *endpoint, const char *key, const char *value,
						char *out, size_t size, char *error, size_t error_size) {
	subsonic_credentials_t c;
	subsonic_credentials(&c);
	if (!c.server[0] || !c.username[0] || !c.password[0]) {
		reason(error, error_size, "No server is configured.");
		return false;
	}
	unsigned char random[16];
	int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
	size_t done = 0;
	while (fd >= 0 && done < sizeof(random)) {
		ssize_t n = read(fd, random + done, sizeof(random) - done);
		if (n > 0) {
			done += (size_t)n;
		} else if (n < 0 && errno == EINTR) {
			continue;
		} else {
			break;
		}
	}
	if (fd >= 0) {
		close(fd);
	}
	if (done != sizeof(random)) {
		reason(error, error_size, "No secure random source.");
		memset(&c, 0, sizeof(c));
		return false;
	}
	static const char hex[] = "0123456789abcdef";
	char salt[33], token[MD5_HEX_LEN];
	for (size_t i = 0; i < sizeof(random); i++) {
		salt[i * 2] = hex[random[i] >> 4];
		salt[i * 2 + 1] = hex[random[i] & 15];
	}
	salt[32] = '\0';
	char signed_text[SUBSONIC_PASSWORD_MAX + sizeof(salt)];
	snprintf(signed_text, sizeof(signed_text), "%s%s", c.password, salt);
	md5_hex(signed_text, strlen(signed_text), token);
	memset(signed_text, 0, sizeof(signed_text));
	char user[SUBSONIC_USER_MAX * 3 + 4], encoded[SUBSONIC_ID_MAX * 3 + 4];
	http_url_encode(c.username, user, sizeof(user));
	if (key && (!value || strlen(value) >= SUBSONIC_ID_MAX)) {
		reason(error, error_size, "The server returned an invalid identifier.");
		memset(&c, 0, sizeof(c));
		return false;
	}
	if (key) {
		http_url_encode(value, encoded, sizeof(encoded));
	}
	int n = snprintf(out, size, "%s/rest/%s.view?u=%s&t=%s&s=%s&v=1.16.1&c=morpthis&f=json%s%s%s",
					 c.server, endpoint, user, token, salt, key ? "&" : "", key ? key : "", key ? "=" : "");
	bool ok = n > 0 && (size_t)n < size;
	if (ok && key) {
		size_t used = (size_t)n;
		ok = strlen(encoded) < size - used;
		if (ok) {
			memcpy(out + used, encoded, strlen(encoded) + 1);
		}
	}
	memset(&c, 0, sizeof(c));
	if (!ok) {
		reason(error, error_size, "The request URL is too long.");
	}
	return ok;
}

bool subsonic_stream_url(const char *id, subsonic_quality_t q, char *out, size_t size,
						 char *error, size_t error_size) {
	reason(error, error_size, "");
	if (!out || !size || !id || !id[0] || q.format < SUBSONIC_FORMAT_MP3 ||
		q.format > SUBSONIC_FORMAT_FLAC ||
		!request_url("stream", "id", id, out, size, error, error_size)) {
		return false;
	}
	size_t used = strlen(out);
	const char *format = q.format == SUBSONIC_FORMAT_RAW ? "raw"
						 : q.format == SUBSONIC_FORMAT_OPUS ? "opus"
						 : q.format == SUBSONIC_FORMAT_FLAC ? "flac"
						 : "mp3";
	bool lossless = q.format == SUBSONIC_FORMAT_RAW || q.format == SUBSONIC_FORMAT_FLAC;
	int n = lossless ? snprintf(out + used, size - used, "&format=%s", format)
					 : snprintf(out + used, size - used, "&format=%s&maxBitRate=%d", format, q.max_bitrate);
	if (n < 0 || (size_t)n >= size - used) {
		reason(error, error_size, "The request URL is too long.");
		return false;
	}
	return true;
}

static bool api_extra(const char *endpoint, const char *key, const char *value, const char *extra,
					  char **body, json_doc_t *doc, int *response, char *error, size_t size) {
	reason(error, size, "");
	char url[SUBSONIC_URL_MAX];
	if (!request_url(endpoint, key, value, url, sizeof(url), error, size)) {
		return false;
	}
	if (extra && extra[0]) {
		size_t used = strlen(url);
		int n = snprintf(url + used, sizeof(url) - used, "&%s", extra);
		if (n < 0 || (size_t)n >= sizeof(url) - used) {
			reason(error, size, "The request URL is too long.");
			return false;
		}
	}
	int status = 0;
	size_t len = 0;
	http_req_t request = {.want_error_body = true, .no_https_downgrade = true};
	bool fetched = http_request(url, &request, body, &len, API_BODY_LIMIT + 1, API_TIMEOUT, &status);
	memset(url, 0, sizeof(url));
	if (status == 401 || status == 403) {
		free(*body);
		*body = NULL;
		reason(error, size, "Login failed. Check the settings.");
		return false;
	}
	if (!fetched) {
		reason(error, size, http_last_error() ? http_last_error() : "Cannot reach the server.");
		return false;
	}
	if (len > API_BODY_LIMIT || !json_parse_limited(*body, doc, API_TOKEN_LIMIT)) {
		reason(error, size, len >= API_BODY_LIMIT ? "The server response is too large." : "The server sent an invalid response.");
		free(*body);
		*body = NULL;
		return false;
	}
	*response = json_get(doc, json_root(doc), "subsonic-response");
	char result[24];
	bool ok = status == 200 && json_obj_str(doc, *response, "status", result, sizeof(result)) && strcmp(result, "ok") == 0;
	if (!ok) {
		int detail = json_get(doc, *response, "error");
		long code = json_obj_long(doc, detail, "code", 0);
		if (code == 30 || code == 40 || code == 41 || code == 42 || code == 43 || code == 44 || status == 401 || status == 403) {
			reason(error, size, "Login failed. Check the settings.");
		} else {
			char message[192] = "";
			json_obj_str(doc, detail, "message", message, sizeof(message));
			reason(error, size, message[0] ? message : "The server refused the request.");
		}
		json_free(doc);
		free(*body);
		*body = NULL;
	}
	return ok;
}

static bool api(const char *endpoint, const char *key, const char *value, char **body,
				json_doc_t *doc, int *response, char *error, size_t size) {
	return api_extra(endpoint, key, value, NULL, body, doc, response, error, size);
}

bool subsonic_star(subsonic_entry_kind_t kind, const char *id, bool on, char *error, size_t size) {
	const char *key = kind == SUBSONIC_ALBUM ? "albumId"
					  : kind == SUBSONIC_ARTIST ? "artistId"
					  : kind == SUBSONIC_TRACK ? "id"
					  : NULL;
	if (!key || !id || !id[0]) {
		reason(error, size, "This item cannot be starred.");
		return false;
	}
	char *body = NULL;
	json_doc_t doc;
	int response;
	bool ok = api(on ? "star" : "unstar", key, id, &body, &doc, &response, error, size);
	if (ok) {
		json_free(&doc);
		free(body);
	}
	return ok;
}

bool subsonic_scrobble(const char *id, bool submission, char *error, size_t size) {
	if (!id || !id[0]) {
		reason(error, size, "The track has no identifier.");
		return false;
	}
	char *body = NULL;
	json_doc_t doc;
	int response;
	bool ok = api_extra("scrobble", "id", id, submission ? "submission=true" : "submission=false",
						&body, &doc, &response, error, size);
	if (ok) {
		json_free(&doc);
		free(body);
	}
	return ok;
}

bool subsonic_ping(char *error, size_t size) {
	char *body = NULL;
	json_doc_t doc;
	int response;
	if (!api("ping", NULL, NULL, &body, &doc, &response, error, size)) {
		return false;
	}
	json_free(&doc);
	free(body);
	return true;
}

static bool identifier(const json_doc_t *doc, int obj, char *out, size_t size) {
	int tok = json_get(doc, obj, "id");
	if (tok < 0) {
		return false;
	}
	if (doc->toks[tok].type == JSON_NUMBER) {
		const json_tok_t *t = &doc->toks[tok];
		size_t len = (size_t)(t->end - t->start);
		if (!len || len >= size) return false;
		for (size_t i = 0; i < len; i++) {
			if (doc->text[t->start + i] < '0' || doc->text[t->start + i] > '9') return false;
		}
		// Preserve integer IDs verbatim even above INT64_MAX; do not turn a
		// fractional/exponent form or an overflow into a different identity.
		memcpy(out, doc->text + t->start, len);
		out[len] = '\0';
		return true;
	}
	// Never silently cut an opaque identifier down to a different identifier.
	return json_str_exact(doc, tok, out, size) && out[0];
}

static bool entry(const json_doc_t *doc, int obj, subsonic_entry_kind_t kind, subsonic_entry_t *out) {
	memset(out, 0, sizeof(*out));
	out->kind = kind;
	if (kind == SUBSONIC_GENRE) {
		// A genre has no id; its name is what getSongsByGenre expects.
		json_obj_str(doc, obj, "value", out->id, sizeof(out->id));
		json_obj_str(doc, obj, "value", out->title, sizeof(out->title));
	}
	if (!out->id[0] && !identifier(doc, obj, out->id, sizeof(out->id))) {
		return false;
	}
	if (!out->title[0] && !json_obj_str(doc, obj, "title", out->title, sizeof(out->title))) {
		json_obj_str(doc, obj, "name", out->title, sizeof(out->title));
	}
	if (!out->title[0]) {
		snprintf(out->title, sizeof(out->title), "%s", out->id);
	}
	json_obj_str(doc, obj, "artist", out->artist, sizeof(out->artist));
	json_obj_str(doc, obj, "album", out->album, sizeof(out->album));
	json_obj_str(doc, obj, "suffix", out->suffix, sizeof(out->suffix));
	long duration = json_obj_long(doc, obj, "duration", 0);
	out->duration = duration > 0 && duration <= INT_MAX ? (int)duration : 0;
	out->track_number = (int)json_obj_long(doc, obj, "track", 0);
	out->starred = json_get(doc, obj, "starred") >= 0;
	return true;
}

typedef struct {
	subsonic_entry_t *entries;
	int offset, limit, seen, count;
	bool more;
} page_t;

static void append(const json_doc_t *doc, int obj, subsonic_entry_kind_t kind, page_t *page) {
	subsonic_entry_t item;
	if (!entry(doc, obj, kind, &item)) {
		return;
	}
	if (page->seen++ < page->offset) {
		return;
	}
	if (page->count < page->limit) {
		page->entries[page->count++] = item;
	} else {
		page->more = true;
	}
}

static void array(const json_doc_t *doc, int arr, subsonic_entry_kind_t kind, bool mixed, page_t *page) {
	// Linear token walk rather than repeated json_at(0..n), which re-scans the
	// preceding entries each time. Subtrees are skipped by their end offsets.
	if (arr < 0 || doc->toks[arr].type != JSON_ARRAY) {
		return;
	}
	for (int i = arr + 1; i < doc->count && doc->toks[i].start < doc->toks[arr].end && !page->more;) {
		int obj = i;
		subsonic_entry_kind_t actual = mixed ? (json_obj_bool(doc, obj, "isDir", false) ? SUBSONIC_DIRECTORY : SUBSONIC_TRACK) : kind;
		append(doc, obj, actual, page);
		int end = doc->toks[obj].end;
		do { i++; } while (i < doc->count && doc->toks[i].start < end);
	}
}

int subsonic_browse(subsonic_browse_t browse, const char *id, int offset, subsonic_entry_t *entries,
					int limit, bool *more, char *error, size_t size) {
	if (more) {
		*more = false;
	}
	if (!entries || limit <= 0 || limit > SUBSONIC_PAGE_SIZE || offset < 0) {
		reason(error, size, "Invalid browse request.");
		return -1;
	}
	const char *endpoint, *key = NULL, *extra = NULL;
	switch (browse) {
	case SUBSONIC_BROWSE_FOLDERS: endpoint = "getMusicFolders"; break;
	case SUBSONIC_BROWSE_INDEXES: endpoint = "getIndexes"; key = "musicFolderId"; break;
	case SUBSONIC_BROWSE_DIRECTORY: endpoint = "getMusicDirectory"; key = "id"; break;
	case SUBSONIC_BROWSE_ARTISTS: endpoint = "getArtists"; break;
	case SUBSONIC_BROWSE_ARTIST: endpoint = "getArtist"; key = "id"; break;
	case SUBSONIC_BROWSE_ALBUM: endpoint = "getAlbum"; key = "id"; break;
	case SUBSONIC_BROWSE_ALBUMS: endpoint = "getAlbumList2"; extra = "type=alphabeticalByName&size=200"; break;
	case SUBSONIC_BROWSE_PLAYLISTS: endpoint = "getPlaylists"; break;
	case SUBSONIC_BROWSE_PLAYLIST: endpoint = "getPlaylist"; key = "id"; break;
	case SUBSONIC_BROWSE_STARRED: endpoint = "getStarred2"; break;
	case SUBSONIC_BROWSE_RANDOM: endpoint = "getRandomSongs"; extra = "size=200"; break;
	case SUBSONIC_BROWSE_NEWEST: endpoint = "getAlbumList2"; extra = "type=newest&size=200"; break;
	case SUBSONIC_BROWSE_GENRES: endpoint = "getGenres"; break;
	case SUBSONIC_BROWSE_GENRE: endpoint = "getSongsByGenre"; key = "genre"; extra = "count=200"; break;
	case SUBSONIC_BROWSE_SEARCH:
		endpoint = "search3";
		key = "query";
		extra = "songCount=100&albumCount=100&artistCount=100";
		break;
	default: return -1;
	}
	char *body = NULL;
	json_doc_t doc;
	int response;
	if (!api_extra(endpoint, key, id, extra, &body, &doc, &response, error, size)) {
		return -1;
	}
	page_t page = {entries, offset, limit, 0, 0, false};
	if (browse == SUBSONIC_BROWSE_ARTISTS || browse == SUBSONIC_BROWSE_INDEXES) {
		int root = json_get(&doc, response, browse == SUBSONIC_BROWSE_ARTISTS ? "artists" : "indexes");
		int indexes = json_get(&doc, root, "index");
		for (int i = 0; i < json_len(&doc, indexes) && !page.more; i++) {
			int index = json_at(&doc, indexes, i);
			array(&doc, json_get(&doc, index, "artist"), browse == SUBSONIC_BROWSE_ARTISTS ? SUBSONIC_ARTIST : SUBSONIC_DIRECTORY, false, &page);
		}
		// Loose songs directly inside a music folder, where the server has them.
		if (browse == SUBSONIC_BROWSE_INDEXES) {
			array(&doc, json_get(&doc, root, "child"), SUBSONIC_TRACK, true, &page);
		}
	} else if (browse == SUBSONIC_BROWSE_FOLDERS) {
		array(&doc, json_path(&doc, response, "musicFolders.musicFolder"), SUBSONIC_MUSIC_FOLDER, false, &page);
	} else if (browse == SUBSONIC_BROWSE_DIRECTORY) {
		array(&doc, json_path(&doc, response, "directory.child"), SUBSONIC_TRACK, true, &page);
	} else if (browse == SUBSONIC_BROWSE_ARTIST) {
		array(&doc, json_path(&doc, response, "artist.album"), SUBSONIC_ALBUM, false, &page);
	} else if (browse == SUBSONIC_BROWSE_ALBUM) {
		array(&doc, json_path(&doc, response, "album.song"), SUBSONIC_TRACK, false, &page);
	} else if (browse == SUBSONIC_BROWSE_ALBUMS || browse == SUBSONIC_BROWSE_NEWEST) {
		array(&doc, json_path(&doc, response, "albumList2.album"), SUBSONIC_ALBUM, false, &page);
	} else if (browse == SUBSONIC_BROWSE_PLAYLISTS) {
		array(&doc, json_path(&doc, response, "playlists.playlist"), SUBSONIC_PLAYLIST, false, &page);
	} else if (browse == SUBSONIC_BROWSE_PLAYLIST) {
		array(&doc, json_path(&doc, response, "playlist.entry"), SUBSONIC_TRACK, true, &page);
	} else if (browse == SUBSONIC_BROWSE_STARRED) {
		array(&doc, json_path(&doc, response, "starred2.song"), SUBSONIC_TRACK, false, &page);
		array(&doc, json_path(&doc, response, "starred2.album"), SUBSONIC_ALBUM, false, &page);
		array(&doc, json_path(&doc, response, "starred2.artist"), SUBSONIC_ARTIST, false, &page);
	} else if (browse == SUBSONIC_BROWSE_RANDOM) {
		array(&doc, json_path(&doc, response, "randomSongs.song"), SUBSONIC_TRACK, false, &page);
	} else if (browse == SUBSONIC_BROWSE_GENRES) {
		array(&doc, json_path(&doc, response, "genres.genre"), SUBSONIC_GENRE, false, &page);
	} else if (browse == SUBSONIC_BROWSE_GENRE) {
		array(&doc, json_path(&doc, response, "songsByGenre.song"), SUBSONIC_TRACK, false, &page);
	} else if (browse == SUBSONIC_BROWSE_SEARCH) {
		array(&doc, json_path(&doc, response, "searchResult3.song"), SUBSONIC_TRACK, false, &page);
		array(&doc, json_path(&doc, response, "searchResult3.album"), SUBSONIC_ALBUM, false, &page);
		array(&doc, json_path(&doc, response, "searchResult3.artist"), SUBSONIC_ARTIST, false, &page);
	} else {
		array(&doc, json_path(&doc, response, "album.song"), SUBSONIC_TRACK, false, &page);
	}
	if (more) {
		*more = page.more;
	}
	json_free(&doc);
	free(body);
	return page.count;
}
