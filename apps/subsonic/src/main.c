// Subsonic as a Morpthis app.
//
// The test this app exists for: can a network music client live in a package
// on the card instead of inside the player? It reaches the DAC through
// morpthis_audio_open/write, does its own HTTP and TLS, and carries dr_mp3 and
// dr_flac.
//
// API 1 has no text-entry widget, so the server, username and password come
// from merp_data/subsonic/settings.json. The settings screen shows the path and
// toggles the two things that are pleasant to change without a PC: strict or
// relaxed TLS, and the MP3 bitrate.

#include <morpthis/morpthis.h>

#include <signal.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "browse_worker.h"
#include "settings.h"
#include "stream_player.h"
#include "subsonic.h"
#include "vendor/tls.h"

MORPTHIS_DEFINE_API_VERSION();

#define MAX_ENTRIES 200
#define MAX_DEPTH 8

typedef struct {
	subsonic_browse_t kind;
	char id[SUBSONIC_ID_MAX];
	char title[64];
} view_t;

typedef struct {
	const char *title;
	subsonic_browse_t browse;
} root_item_t;

static const root_item_t ROOT_ITEMS[] = {
	{"Artists", SUBSONIC_BROWSE_ARTISTS},
	{"Albums", SUBSONIC_BROWSE_ALBUMS},
	{"Folders", SUBSONIC_BROWSE_FOLDERS},
	{"Playlists", SUBSONIC_BROWSE_PLAYLISTS},
	{"Genres", SUBSONIC_BROWSE_GENRES},
	{"Starred", SUBSONIC_BROWSE_STARRED},
	{"Random", SUBSONIC_BROWSE_RANDOM},
	{"Newest", SUBSONIC_BROWSE_NEWEST},
	{"Search", SUBSONIC_BROWSE_SEARCH},
};

#define ROOT_COUNT ((int)(sizeof(ROOT_ITEMS) / sizeof(ROOT_ITEMS[0])))

static int g_width;
static int g_height;

static morpthis_screen_t *g_main_screen;
static morpthis_screen_t *g_settings_screen;
static morpthis_screen_t *g_more_screen;
static morpthis_screen_t *g_queue_screen;
static morpthis_screen_t *g_search_screen;
static morpthis_widget_t *g_status;
static morpthis_widget_t *g_list;
static morpthis_widget_t *g_stop_button;
static morpthis_widget_t *g_more_star;
static morpthis_widget_t *g_queue_list;
static morpthis_widget_t *g_search_label;

static morpthis_widget_t *g_settings_label;
static morpthis_widget_t *g_tls_button;
static morpthis_widget_t *g_quality_button;

static subsonic_settings_t g_settings;
static subsonic_entry_t g_entries[MAX_ENTRIES];
static int g_entry_count;

// The play queue is independent of the list on screen: browsing an album and
// then another one must not change what Next means.
static subsonic_entry_t g_queue[MAX_ENTRIES];
static int g_queue_count;
static int g_queue_index = -1;

static view_t g_views[MAX_DEPTH];
static int g_depth;
static unsigned g_view_generation;
static bool g_browsing;
static bool g_root_fallback_done;

static bool g_playing;
static bool g_paused;
static char g_now_playing[200];

static char g_search_query[64];

static bool g_have_error;
static char g_last_error[128];
static bool g_shutting_down;
static bool g_settings_valid;

static void update_more_screen(void);
static void update_queue_screen(void);
static void enqueue_scrobble(const char *id, bool submission);
static void open_settings(morpthis_widget_t *widget, void *user);
static void open_more(morpthis_widget_t *widget, void *user);
static void open_queue(morpthis_widget_t *widget, void *user);
static void open_search(morpthis_widget_t *widget, void *user);
static void back_to_main(morpthis_widget_t *widget, void *user);

// ---------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------

static void clip(char *out, size_t size, const char *text, size_t limit) {
	if (!out || size == 0) {
		return;
	}
	if (!text) {
		out[0] = '\0';
		return;
	}
	size_t n = strlen(text);
	if (n > limit) {
		n = limit;
	}
	if (n >= size) {
		n = size - 1;
	}
	// Do not leave half of a UTF-8 sequence at the end of a title.
	while (n > 0 && ((unsigned char)text[n] & 0xC0) == 0x80) {
		n--;
	}
	memcpy(out, text, n);
	out[n] = '\0';
}

static void set_status(const char *first, const char *second) {
	char a[72], b[72], text[148];
	clip(a, sizeof(a), first, 64);
	clip(b, sizeof(b), second, 64);
	if (b[0]) {
		snprintf(text, sizeof(text), "%s\n%s", a, b);
	} else {
		snprintf(text, sizeof(text), "%s", a);
	}
	if (g_status) {
		morpthis_widget_set_text(g_status, text);
	}
}

static const char *current_view_title(void) {
	return g_depth > 0 ? g_views[g_depth - 1].title : "Subsonic";
}

static void refresh_status(void) {
	if (g_playing) {
		set_status(g_paused ? "Paused:" : "Playing:", g_now_playing);
	} else if (g_have_error) {
		set_status("Error:", g_last_error);
	} else if (g_browsing) {
		set_status("Loading...", current_view_title());
	} else if (!g_settings.configured) {
		set_status("Not configured.", "Tap More for settings.");
	} else {
		set_status("Ready", current_view_title());
	}
}

static void show_error(const char *where, const char *message) {
	clip(g_last_error, sizeof(g_last_error), message ? message : "Unknown error.", 96);
	g_have_error = true;
	morpthis_loge("subsonic", "%s: %s", where, g_last_error);
	morpthis_toast(g_last_error);
	refresh_status();
}

// ---------------------------------------------------------------------------
// Browsing
// ---------------------------------------------------------------------------

static void request_view(void) {
	if (g_depth <= 0 || !g_list) {
		return;
	}
	const view_t *view = &g_views[g_depth - 1];
	g_view_generation++;
	g_browsing = true;
	g_have_error = false;
	g_entry_count = 0;
	morpthis_list_clear(g_list);
	refresh_status();
	browse_worker_request((int)g_view_generation, view->kind, view->id, 0);
}

static void format_entry(const subsonic_entry_t *entry, char *out, size_t size);

static void render_entries(void) {
	if (!g_list) {
		return;
	}
	morpthis_list_clear(g_list);
	for (int i = 0; i < g_entry_count; i++) {
		char text[384];
		format_entry(&g_entries[i], text, sizeof(text));
		morpthis_list_add(g_list, text);
	}
}

static void show_root(void) {
	g_depth = 0;
	g_root_fallback_done = false;
	g_browsing = false;
	g_have_error = false;
	g_entry_count = ROOT_COUNT;
	for (int i = 0; i < ROOT_COUNT; i++) {
		memset(&g_entries[i], 0, sizeof(g_entries[i]));
		g_entries[i].kind = SUBSONIC_MENU;
		snprintf(g_entries[i].id, sizeof(g_entries[i].id), "%d", i);
		snprintf(g_entries[i].title, sizeof(g_entries[i].title), "%s", ROOT_ITEMS[i].title);
	}
	render_entries();
	refresh_status();
}

static void show_not_configured(void) {
	g_depth = 0;
	g_entry_count = 0;
	g_browsing = false;
	g_have_error = false;
	if (g_list) {
		morpthis_list_clear(g_list);
	}
	refresh_status();
}

static void format_entry(const subsonic_entry_t *entry, char *out, size_t size) {
	if (entry->kind == SUBSONIC_TRACK && entry->artist[0] && strcmp(entry->artist, entry->title) != 0) {
		snprintf(out, size, "%s%s - %s", entry->starred ? "* " : "", entry->title, entry->artist);
	} else {
		snprintf(out, size, "%s%s", entry->starred ? "* " : "", entry->title);
	}
}

static void on_browse_done(const browse_result_t *result, void *user) {
	(void)user;
	if (g_shutting_down || !result || (unsigned)result->generation != g_view_generation) {
		return;
	}
	g_browsing = false;

	if (result->error[0]) {
		show_error("browse", result->error);
		return;
	}

	g_entry_count = result->count > MAX_ENTRIES ? MAX_ENTRIES : result->count;
	if (g_entry_count > 0) {
		memcpy(g_entries, result->entries, (size_t)g_entry_count * sizeof(g_entries[0]));
	}

	morpthis_list_clear(g_list);
	for (int i = 0; i < g_entry_count; i++) {
		char text[384];
		format_entry(&g_entries[i], text, sizeof(text));
		morpthis_list_add(g_list, text);
	}

	if (g_entry_count == 0 && g_depth == 1 &&
		(g_views[0].kind == SUBSONIC_BROWSE_ARTISTS || g_views[0].kind == SUBSONIC_BROWSE_FOLDERS)) {
		if (g_root_fallback_done) {
			show_error("browse", "The server returned an empty library.");
			return;
		}
		g_root_fallback_done = true;
		g_views[0].kind = g_views[0].kind == SUBSONIC_BROWSE_ARTISTS ? SUBSONIC_BROWSE_FOLDERS
															 : SUBSONIC_BROWSE_ARTISTS;
		snprintf(g_views[0].title, sizeof(g_views[0].title), "%s",
				 g_views[0].kind == SUBSONIC_BROWSE_FOLDERS ? "Music folders" : "Artists");
		request_view();
		return;
	}

	g_have_error = false;
	if (result->more) {
		morpthis_toast("Showing the first 200 entries.");
	}
	refresh_status();
}

static void open_entry(const subsonic_entry_t *entry) {
	subsonic_browse_t kind;
	switch (entry->kind) {
	case SUBSONIC_MUSIC_FOLDER:
		kind = SUBSONIC_BROWSE_INDEXES;
		break;
	case SUBSONIC_DIRECTORY:
		kind = SUBSONIC_BROWSE_DIRECTORY;
		break;
	case SUBSONIC_ARTIST:
		kind = SUBSONIC_BROWSE_ARTIST;
		break;
	case SUBSONIC_ALBUM:
		kind = SUBSONIC_BROWSE_ALBUM;
		break;
	case SUBSONIC_PLAYLIST:
		kind = SUBSONIC_BROWSE_PLAYLIST;
		break;
	case SUBSONIC_GENRE:
		kind = SUBSONIC_BROWSE_GENRE;
		break;
	default:
		return;
	}
	if (g_depth >= MAX_DEPTH) {
		morpthis_toast("Too many levels.");
		return;
	}
	view_t *view = &g_views[g_depth++];
	view->kind = kind;
	snprintf(view->id, sizeof(view->id), "%s", entry->id);
	clip(view->title, sizeof(view->title), entry->title, 60);
	request_view();
}

// ---------------------------------------------------------------------------
// Playback
// ---------------------------------------------------------------------------

static bool track_format(const subsonic_entry_t *entry, subsonic_format_t *out) {
	subsonic_format_t format = g_settings.quality.format;
	if (format == SUBSONIC_FORMAT_OPUS) {
		return false; // no Opus decoder in this app
	}
	if (format == SUBSONIC_FORMAT_RAW && strcasecmp(entry->suffix, "flac") != 0 &&
		strcasecmp(entry->suffix, "mp3") != 0) {
		return false;
	}
	*out = format;
	return true;
}

static bool play_queue_at(int index) {
	if (index < 0 || index >= g_queue_count) {
		return false;
	}
	const subsonic_entry_t *entry = &g_queue[index];
	subsonic_format_t format;
	if (!track_format(entry, &format)) {
		show_error("playback", "This track's format is not supported. Choose MP3 or FLAC in settings.");
		return false;
	}

	stream_track_t track;
	memset(&track, 0, sizeof(track));
	snprintf(track.id, sizeof(track.id), "%s", entry->id);
	snprintf(track.title, sizeof(track.title), "%s", entry->title);
	snprintf(track.artist, sizeof(track.artist), "%s", entry->artist);
	snprintf(track.album, sizeof(track.album), "%s", entry->album);
	snprintf(track.suffix, sizeof(track.suffix), "%s", entry->suffix);
	track.format = format;

	if (!stream_player_start(&track)) {
		show_error("playback", "Could not start the player.");
		return false;
	}

	g_playing = true;
	g_paused = false;
	g_have_error = false;
	g_queue_index = index;
	snprintf(g_now_playing, sizeof(g_now_playing), "%s", entry->title);
	if (g_queue_screen && morpthis_screen_active() == g_queue_screen) {
		update_queue_screen();
	}
	refresh_status();
	return true;
}

static bool play_entry(int index) {
	if (index < 0 || index >= g_entry_count || g_entries[index].kind != SUBSONIC_TRACK) {
		return false;
	}

	// The queue is every track in the list being looked at, starting with the
	// one tapped. It stays put when the user browses elsewhere.
	g_queue_count = 0;
	g_queue_index = -1;
	int target = -1;
	for (int i = 0; i < g_entry_count && g_queue_count < MAX_ENTRIES; i++) {
		if (g_entries[i].kind != SUBSONIC_TRACK) {
			continue;
		}
		g_queue[g_queue_count] = g_entries[i];
		if (i == index) {
			target = g_queue_count;
		}
		g_queue_count++;
	}
	if (target < 0) {
		return false;
	}
	return play_queue_at(target);
}

static void play_relative(int delta) {
	if (g_queue_count == 0) {
		return;
	}
	int from = g_queue_index < 0 ? (delta > 0 ? -1 : g_queue_count) : g_queue_index;
	for (int i = from + delta; i >= 0 && i < g_queue_count; i += delta) {
		if (play_queue_at(i)) {
			return;
		}
	}
	morpthis_toast(delta > 0 ? "End of the queue." : "Start of the queue.");
}

static void on_stream_event(stream_event_kind_t kind, const char *message,
							const stream_track_t *track, void *user) {
	(void)user;
	if (g_shutting_down) {
		return;
	}
	if (kind == STREAM_EVENT_STARTED) {
		if (track) {
			enqueue_scrobble(track->id, false);
		}
		return;
	}
	if (kind == STREAM_EVENT_LISTENED) {
		if (track) {
			enqueue_scrobble(track->id, true);
		}
		return;
	}
	if (kind == STREAM_EVENT_FINISHED) {
		g_playing = false;
		g_paused = false;
		if (g_queue_index >= 0 && play_queue_at(g_queue_index + 1)) {
			return;
		}
		refresh_status();
		return;
	}
	g_playing = false;
	g_paused = false;
	show_error("playback", message[0] ? message : "Playback failed.");
}

static void stop_playback(morpthis_widget_t *widget, void *user) {
	(void)widget;
	(void)user;
	if (!g_playing) {
		morpthis_toast("Nothing is playing.");
		return;
	}
	stream_player_stop();
	g_playing = false;
	g_paused = false;
	g_have_error = false;
	refresh_status();
}

// ---------------------------------------------------------------------------
// Star/unstar and scrobbles
//
// One request at a time on its own thread: the API call can sit in connect or
// TLS for seconds, and neither the GUI thread nor the thread feeding the DAC
// may wait for it. Results come back through morpthis_app_post, like every
// other worker in this app.
// ---------------------------------------------------------------------------

typedef struct {
	pthread_mutex_t lock;
	pthread_cond_t cond;
	pthread_t thread;
	bool started;
	bool quit;
	bool pending;
	bool scrobble;
	bool submission;
	subsonic_entry_kind_t kind;
	char id[SUBSONIC_ID_MAX];
	bool on;
} star_worker_t;

static star_worker_t g_star;

typedef struct {
	char id[SUBSONIC_ID_MAX];
	bool on;
	bool ok;
	char error[192];
} star_result_t;

static void star_done(void *user) {
	star_result_t *result = user;
	if (!result) {
		return;
	}
	if (g_shutting_down) {
		free(result);
		return;
	}
	if (!result->ok) {
		morpthis_toast(result->error[0] ? result->error : "Could not update the star.");
		free(result);
		return;
	}
	for (int i = 0; i < g_queue_count; i++) {
		if (strcmp(g_queue[i].id, result->id) == 0) {
			g_queue[i].starred = result->on;
		}
	}
	for (int i = 0; i < g_entry_count; i++) {
		if (strcmp(g_entries[i].id, result->id) == 0) {
			g_entries[i].starred = result->on;
		}
	}
	render_entries();
	update_more_screen();
	morpthis_toast(result->on ? "Starred." : "Unstarred.");
	free(result);
}

static void *star_main(void *unused) {
	(void)unused;
	for (;;) {
		pthread_mutex_lock(&g_star.lock);
		while (!g_star.quit && !g_star.pending) {
			pthread_cond_wait(&g_star.cond, &g_star.lock);
		}
		if (g_star.quit) {
			pthread_mutex_unlock(&g_star.lock);
			break;
		}
		subsonic_entry_kind_t kind = g_star.kind;
		char id[SUBSONIC_ID_MAX];
		snprintf(id, sizeof(id), "%s", g_star.id);
		bool on = g_star.on;
		bool scrobble = g_star.scrobble;
		bool submission = g_star.submission;
		g_star.pending = false;
		pthread_mutex_unlock(&g_star.lock);

		if (scrobble) {
			char error[192] = "";
			if (!subsonic_scrobble(id, submission, error, sizeof(error))) {
				morpthis_logd("subsonic", "scrobble failed: %s", error);
			}
			continue;
		}

		star_result_t *result = calloc(1, sizeof(*result));
		if (!result) {
			continue;
		}
		snprintf(result->id, sizeof(result->id), "%s", id);
		result->on = on;
		result->ok = subsonic_star(kind, id, on, result->error, sizeof(result->error));
		if (!result->ok) {
			morpthis_logw("subsonic", "star failed: %s", result->error);
		}
		pthread_mutex_lock(&g_star.lock);
		bool quit = g_star.quit;
		pthread_mutex_unlock(&g_star.lock);
		if (quit) {
			free(result);
			break;
		}
		morpthis_app_post(star_done, result);
	}
	return NULL;
}

static bool star_worker_init(void) {
	memset(&g_star, 0, sizeof(g_star));
	pthread_mutex_init(&g_star.lock, NULL);
	pthread_cond_init(&g_star.cond, NULL);
	int rc = pthread_create(&g_star.thread, NULL, star_main, NULL);
	g_star.started = rc == 0;
	return g_star.started;
}

static void star_worker_shutdown(void) {
	if (!g_star.started) {
		return;
	}
	pthread_mutex_lock(&g_star.lock);
	g_star.quit = true;
	pthread_cond_broadcast(&g_star.cond);
	pthread_mutex_unlock(&g_star.lock);
	subsonic_abort();
	pthread_join(g_star.thread, NULL);
	g_star.started = false;
}

static void toggle_star(morpthis_widget_t *widget, void *user) {
	(void)widget;
	(void)user;
	if (g_queue_index < 0 || g_queue_index >= g_queue_count) {
		morpthis_toast("Nothing is playing.");
		return;
	}
	if (!g_star.started) {
		return;
	}
	subsonic_entry_t *entry = &g_queue[g_queue_index];
	pthread_mutex_lock(&g_star.lock);
	if (g_star.pending) {
		pthread_mutex_unlock(&g_star.lock);
		return;
	}
	g_star.pending = true;
	g_star.scrobble = false;
	g_star.kind = entry->kind;
	g_star.on = !entry->starred;
	snprintf(g_star.id, sizeof(g_star.id), "%s", entry->id);
	pthread_cond_signal(&g_star.cond);
	pthread_mutex_unlock(&g_star.lock);
}

// The playback thread posts STARTED/LISTENED; the action worker does the
// request. A pending star is not overwritten by a scrobble: the next one is
// dropped instead.
static void enqueue_scrobble(const char *id, bool submission) {
	if (!g_star.started || !id || !id[0]) {
		return;
	}
	pthread_mutex_lock(&g_star.lock);
	if (g_star.pending) {
		pthread_mutex_unlock(&g_star.lock);
		return;
	}
	g_star.pending = true;
	g_star.scrobble = true;
	g_star.submission = submission;
	snprintf(g_star.id, sizeof(g_star.id), "%s", id);
	pthread_cond_signal(&g_star.cond);
	pthread_mutex_unlock(&g_star.lock);
}

// ---------------------------------------------------------------------------
// More, queue and search
// ---------------------------------------------------------------------------

static void update_more_screen(void) {
	if (!g_more_star) {
		return;
	}
	bool have = g_queue_index >= 0 && g_queue_index < g_queue_count;
	morpthis_widget_set_enabled(g_more_star, have);
	if (!have) {
		morpthis_widget_set_text(g_more_star, "Star current");
		return;
	}
	morpthis_widget_set_text(g_more_star,
						 g_queue[g_queue_index].starred ? "Unstar current" : "Star current");
}

static void update_queue_screen(void) {
	if (!g_queue_list) {
		return;
	}
	morpthis_list_clear(g_queue_list);
	if (g_queue_count == 0) {
		morpthis_list_add(g_queue_list, "The queue is empty.");
		return;
	}
	for (int i = 0; i < g_queue_count; i++) {
		char text[384], line[400];
		format_entry(&g_queue[i], text, sizeof(text));
		snprintf(line, sizeof(line), "%s%s", i == g_queue_index ? "> " : "", text);
		morpthis_list_add(g_queue_list, line);
	}
}

static void queue_selected(morpthis_widget_t *list, int index, void *user) {
	(void)list;
	(void)user;
	if (index >= 0 && index < g_queue_count) {
		play_queue_at(index);
		update_queue_screen();
	}
}

static void queue_clear(morpthis_widget_t *widget, void *user) {
	(void)widget;
	(void)user;
	stream_player_stop();
	g_queue_count = 0;
	g_queue_index = -1;
	g_playing = false;
	g_paused = false;
	update_queue_screen();
	refresh_status();
}

static void queue_back(morpthis_widget_t *widget, void *user) {
	(void)widget;
	(void)user;
	morpthis_screen_load(g_main_screen);
	refresh_status();
}

static void open_more(morpthis_widget_t *widget, void *user) {
	(void)widget;
	(void)user;
	update_more_screen();
	morpthis_screen_load(g_more_screen);
}

static void open_queue(morpthis_widget_t *widget, void *user) {
	(void)widget;
	(void)user;
	update_queue_screen();
	morpthis_screen_load(g_queue_screen);
}

static void update_search_screen(void) {
	if (!g_search_label) {
		return;
	}
	char text[96];
	snprintf(text, sizeof(text), "Search: %s_", g_search_query);
	morpthis_widget_set_text(g_search_label, text);
}

static void search_go(void) {
	if (!g_search_query[0]) {
		morpthis_toast("Type something first.");
		return;
	}
	if (g_depth >= MAX_DEPTH) {
		morpthis_toast("Too many levels.");
		return;
	}
	g_depth = 0;
	view_t *view = &g_views[g_depth++];
	view->kind = SUBSONIC_BROWSE_SEARCH;
	snprintf(view->id, sizeof(view->id), "%s", g_search_query);
	snprintf(view->title, sizeof(view->title), "Search: %s", g_search_query);
	morpthis_screen_load(g_main_screen);
	request_view();
}

static void search_key(morpthis_widget_t *widget, void *user) {
	(void)widget;
	const char *key = user;
	if (!key) {
		return;
	}
	size_t len = strlen(g_search_query);
	if (strcmp(key, "ESC") == 0) {
		morpthis_screen_load(g_main_screen);
		refresh_status();
		return;
	} else if (strcmp(key, "GO") == 0) {
		search_go();
		return;
	} else if (strcmp(key, "DEL") == 0) {
		if (len > 0) {
			g_search_query[len - 1] = '\0';
		}
	} else if (strcmp(key, "SP") == 0) {
		if (len + 1 < sizeof(g_search_query)) {
			g_search_query[len] = ' ';
			g_search_query[len + 1] = '\0';
		}
	} else if (len + 1 < sizeof(g_search_query)) {
		g_search_query[len] = key[0];
		g_search_query[len + 1] = '\0';
	}
	update_search_screen();
}

static void open_search(morpthis_widget_t *widget, void *user) {
	(void)widget;
	(void)user;
	g_search_query[0] = '\0';
	update_search_screen();
	morpthis_screen_load(g_search_screen);
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

static bool apply_settings(char *error, size_t error_size) {
	tls_set_card_root(morpthis_fs_card_root());
	tls_set_verify_mode(g_settings.tls_relaxed ? TLS_VERIFY_RELAXED : TLS_VERIFY_STRICT);
	tls_set_ca_bundle(g_settings.ca_bundle);
	if (g_settings.quality.format == SUBSONIC_FORMAT_FLAC) {
		morpthis_logi("subsonic", "TLS %s, FLAC lossless", g_settings.tls_relaxed ? "relaxed" : "strict");
	} else if (g_settings.quality.format == SUBSONIC_FORMAT_RAW) {
		morpthis_logi("subsonic", "TLS %s, original (raw)", g_settings.tls_relaxed ? "relaxed" : "strict");
	} else {
		morpthis_logi("subsonic", "TLS %s, %d kbps MP3", g_settings.tls_relaxed ? "relaxed" : "strict",
					  g_settings.quality.max_bitrate);
	}

	if (!g_settings.configured) {
		subsonic_disconnect();
		return true;
	}
	return subsonic_configure(&g_settings.credentials, g_settings.quality, error, error_size);
}

static void update_settings_screen(void) {
	if (!g_settings_label) {
		return;
	}
	char server[80];
	clip(server, sizeof(server), g_settings.credentials.server[0] ? g_settings.credentials.server
																  : "Not configured.",
		 60);
	char text[197];
	snprintf(text, sizeof(text), "merp_data/subsonic/settings.json\n%s", server);
	morpthis_widget_set_text(g_settings_label, text);

	morpthis_widget_set_text(g_tls_button, g_settings.tls_relaxed ? "TLS: relaxed" : "TLS: strict");
	char quality[32];
	if (g_settings.quality.format == SUBSONIC_FORMAT_FLAC) {
		snprintf(quality, sizeof(quality), "Quality: FLAC (lossless)");
	} else if (g_settings.quality.format == SUBSONIC_FORMAT_RAW) {
		snprintf(quality, sizeof(quality), "Quality: original (raw)");
	} else {
		snprintf(quality, sizeof(quality), "Quality: %d kbps MP3", g_settings.quality.max_bitrate);
	}
	morpthis_widget_set_text(g_quality_button, quality);
}

static void toggle_tls(morpthis_widget_t *widget, void *user) {
	(void)widget;
	(void)user;
	if (!g_settings_valid) {
		morpthis_toast("Fix settings.json first.");
		return;
	}
	g_settings.tls_relaxed = !g_settings.tls_relaxed;
	if (!settings_save(&g_settings)) {
		morpthis_toast("Could not save settings.json.");
	}
	char error[192];
	if (!apply_settings(error, sizeof(error))) {
		morpthis_toast(error);
	}
	update_settings_screen();
	morpthis_toast(g_settings.tls_relaxed ? "Self-signed certificates are accepted." : "Strict TLS checking.");
}

static void toggle_quality(morpthis_widget_t *widget, void *user) {
	(void)widget;
	(void)user;
	if (!g_settings_valid) {
		morpthis_toast("Fix settings.json first.");
		return;
	}
	if (g_settings.quality.format == SUBSONIC_FORMAT_MP3 && g_settings.quality.max_bitrate < 320) {
		g_settings.quality.max_bitrate = 320;
	} else if (g_settings.quality.format == SUBSONIC_FORMAT_MP3) {
		g_settings.quality.format = SUBSONIC_FORMAT_FLAC;
	} else {
		g_settings.quality.format = SUBSONIC_FORMAT_MP3;
		g_settings.quality.max_bitrate = 256;
	}
	subsonic_set_quality(g_settings.quality);
	if (!settings_save(&g_settings)) {
		morpthis_toast("Could not save settings.json.");
	}
	update_settings_screen();
	if (g_settings.quality.format == SUBSONIC_FORMAT_FLAC) {
		morpthis_toast("Next track: FLAC lossless (server transcoding).");
	} else {
		char text[48];
		snprintf(text, sizeof(text), "Next track: %d kbps MP3.", g_settings.quality.max_bitrate);
		morpthis_toast(text);
	}
}

static void reload_settings(morpthis_widget_t *widget, void *user) {
	(void)widget;
	(void)user;
	subsonic_settings_t next;
	char error[192] = "";
	if (!settings_load(&next, error, sizeof(error))) {
		g_settings_valid = false;
		morpthis_toast(error);
		return;
	}
	g_settings = next;
	g_settings_valid = true;
	if (!apply_settings(error, sizeof(error))) {
		morpthis_toast(error);
		return;
	}
	update_settings_screen();
	morpthis_screen_load(g_main_screen);
	if (g_settings.configured) {
		show_root();
	} else {
		show_not_configured();
	}
}

static void back_to_main(morpthis_widget_t *widget, void *user) {
	(void)widget;
	(void)user;
	morpthis_screen_load(g_main_screen);
	refresh_status();
}

static void open_settings(morpthis_widget_t *widget, void *user) {
	(void)widget;
	(void)user;
	update_settings_screen();
	morpthis_screen_load(g_settings_screen);
}

// ---------------------------------------------------------------------------
// Main screen buttons and keys
// ---------------------------------------------------------------------------

static void go_up(morpthis_widget_t *widget, void *user) {
	(void)widget;
	(void)user;
	if (g_depth > 1) {
		g_depth--;
		request_view();
	} else if (g_depth == 1) {
		g_depth = 0;
		show_root();
	} else {
		morpthis_toast("Top of the library.");
	}
}

static void open_search(morpthis_widget_t *widget, void *user);

static void on_select(morpthis_widget_t *list, int index, void *user) {
	(void)list;
	(void)user;
	if (index < 0 || index >= g_entry_count) {
		return;
	}
	if (g_entries[index].kind == SUBSONIC_MENU) {
		int which = atoi(g_entries[index].id);
		if (which < 0 || which >= ROOT_COUNT) {
			return;
		}
		if (ROOT_ITEMS[which].browse == SUBSONIC_BROWSE_SEARCH) {
			open_search(NULL, NULL);
			return;
		}
		if (g_depth >= MAX_DEPTH) {
			morpthis_toast("Too many levels.");
			return;
		}
		view_t *view = &g_views[g_depth++];
		view->kind = ROOT_ITEMS[which].browse;
		view->id[0] = '\0';
		snprintf(view->title, sizeof(view->title), "%s", ROOT_ITEMS[which].title);
		request_view();
		return;
	}
	if (g_entries[index].kind == SUBSONIC_TRACK) {
		play_entry(index);
	} else {
		open_entry(&g_entries[index]);
	}
}

static void on_button(morpthis_button_t button, morpthis_input_action_t action, void *user) {
	(void)user;
	if (action != MORPTHIS_INPUT_PRESS) {
		return;
	}
	switch (button) {
	case MORPTHIS_BUTTON_PLAY_PAUSE:
		if (!g_playing) {
			if (g_queue_count > 0) {
				play_queue_at(g_queue_index >= 0 ? g_queue_index : 0);
			}
		} else {
			g_paused = !g_paused;
			stream_player_pause(g_paused);
			refresh_status();
		}
		break;
	case MORPTHIS_BUTTON_NEXT:
		play_relative(1);
		break;
	case MORPTHIS_BUTTON_PREV:
		play_relative(-1);
		break;
	case MORPTHIS_BUTTON_BACK: {
		morpthis_screen_t *active = morpthis_screen_active();
		if (active && active == g_search_screen) {
			morpthis_screen_load(g_main_screen);
			refresh_status();
		} else if (active && active == g_settings_screen) {
			open_more(NULL, NULL);
		} else if (active && active == g_queue_screen) {
			back_to_main(NULL, NULL);
		} else if (active && active == g_more_screen) {
			back_to_main(NULL, NULL);
		} else {
			// The player handles its own chrome, but the host runner and a player
			// that delivers BACK before quitting both need this to be the way out.
			morpthis_app_quit();
		}
		break;
	}
	default:
		break;
	}
}

// ---------------------------------------------------------------------------
// Screens
// ---------------------------------------------------------------------------

static bool create_main_screen(void) {
	g_main_screen = morpthis_screen_create("Subsonic");
	if (!g_main_screen) {
		return false;
	}

	g_status = morpthis_label_create(g_main_screen, "Starting...");
	morpthis_widget_set_pos(g_status, 0, 0);
	morpthis_widget_set_size(g_status, g_width, 54);

	g_list = morpthis_list_create(g_main_screen);
	morpthis_widget_set_pos(g_list, 0, 58);
	morpthis_widget_set_size(g_list, g_width, g_height - 58 - 46);
	morpthis_list_set_select_cb(g_list, on_select, NULL);

	int width = (g_width - 12) / 3;
	if (width < 50) {
		return false;
	}
	int y = g_height - 40;

	morpthis_widget_t *up = morpthis_button_create(g_main_screen, "Up", go_up, NULL);
	morpthis_widget_set_pos(up, 0, y);
	morpthis_widget_set_size(up, width, 36);

	g_stop_button = morpthis_button_create(g_main_screen, "Stop", stop_playback, NULL);
	morpthis_widget_set_pos(g_stop_button, width + 6, y);
	morpthis_widget_set_size(g_stop_button, width, 36);

	morpthis_widget_t *more = morpthis_button_create(g_main_screen, "More", open_more, NULL);
	morpthis_widget_set_pos(more, (width + 6) * 2, y);
	morpthis_widget_set_size(more, width, 36);

	return true;
}

static bool create_settings_screen(void) {
	g_settings_screen = morpthis_screen_create("Subsonic settings");
	if (!g_settings_screen) {
		return false;
	}

	g_settings_label = morpthis_label_create(g_settings_screen, "merp_data/subsonic/settings.json");
	morpthis_widget_set_pos(g_settings_label, 0, 0);
	morpthis_widget_set_size(g_settings_label, g_width, 96);

	int button_width = g_width - 40;
	if (button_width < 80) {
		return false;
	}

	g_tls_button = morpthis_button_create(g_settings_screen, "TLS: strict", toggle_tls, NULL);
	morpthis_widget_set_pos(g_tls_button, 20, 96);
	morpthis_widget_set_size(g_tls_button, button_width, 34);

	g_quality_button = morpthis_button_create(g_settings_screen, "Quality: 256 kbps", toggle_quality, NULL);
	morpthis_widget_set_pos(g_quality_button, 20, 132);
	morpthis_widget_set_size(g_quality_button, button_width, 34);

	morpthis_widget_t *reload = morpthis_button_create(g_settings_screen, "Reload settings",
												   reload_settings, NULL);
	morpthis_widget_set_pos(reload, 20, 168);
	morpthis_widget_set_size(reload, button_width, 34);

	morpthis_widget_t *back = morpthis_button_create(g_settings_screen, "Back", open_more, NULL);
	morpthis_widget_set_pos(back, 20, 204);
	morpthis_widget_set_size(back, button_width, 34);

	return true;
}

static bool create_more_screen(void) {
	g_more_screen = morpthis_screen_create("Subsonic");
	if (!g_more_screen) {
		return false;
	}
	int width = g_width - 40;
	if (width < 80) {
		return false;
	}

	morpthis_widget_t *queue = morpthis_button_create(g_more_screen, "Queue", open_queue, NULL);
	morpthis_widget_set_pos(queue, 20, 0);
	morpthis_widget_set_size(queue, width, 36);

	g_more_star = morpthis_button_create(g_more_screen, "Star current", toggle_star, NULL);
	morpthis_widget_set_pos(g_more_star, 20, 40);
	morpthis_widget_set_size(g_more_star, width, 36);

	morpthis_widget_t *settings = morpthis_button_create(g_more_screen, "Settings", open_settings, NULL);
	morpthis_widget_set_pos(settings, 20, 80);
	morpthis_widget_set_size(settings, width, 36);

	morpthis_widget_t *back = morpthis_button_create(g_more_screen, "Back", back_to_main, NULL);
	morpthis_widget_set_pos(back, 20, 120);
	morpthis_widget_set_size(back, width, 36);

	return true;
}

static bool create_queue_screen(void) {
	g_queue_screen = morpthis_screen_create("Queue");
	if (!g_queue_screen) {
		return false;
	}

	g_queue_list = morpthis_list_create(g_queue_screen);
	morpthis_widget_set_pos(g_queue_list, 0, 0);
	morpthis_widget_set_size(g_queue_list, g_width, g_height - 46);
	morpthis_list_set_select_cb(g_queue_list, queue_selected, NULL);

	int width = (g_width - 12) / 2;
	if (width < 50) {
		return false;
	}
	int y = g_height - 40;

	morpthis_widget_t *clear = morpthis_button_create(g_queue_screen, "Clear", queue_clear, NULL);
	morpthis_widget_set_pos(clear, 0, y);
	morpthis_widget_set_size(clear, width, 36);

	morpthis_widget_t *back = morpthis_button_create(g_queue_screen, "Back", queue_back, NULL);
	morpthis_widget_set_pos(back, width + 6, y);
	morpthis_widget_set_size(back, width, 36);

	return true;
}

static bool create_search_screen(void) {
	g_search_screen = morpthis_screen_create("Search");
	if (!g_search_screen) {
		return false;
	}

	g_search_label = morpthis_label_create(g_search_screen, "Search: _");
	morpthis_widget_set_pos(g_search_label, 0, 0);
	morpthis_widget_set_size(g_search_label, g_width, 36);

	static const char *const keys[] = {
		"A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
		"N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z",
		"SP", "DEL", "ESC", "GO",
	};

	int columns = 5;
	int cell_w = g_width / columns;
	int cell_h = 36;
	int start_y = 40;
	for (int i = 0; i < (int)(sizeof(keys) / sizeof(keys[0])); i++) {
		int row = i / columns;
		int col = i % columns;
		morpthis_widget_t *button = morpthis_button_create(g_search_screen, keys[i], search_key,
													(void *)keys[i]);
		morpthis_widget_set_pos(button, col * cell_w, start_y + row * cell_h);
		morpthis_widget_set_size(button, cell_w - 2, cell_h - 2);
	}

	return true;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

static void load_settings_and_start(void) {
	char error[192] = "";
	if (!settings_load(&g_settings, error, sizeof(error))) {
		g_settings_valid = false;
		update_settings_screen();
		show_error("settings", error);
		return;
	}
	g_settings_valid = true;
	if (!apply_settings(error, sizeof(error))) {
		update_settings_screen();
		show_error("settings", error);
		return;
	}
	update_settings_screen();
	if (g_settings.configured) {
		show_root();
	} else {
		show_not_configured();
	}
}

int morpthis_app_main(const morpthis_app_t *app) {
	// The player already ignores SIGPIPE; the host runtime does not, and a TLS
	// write to a server that has gone away must not end the process.
	signal(SIGPIPE, SIG_IGN);

	morpthis_logi("subsonic", "starting, api %d", morpthis_app_api_version(app));
	g_width = morpthis_ui_width();
	g_height = morpthis_ui_height();
	if (g_width < 200 || g_height < 240) {
		morpthis_loge("subsonic", "the display is too small (%dx%d)", g_width, g_height);
		return 1;
	}

	subsonic_client_init();
	settings_defaults(&g_settings);
	tls_set_card_root(morpthis_fs_card_root());

	if (!create_main_screen() || !create_settings_screen() || !create_more_screen() ||
		!create_queue_screen() || !create_search_screen()) {
		morpthis_loge("subsonic", "could not create a screen");
		return 1;
	}
	if (!stream_player_init(on_stream_event, NULL)) {
		return 1;
	}
	if (!browse_worker_init(on_browse_done, NULL)) {
		stream_player_shutdown();
		return 1;
	}
	if (!star_worker_init()) {
		stream_player_shutdown();
		browse_worker_shutdown();
		return 1;
	}

	morpthis_input_set_button_cb(on_button, NULL);
	morpthis_screen_load(g_main_screen);
	load_settings_and_start();
	morpthis_logi("subsonic", "ready");
	return 0;
}

// The player calls this before unloading the shared object, including when USB
// mass-storage mode closes the app to share the card. No UI calls, and it must
// be idempotent: the worker threads stop and are joined while their code is
// still mapped.
MORPTHIS_EXPORT void morpthis_app_shutdown(void) {
	if (g_shutting_down) {
		return;
	}
	g_shutting_down = true;
	stream_player_shutdown();
	browse_worker_shutdown();
	star_worker_shutdown();
	morpthis_logi("subsonic", "stopped");
}
