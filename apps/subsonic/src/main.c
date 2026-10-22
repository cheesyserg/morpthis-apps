// Subsonic as a Morpthis app.
//
// The test this app exists for: can a network music client live in a package
// on the card instead of inside the player? It reaches the DAC through
// morpthis_audio_open/write, does its own HTTP and TLS, and carries dr_mp3 and
// dr_flac.
//
// API 3 gives the app the player's own settings rows and keyboard. The account
// is still edited in Apps/merp_data/subsonic/settings.json; the settings page
// exposes the values that can change from the device (TLS mode and stream
// quality) as the player's popover menus. Search uses the player's keyboard.

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
#include "cover_worker.h"
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
static int g_full_height; // a screen without the title row, for the now-playing page

static morpthis_screen_t *g_main_screen;
static morpthis_screen_t *g_settings_screen;
static morpthis_screen_t *g_more_screen;
static morpthis_screen_t *g_queue_screen;
static morpthis_screen_t *g_now_screen;

// The app's own page stack. The player draws the swipe and the chevron, but it
// cannot know that More was opened from the now-playing page rather than from
// the library; this is what makes BACK retrace the real route.
#define NAV_MAX 12
static morpthis_screen_t *g_nav[NAV_MAX];
static int g_nav_depth;

static void nav_clear(void) { g_nav_depth = 0; }

static void navigate_to(morpthis_screen_t *target) {
	morpthis_screen_t *active = morpthis_screen_active();
	if (active && active != target) {
		if (g_nav_depth > 0 && g_nav[g_nav_depth - 1] == active) {
			// Already recorded; a re-entry must not grow the stack.
		} else if (g_nav_depth < NAV_MAX) {
			g_nav[g_nav_depth++] = active;
		} else {
			// A very long session must not wedge: drop the oldest step. The
			// pages are all still built, so the pointer stays valid.
			memmove(&g_nav[0], &g_nav[1], sizeof(g_nav[0]) * (NAV_MAX - 1));
			g_nav[NAV_MAX - 1] = active;
		}
	}
	morpthis_screen_load(target);
}

static bool navigate_back(void) {
	while (g_nav_depth > 0) {
		morpthis_screen_t *target = g_nav[--g_nav_depth];
		if (target && target != morpthis_screen_active()) {
			morpthis_screen_load(target);
			return true;
		}
	}
	return false;
}

static morpthis_widget_t *g_status;
static morpthis_widget_t *g_list;
static morpthis_widget_t *g_queue_list;
static morpthis_widget_t *g_more_list;

// The mini-player row at the top of the library and the full now-playing page.
static morpthis_widget_t *g_now_bar;
static morpthis_widget_t *g_now_cover;
static morpthis_widget_t *g_now_title;
static morpthis_widget_t *g_now_artist;
static morpthis_widget_t *g_now_format;
static morpthis_widget_t *g_now_progress;
static morpthis_widget_t *g_now_elapsed;
static morpthis_widget_t *g_now_total;
static morpthis_widget_t *g_now_queue_pos;
static morpthis_widget_t *g_now_play;
static morpthis_icon_t g_now_play_kind; // the glyph already on g_now_play
static bool g_seeking;               // a finger is on the progress bar
static bool g_updating_slider;       // a programmatic set, not a drag
static bool g_now_seekable = true;   // the bar's current enabled state
static char g_now_artist_text[128];
static char g_now_format_text[32];
static int g_now_duration_secs;
static char g_now_cover_id[SUBSONIC_ID_MAX];
static char g_now_cover_path[32];
static unsigned g_now_cover_applied; // the generation whose file is on the page
static unsigned g_cover_generation;

static morpthis_widget_t *g_settings_container;
static morpthis_widget_t *g_settings_tls;
static morpthis_widget_t *g_settings_quality;
static morpthis_widget_t *g_settings_server;
static morpthis_widget_t *g_settings_account;

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
static void update_now_bar(void);
static void update_now_screen(void);
static void show_now_playing(morpthis_widget_t *widget, void *user);
static void enqueue_scrobble(const char *id, bool submission);
static void open_settings(morpthis_widget_t *widget, void *user);
static void open_more(morpthis_widget_t *widget, void *user);
static void open_queue(morpthis_widget_t *widget, void *user);
static void open_search(morpthis_widget_t *widget, void *user);

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
	char a[80], b[80], text[168];
	clip(a, sizeof(a), first, 64);
	clip(b, sizeof(b), second, 64);
	if (b[0]) {
		snprintf(text, sizeof(text), "%s %s", a, b);
	} else {
		snprintf(text, sizeof(text), "%s", a);
	}
	if (g_status) {
		morpthis_widget_set_text(g_status, text);
	}
}

// The now page is refreshed once a second; a label whose text has not moved
// must not be freed and rebuilt for that. The player's label setter allocates
// the new string every time, so compare first.
static void set_text_if_changed(morpthis_widget_t *widget, const char *text) {
	if (!widget) {
		return;
	}
	const char *current = morpthis_widget_text(widget);
	if (!current || !text || strcmp(current, text) != 0) {
		morpthis_widget_set_text(widget, text ? text : "");
	}
}

static const char *entry_noun(void) {
	if (g_entry_count <= 0) {
		return "items";
	}
	subsonic_entry_kind_t kind = g_entries[0].kind;
	for (int i = 1; i < g_entry_count; i++) {
		if (g_entries[i].kind != kind) {
			return "items";
		}
	}
	switch (kind) {
	case SUBSONIC_TRACK:
		return "tracks";
	case SUBSONIC_ALBUM:
		return "albums";
	case SUBSONIC_ARTIST:
		return "artists";
	case SUBSONIC_PLAYLIST:
		return "playlists";
	case SUBSONIC_GENRE:
		return "genres";
	case SUBSONIC_DIRECTORY:
	case SUBSONIC_MUSIC_FOLDER:
		return "folders";
	default:
		return "items";
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
		set_status("Not configured.", "Open More for settings.");
	} else if (g_depth > 0 && g_entry_count > 0) {
		char count[32];
		snprintf(count, sizeof(count), "%d %s", g_entry_count, entry_noun());
		set_status(count, NULL);
	} else {
		set_status("Choose a section.", NULL);
	}
}

static void show_error(const char *where, const char *message) {
	clip(g_last_error, sizeof(g_last_error), message ? message : "Unknown error.", 96);
	g_have_error = true;
	morpthis_loge("subsonic", "%s: %s", where, g_last_error);
	morpthis_toast_error(g_last_error);
	refresh_status();
}

// ---------------------------------------------------------------------------
// Now playing
// ---------------------------------------------------------------------------

static void format_time(int seconds, char *out, size_t size) {
	if (seconds < 0) {
		seconds = 0;
	}
	snprintf(out, size, "%d:%02d", seconds / 60, seconds % 60);
}

static void update_now_bar(void) {
	if (!g_now_bar || !g_list) {
		return;
	}
	bool show = (g_playing || g_paused) && g_now_playing[0] != '\0';
	if (!show) {
		morpthis_widget_set_visible(g_now_bar, false);
		if (g_status) {
			morpthis_widget_set_visible(g_status, true);
		}
		morpthis_widget_set_pos(g_list, 0, 34);
		morpthis_widget_set_size(g_list, g_width, g_height - 34);
		return;
	}

	morpthis_list_clear(g_now_bar);
	// The mini-player takes the status line's place, so the browse feedback
	// moves into its detail line: a view change must not look like a hang just
	// because a track is playing.
	const char *detail = g_browsing ? "Loading..."
								 : (g_now_artist_text[0] ? g_now_artist_text : NULL);
	morpthis_list_add_entry(g_now_bar, g_now_playing, detail,
							g_now_cover_path[0] ? g_now_cover_path : NULL, true);
	morpthis_list_set_playing(g_now_bar, 0);
	morpthis_widget_set_visible(g_now_bar, true);
	if (g_status) {
		morpthis_widget_set_visible(g_status, false);
	}
	morpthis_widget_set_pos(g_list, 0, 48);
	morpthis_widget_set_size(g_list, g_width, g_height - 48);
}

static void update_now_screen(void) {
	if (!g_now_screen) {
		return;
	}
	bool have = g_now_playing[0] != '\0';
	if (g_now_title) {
		char title[128];
		clip(title, sizeof(title), have ? g_now_playing : "Nothing playing", 28);
		set_text_if_changed(g_now_title, title);
	}
	if (g_now_artist) {
		char artist[128];
		clip(artist, sizeof(artist), have ? g_now_artist_text : "", 18);
		set_text_if_changed(g_now_artist, artist);
	}
	if (g_now_cover) {
		// A track with no cover must not leave the previous one on display.
		bool art = have && g_now_cover_path[0] != '\0';
		morpthis_widget_set_visible(g_now_cover, art);
		// Decoding is synchronous and the page is refreshed once a second while
		// it is open, so only hand the picture over when it is a new download.
		// The file name is the same for every track; the generation is what says
		// its contents changed.
		if (art && g_now_cover_applied != g_cover_generation) {
			morpthis_image_set_source(g_now_cover, g_now_cover_path);
			g_now_cover_applied = g_cover_generation;
		}
	}

	int position = stream_player_position_ms() / 1000;
	int duration = g_now_duration_secs > 0 ? g_now_duration_secs : stream_player_duration_ms() / 1000;
	if (duration > 0 && position > duration) {
		position = duration;
	}
	if (g_now_progress) {
		morpthis_widget_set_visible(g_now_progress, duration > 0);
		// Seeking is only meaningful while a track is loaded: a seek restarts
		// the stream, so a stopped page must not offer it.
		bool seekable = duration > 0 && (g_playing || g_paused);
		if (g_now_seekable != seekable) {
			morpthis_widget_set_enabled(g_now_progress, seekable);
			g_now_seekable = seekable;
		}
		if (duration > 0 && !g_seeking) {
			int value = (int)((long long)position * 1000 / duration);
			g_updating_slider = true;
			morpthis_slider_set_value(g_now_progress, value);
			g_updating_slider = false;
		}
	}
	char elapsed[16];
	format_time(position, elapsed, sizeof(elapsed));
	if (g_now_elapsed && !g_seeking) {
		set_text_if_changed(g_now_elapsed, elapsed);
	}
	if (g_now_total) {
		char total[16];
		format_time(duration, total, sizeof(total));
		set_text_if_changed(g_now_total, duration > 0 ? total : "");
	}
	if (g_now_queue_pos) {
		char place[24];
		if (g_queue_count > 1 && g_queue_index >= 0) {
			snprintf(place, sizeof(place), "%d/%d", g_queue_index + 1, g_queue_count);
		} else {
			place[0] = '\0';
		}
		set_text_if_changed(g_now_queue_pos, place);
	}
	if (g_now_format) {
		set_text_if_changed(g_now_format, have ? g_now_format_text : "");
	}
	if (g_now_play) {
		morpthis_icon_t kind = (g_playing && !g_paused) ? MORPTHIS_ICON_PAUSE : MORPTHIS_ICON_PLAY;
		if (g_now_play_kind != kind) {
			morpthis_widget_set_icon(g_now_play, kind);
			g_now_play_kind = kind;
		}
	}
}

static void show_now_playing(morpthis_widget_t *widget, void *user) {
	(void)widget;
	(void)user;
	if (!g_now_screen) {
		return;
	}
	update_now_screen();
	navigate_to(g_now_screen);
}

// The progress bar maps 0..1000 to the track's length. -1 when the length is
// not known, which is also when the bar is disabled.
static int slider_seconds(int value) {
	int duration = g_now_duration_secs > 0 ? g_now_duration_secs : stream_player_duration_ms() / 1000;
	if (duration <= 0) {
		return -1;
	}
	int seconds = (int)((long long)value * duration / 1000);
	if (seconds < 0) {
		seconds = 0;
	}
	if (seconds > duration) {
		seconds = duration;
	}
	return seconds;
}

// While the finger moves, only the clock follows: restarting the HTTP stream on
// every slider event would never let a connection finish.
static void now_seek_cb(morpthis_widget_t *slider, int value, void *user) {
	(void)slider;
	(void)user;
	if (g_updating_slider) {
		return;
	}
	int seconds = slider_seconds(value);
	if (seconds < 0) {
		return;
	}
	g_seeking = true;
	char elapsed[16];
	format_time(seconds, elapsed, sizeof(elapsed));
	set_text_if_changed(g_now_elapsed, elapsed);
}

// The finger lifted: one restart at the chosen offset, then the normal progress
// events take over again.
static void now_seek_release_cb(morpthis_widget_t *slider, int value, void *user) {
	(void)slider;
	(void)user;
	if (g_updating_slider) {
		return;
	}
	g_seeking = false;
	int seconds = slider_seconds(value);
	if (seconds < 0) {
		return;
	}
	int duration = g_now_duration_secs > 0 ? g_now_duration_secs : stream_player_duration_ms() / 1000;
	if (duration > 2 && seconds >= duration) {
		seconds = duration - 2; // leave a little to play
	}
	if (seconds == stream_player_position_ms() / 1000) {
		return; // a tap on the current position, or a duplicate release
	}
	if (!stream_player_seek(seconds * 1000)) {
		morpthis_toast("Could not seek this stream.");
	}
	update_now_screen();
}

static const subsonic_entry_t *current_entry(void) {
	if (g_queue_index < 0 || g_queue_index >= g_queue_count) {
		return NULL;
	}
	return &g_queue[g_queue_index];
}

// Tells the player's control centre and status bar what this app is playing.
// The player copies the strings, so it is safe to call from here on the GUI
// thread; it is also safe before the output is open, which is when a track has
// just been chosen but the HTTP and TLS handshake is still running.
static void publish_track(bool playing) {
	const subsonic_entry_t *entry = current_entry();
	if (!entry) {
		morpthis_audio_set_track(NULL, false);
		return;
	}
	morpthis_track_t track;
	memset(&track, 0, sizeof(track));
	snprintf(track.title, sizeof(track.title), "%s", entry->title);
	snprintf(track.artist, sizeof(track.artist), "%s", entry->artist);
	snprintf(track.album, sizeof(track.album), "%s", entry->album);
	track.duration_ms = entry->duration > 0 ? entry->duration * 1000 : 0;
	morpthis_audio_set_track(&track, playing);
}

// The player's format line, from the stream that actually opened rather than
// the quality that was asked for: 44100 reads 44.1, 48000 reads 48.
static void format_from_stream(void) {
	int rate = stream_player_rate();
	int bits = stream_player_bits();
	if (rate <= 0 || bits <= 0) {
		return;
	}
	int khz10 = (rate + 50) / 100;
	const char *codec = stream_player_flac() ? "FLAC" : "MP3";
	if (khz10 % 10 == 0) {
		snprintf(g_now_format_text, sizeof(g_now_format_text), "%d/%d %s", bits, khz10 / 10, codec);
	} else {
		snprintf(g_now_format_text, sizeof(g_now_format_text), "%d/%d.%d %s", bits, khz10 / 10,
				 khz10 % 10, codec);
	}
}

static void on_cover(const cover_result_t *result, void *user) {
	(void)user;
	if (g_shutting_down || !result || result->generation != g_cover_generation) {
		return;
	}
	if (strcmp(result->id, g_now_cover_id) != 0) {
		return;
	}
	if (result->error[0]) {
		morpthis_logd("subsonic", "cover: %s", result->error);
		return;
	}
	snprintf(g_now_cover_path, sizeof(g_now_cover_path), "%s", result->path);
	if (g_now_cover) {
		morpthis_image_set_source(g_now_cover, g_now_cover_path);
		g_now_cover_applied = g_cover_generation;
	}
	update_now_bar();
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
	morpthis_screen_set_title(g_main_screen, current_view_title());
	morpthis_list_clear(g_list);
	refresh_status();
	update_now_bar();
	browse_worker_request((int)g_view_generation, view->kind, view->id, 0);
}

// The row the accent bar belongs to: the playing track, if it is in the list
// on screen. The queue keeps the authoritative id, so this survives a rebuild.
static int playing_row_index(void) {
	if ((!g_playing && !g_paused) || g_queue_index < 0 || g_queue_index >= g_queue_count) {
		return -1;
	}
	const char *id = g_queue[g_queue_index].id;
	for (int i = 0; i < g_entry_count; i++) {
		if (g_entries[i].kind == SUBSONIC_TRACK && strcmp(g_entries[i].id, id) == 0) {
			return i;
		}
	}
	return -1;
}

static void update_playing_mark(void) {
	if (g_list) {
		morpthis_list_set_playing(g_list, playing_row_index());
	}
	if (g_queue_list) {
		morpthis_list_set_playing(g_queue_list, (!g_playing && !g_paused) ? -1 : g_queue_index);
	}
}

static void render_entries(void) {
	if (!g_list) {
		return;
	}
	morpthis_list_clear(g_list);
	for (int i = 0; i < g_entry_count; i++) {
		const subsonic_entry_t *entry = &g_entries[i];
		char title[220];
		snprintf(title, sizeof(title), "%s%s", entry->starred ? "* " : "", entry->title);

		// The same shape as the player's library: two lines for a recording,
		// one for a name, and a chevron on everything that opens another view.
		const char *detail = NULL;
		if (entry->kind == SUBSONIC_TRACK) {
			detail = (entry->artist[0] && strcmp(entry->artist, entry->title) != 0)
						 ? entry->artist
						 : (entry->album[0] ? entry->album : NULL);
		} else if (entry->kind == SUBSONIC_ALBUM) {
			detail = entry->artist[0] ? entry->artist : NULL;
		}
		morpthis_list_add_entry(g_list, title, detail, NULL, entry->kind != SUBSONIC_TRACK);
	}
	update_playing_mark();
}

static void show_root(void) {
	g_depth = 0;
	g_root_fallback_done = false;
	g_browsing = false;
	g_have_error = false;
	// The library root is the bottom of the app: nothing is under it to go
	// back to, including after a settings reload loaded this page directly.
	nav_clear();
	g_entry_count = ROOT_COUNT;
	for (int i = 0; i < ROOT_COUNT; i++) {
		memset(&g_entries[i], 0, sizeof(g_entries[i]));
		g_entries[i].kind = SUBSONIC_MENU;
		snprintf(g_entries[i].id, sizeof(g_entries[i].id), "%d", i);
		snprintf(g_entries[i].title, sizeof(g_entries[i].title), "%s", ROOT_ITEMS[i].title);
	}
	morpthis_screen_set_title(g_main_screen, current_view_title());
	render_entries();
	refresh_status();
	update_now_bar();
}

static void show_not_configured(void) {
	g_depth = 0;
	g_entry_count = 0;
	g_browsing = false;
	g_have_error = false;
	nav_clear();
	if (g_list) {
		morpthis_list_clear(g_list);
	}
	morpthis_screen_set_title(g_main_screen, "Subsonic");
	refresh_status();
}

static void on_browse_done(const browse_result_t *result, void *user) {
	(void)user;
	if (g_shutting_down || !result || (unsigned)result->generation != g_view_generation) {
		return;
	}
	g_browsing = false;
	update_now_bar();

	if (result->error[0]) {
		show_error("browse", result->error);
		return;
	}

	g_entry_count = result->count > MAX_ENTRIES ? MAX_ENTRIES : result->count;
	if (g_entry_count > 0) {
		memcpy(g_entries, result->entries, (size_t)g_entry_count * sizeof(g_entries[0]));
	}

	render_entries();

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

static bool track_format(const subsonic_entry_t *entry, subsonic_format_t *out,
						char *why, size_t why_size) {
	subsonic_format_t format = g_settings.quality.format;
	if (format == SUBSONIC_FORMAT_OPUS) {
		snprintf(why, why_size, "Opus is not supported by this app. Choose MP3 or FLAC.");
		return false; // no Opus decoder in this app
	}
	if (format == SUBSONIC_FORMAT_RAW) {
		if (strcasecmp(entry->suffix, "flac") == 0 || strcasecmp(entry->suffix, "mp3") == 0) {
			*out = format;
			return true;
		}
		if (entry->suffix[0]) {
			snprintf(why, why_size,
					 "Original quality: .%s is not supported. Choose MP3 or FLAC transcoding.",
					 entry->suffix);
		} else {
			snprintf(why, why_size,
					 "Original quality: the server did not say what format this file is. Choose MP3 or FLAC transcoding.");
		}
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
	char why[192];
	if (!track_format(entry, &format, why, sizeof(why))) {
		show_error("playback", why);
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
	track.duration_ms = entry->duration > 0 ? entry->duration * 1000 : 0;

	if (!stream_player_start(&track)) {
		show_error("playback", "Could not start the player.");
		return false;
	}

	g_playing = true;
	g_paused = false;
	g_have_error = false;
	g_seeking = false;
	g_queue_index = index;
	snprintf(g_now_playing, sizeof(g_now_playing), "%s", entry->title);
	// The now-playing page and the mini-player show the artist; an album with no
	// artist tag is more useful than an empty line.
	clip(g_now_artist_text, sizeof(g_now_artist_text),
		 entry->artist[0] ? entry->artist : entry->album, sizeof(g_now_artist_text) - 1);
	g_now_duration_secs = entry->duration > 0 ? entry->duration : 0;
	// What the server is sending, for the small line the player puts beside the
	// names. The quality is the one this track was started with, not whatever
	// the settings page says after a later toggle.
	if (g_settings.quality.format == SUBSONIC_FORMAT_MP3) {
		snprintf(g_now_format_text, sizeof(g_now_format_text), "MP3 %d",
				 g_settings.quality.max_bitrate);
	} else if (g_settings.quality.format == SUBSONIC_FORMAT_FLAC) {
		snprintf(g_now_format_text, sizeof(g_now_format_text), "FLAC");
	} else if (g_settings.quality.format == SUBSONIC_FORMAT_RAW) {
		char suffix[16];
		clip(suffix, sizeof(suffix), entry->suffix, sizeof(suffix) - 1);
		for (char *p = suffix; *p; p++) {
			if (*p >= 'a' && *p <= 'z') {
				*p = (char)(*p - 'a' + 'A');
			}
		}
		snprintf(g_now_format_text, sizeof(g_now_format_text), "%s orig",
				 suffix[0] ? suffix : "?");
	} else {
		g_now_format_text[0] = '\0';
	}
	snprintf(g_now_cover_id, sizeof(g_now_cover_id), "%s", entry->cover_art);
	g_now_cover_path[0] = '\0';
	// The worker cancels any download for the previous track; an empty id leaves
	// it with nothing to fetch. The old file is not shown because the path above
	// was cleared.
	g_cover_generation++;
	g_now_cover_applied = 0;
	cover_worker_request(g_cover_generation, g_now_cover_id);
	publish_track(true);
	update_playing_mark();
	update_now_bar();
	update_now_screen();
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

static void toggle_play_pause(void) {
	if (!g_playing) {
		if (g_queue_count > 0) {
			play_queue_at(g_queue_index >= 0 ? g_queue_index : 0);
		}
		return;
	}
	g_paused = !g_paused;
	stream_player_pause(g_paused);
	morpthis_audio_set_playing(!g_paused);
	update_now_screen();
	update_now_bar();
	refresh_status();
}

static void now_play_cb(morpthis_widget_t *widget, void *user) {
	(void)widget;
	(void)user;
	toggle_play_pause();
}

static void now_prev_cb(morpthis_widget_t *widget, void *user) {
	(void)widget;
	(void)user;
	play_relative(-1);
	update_now_screen();
}

static void now_next_cb(morpthis_widget_t *widget, void *user) {
	(void)widget;
	(void)user;
	play_relative(1);
	update_now_screen();
}

static void now_bar_selected(morpthis_widget_t *list, int index, void *user) {
	(void)list;
	(void)index;
	(void)user;
	show_now_playing(NULL, NULL);
}

static void on_stream_event(stream_event_kind_t kind, const char *message,
							const stream_track_t *track, void *user) {
	(void)user;
	if (g_shutting_down) {
		return;
	}
	if (kind == STREAM_EVENT_STARTED) {
		// A stream (re)start is a definitive reset: if a release was lost, the
		// page must follow the new stream rather than stay frozen on a drag.
		g_seeking = false;
		if (track) {
			enqueue_scrobble(track->id, false);
		}
		// The output is open now, so the status glyph can follow. Publish again
		// with the current pause state rather than trusting the one from the
		// moment the track was chosen. The decoder has also read the header, so
		// the format line can say what is really arriving.
		format_from_stream();
		publish_track(!g_paused);
		update_now_screen();
		return;
	}
	if (kind == STREAM_EVENT_LISTENED) {
		if (track) {
			enqueue_scrobble(track->id, true);
		}
		return;
	}
	if (kind == STREAM_EVENT_PROGRESS) {
		// One event a second. Redraw only the page that shows time; rebuilding
		// the library list here would cost far more than the label it updates.
		if (morpthis_screen_active() == g_now_screen) {
			update_now_screen();
		}
		return;
	}
	if (kind == STREAM_EVENT_FINISHED) {
		g_playing = false;
		g_paused = false;
		g_seeking = false;
		if (g_queue_index >= 0 && play_queue_at(g_queue_index + 1)) {
			return;
		}
		// Keep the last track on the page, as the player's own now-playing page
		// does, but release the status bar.
		publish_track(false);
		update_playing_mark();
		update_now_bar();
		update_now_screen();
		refresh_status();
		return;
	}
	g_playing = false;
	g_paused = false;
	g_seeking = false;
	publish_track(false);
	update_playing_mark();
	update_now_bar();
	update_now_screen();
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
	g_seeking = false;
	g_have_error = false;
	morpthis_audio_set_track(NULL, false);
	update_playing_mark();
	update_now_bar();
	update_now_screen();
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
	morpthis_toast_success(result->on ? "Starred." : "Unstarred.");
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

static void more_selected(morpthis_widget_t *list, int index, void *user) {
	(void)list;
	(void)user;
	switch (index) {
	case 0:
		show_now_playing(NULL, NULL);
		break;
	case 1:
		open_queue(NULL, NULL);
		break;
	case 2:
		toggle_star(NULL, NULL);
		break;
	case 3:
		stop_playback(NULL, NULL);
		break;
	case 4:
		open_settings(NULL, NULL);
		break;
	default:
		break;
	}
}

// Rebuilt every time the page opens, so the star row names the action the tap
// will take. The player's own library rows are the model: title, dim detail,
// and a chevron on the rows that open another page.
static void update_more_screen(void) {
	if (!g_more_list) {
		return;
	}
	bool have = g_queue_index >= 0 && g_queue_index < g_queue_count;
	morpthis_list_clear(g_more_list);
	morpthis_list_add_entry(g_more_list, "Now playing", "The current track and its controls", NULL, true);
	morpthis_list_add_entry(g_more_list, "Queue", "What plays next, and Clear", NULL, true);
	morpthis_list_add_entry(g_more_list,
						 have && g_queue[g_queue_index].starred ? "Unstar current" : "Star current",
						 have ? "The track that is playing" : "Nothing is playing", NULL, false);
	morpthis_list_add_entry(g_more_list, "Stop playback", "The queue is kept for later", NULL, false);
	morpthis_list_add_entry(g_more_list, "Settings", "Account, TLS and streaming quality", NULL, true);
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
		const subsonic_entry_t *entry = &g_queue[i];
		char title[220];
		snprintf(title, sizeof(title), "%s%s", entry->starred ? "* " : "", entry->title);
		const char *detail = (entry->artist[0] && strcmp(entry->artist, entry->title) != 0)
							 ? entry->artist
							 : (entry->album[0] ? entry->album : NULL);
		morpthis_list_add_entry(g_queue_list, title, detail, NULL, false);
	}
	morpthis_list_set_playing(g_queue_list, (!g_playing && !g_paused) ? -1 : g_queue_index);
}

static void queue_selected(morpthis_widget_t *list, int index, void *user) {
	(void)list;
	(void)user;
	if (index >= 0 && index < g_queue_count) {
		// play_queue_at() redraws the queue when it is the screen on display.
		play_queue_at(index);
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
	g_seeking = false;
	morpthis_audio_set_track(NULL, false);
	update_queue_screen();
	update_playing_mark();
	update_now_bar();
	update_now_screen();
	refresh_status();
}

static void open_more(morpthis_widget_t *widget, void *user) {
	(void)widget;
	(void)user;
	update_more_screen();
	navigate_to(g_more_screen);
}

static void open_queue(morpthis_widget_t *widget, void *user) {
	(void)widget;
	(void)user;
	update_queue_screen();
	navigate_to(g_queue_screen);
}

static void search_go(void) {
	if (!g_search_query[0]) {
		morpthis_toast("Type something first.");
		return;
	}
	g_depth = 0;
	view_t *view = &g_views[g_depth++];
	view->kind = SUBSONIC_BROWSE_SEARCH;
	snprintf(view->id, sizeof(view->id), "%s", g_search_query);
	char title[128];
	snprintf(title, sizeof(title), "Search: %s", g_search_query);
	clip(view->title, sizeof(view->title), title, sizeof(view->title) - 1);
	morpthis_screen_load(g_main_screen);
	request_view();
}

// API 3 gives apps the player's own keyboard, so the hand-built A-Z page and
// its screen are gone. The query is kept as the initial text, which makes a
// second search a small edit rather than typing it all again.
static void search_prompt_done(bool accepted, const char *text, void *user) {
	(void)user;
	if (g_shutting_down || !accepted || !text) {
		return;
	}
	clip(g_search_query, sizeof(g_search_query), text, sizeof(g_search_query) - 1);
	search_go();
}

static void open_search(morpthis_widget_t *widget, void *user) {
	(void)widget;
	(void)user;
	if (!morpthis_ui_prompt_text("Search", g_search_query, false, search_prompt_done, NULL)) {
		morpthis_toast("The player's keyboard is not available here.");
	}
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
		morpthis_logi("subsonic", "TLS %s, original (no transcoding)",
					  g_settings.tls_relaxed ? "relaxed" : "strict");
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

// The settings page is the player's own shape now: a scrolling column of
// settings rows, each with its value on the right. The two rows that change a
// setting open the player's popover anchored to the row that was tapped.
static void update_settings_values(void) {
	if (g_settings_tls) {
		morpthis_settings_row_set_value(g_settings_tls, g_settings.tls_relaxed ? "Relaxed" : "Strict");
	}
	if (g_settings_quality) {
		char quality[48];
		if (g_settings.quality.format == SUBSONIC_FORMAT_FLAC) {
			snprintf(quality, sizeof(quality), "FLAC lossless");
		} else if (g_settings.quality.format == SUBSONIC_FORMAT_RAW) {
			snprintf(quality, sizeof(quality), "Original");
		} else {
			snprintf(quality, sizeof(quality), "MP3 %d kbps", g_settings.quality.max_bitrate);
		}
		morpthis_settings_row_set_value(g_settings_quality, quality);
	}
	if (g_settings_server) {
		char server[96];
		clip(server, sizeof(server),
			 g_settings_valid && g_settings.credentials.server[0] ? g_settings.credentials.server
														  : "Not configured",
			 80);
		morpthis_settings_row_set_value(g_settings_server, server);
	}
	if (g_settings_account) {
		char account[64];
		clip(account, sizeof(account),
			 g_settings_valid && g_settings.credentials.username[0] ? g_settings.credentials.username
														 : "Not configured",
			 48);
		morpthis_settings_row_set_value(g_settings_account, account);
	}
}

static void set_tls(bool relaxed) {
	if (!g_settings_valid) {
		morpthis_toast("Fix settings.json first.");
		return;
	}
	if (g_settings.tls_relaxed == relaxed) {
		return;
	}
	g_settings.tls_relaxed = relaxed;
	if (!settings_save(&g_settings)) {
		morpthis_toast("Could not save settings.json.");
	}
	char error[192];
	if (!apply_settings(error, sizeof(error))) {
		morpthis_toast(error);
	}
	update_settings_values();
	morpthis_toast(relaxed ? "Self-signed certificates are accepted." : "Strict TLS checking.");
}

static void set_quality(subsonic_format_t format, int bitrate) {
	if (!g_settings_valid) {
		morpthis_toast("Fix settings.json first.");
		return;
	}
	if (g_settings.quality.format == format &&
		(format != SUBSONIC_FORMAT_MP3 || g_settings.quality.max_bitrate == bitrate)) {
		return;
	}
	g_settings.quality.format = format;
	if (format == SUBSONIC_FORMAT_MP3) {
		g_settings.quality.max_bitrate = bitrate;
	}
	subsonic_set_quality(g_settings.quality);
	if (!settings_save(&g_settings)) {
		morpthis_toast("Could not save settings.json.");
	}
	update_settings_values();
	if (format == SUBSONIC_FORMAT_FLAC) {
		morpthis_toast("Next track: FLAC lossless (server transcoding).");
	} else if (format == SUBSONIC_FORMAT_RAW) {
		morpthis_toast("Next track: the original file, no transcoding.");
	} else {
		char text[48];
		snprintf(text, sizeof(text), "Next track: %d kbps MP3.", bitrate);
		morpthis_toast(text);
	}
}

static void tls_chosen(int index, void *user) {
	(void)user;
	set_tls(index == 1);
}

static void quality_chosen(int index, void *user) {
	(void)user;
	switch (index) {
	case 0:
		set_quality(SUBSONIC_FORMAT_MP3, 256);
		break;
	case 1:
		set_quality(SUBSONIC_FORMAT_MP3, 320);
		break;
	case 2:
		set_quality(SUBSONIC_FORMAT_FLAC, 0);
		break;
	case 3:
		set_quality(SUBSONIC_FORMAT_RAW, 0);
		break;
	default:
		break;
	}
}

static void tls_row_cb(morpthis_widget_t *row, void *user) {
	(void)user;
	morpthis_menu_item_t items[2] = {
		{"Strict", !g_settings.tls_relaxed},
		{"Relaxed", g_settings.tls_relaxed},
	};
	if (!morpthis_menu_show(row, items, 2, tls_chosen, NULL)) {
		morpthis_toast("Could not open the menu.");
	}
}

static void quality_row_cb(morpthis_widget_t *row, void *user) {
	(void)user;
	bool mp3 = g_settings.quality.format == SUBSONIC_FORMAT_MP3;
	morpthis_menu_item_t items[4] = {
		{"MP3 256 kbps", mp3 && g_settings.quality.max_bitrate == 256},
		{"MP3 320 kbps", mp3 && g_settings.quality.max_bitrate == 320},
		{"FLAC (lossless)", g_settings.quality.format == SUBSONIC_FORMAT_FLAC},
		{"Original (no transcoding)", g_settings.quality.format == SUBSONIC_FORMAT_RAW},
	};
	if (!morpthis_menu_show(row, items, 4, quality_chosen, NULL)) {
		morpthis_toast("Could not open the menu.");
	}
}

static void file_row_cb(morpthis_widget_t *row, void *user) {
	(void)row;
	(void)user;
	morpthis_toast("Edit Apps/merp_data/subsonic/settings.json, then Reload settings.");
}

// The network is not reachable from the settings page, so the account rows only
// say what is in force. Editing happens in the file, from USB or ADB.
static void account_row_cb(morpthis_widget_t *row, void *user) {
	(void)row;
	(void)user;
	morpthis_toast("Edit settings.json on the card, then Reload settings.");
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
		// The account was replaced but could not be applied; keep the rows
		// truthful about what the app now holds.
		update_settings_values();
		return;
	}
	update_settings_values();
	morpthis_screen_load(g_main_screen);
	if (g_settings.configured) {
		show_root();
	} else {
		show_not_configured();
	}
}

static void open_settings(morpthis_widget_t *widget, void *user) {
	(void)widget;
	(void)user;
	update_settings_values();
	navigate_to(g_settings_screen);
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
		// The player's control centre sends the same button; one path keeps the
		// status glyph, the mini-player and the now-playing page in step.
		toggle_play_pause();
		break;
	case MORPTHIS_BUTTON_NEXT:
		play_relative(1);
		update_now_screen();
		break;
	case MORPTHIS_BUTTON_PREV:
		play_relative(-1);
		update_now_screen();
		break;
	case MORPTHIS_BUTTON_NOW_PLAYING:
		show_now_playing(NULL, NULL);
		break;
	case MORPTHIS_BUTTON_BACK: {
		morpthis_screen_t *active = morpthis_screen_active();
		bool stays = true;
		if (active && active == g_main_screen && g_depth > 0) {
			// The player's chevron is the library's way up, as it is everywhere
			// else in the player. Only the root uses it to leave the app.
			go_up(NULL, NULL);
		} else if (active && active != g_main_screen) {
			// Every app page was reached through navigate_to(), so the stack has
			// the page below it: the queue returns to its opener, settings to
			// More, the now-playing page to whatever opened it.
			if (!navigate_back()) {
				morpthis_screen_load(g_main_screen);
				refresh_status();
			}
		} else {
			// At the root: a music app that is playing stays loaded instead. The
			// chevron hides its screens and the sound keeps going, and the status
			// bar shows it is still playing. With nothing loaded there is nothing
			// to keep, so BACK closes the app.
			stays = false;
			if ((g_playing || g_paused) && morpthis_app_background()) {
				break;
			}
			morpthis_app_quit();
		}
		if (stays) {
			// The player's back guard quits an app after BACK unless told the app
			// stayed on purpose; without this the chevron would close the app
			// instead of walking up the library.
			morpthis_app_back_handled();
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
	morpthis_widget_set_size(g_status, g_width, 30);

	// The list fills the page: there is no button strip at the bottom any more.
	g_list = morpthis_list_create(g_main_screen);
	morpthis_widget_set_pos(g_list, 0, 34);
	morpthis_widget_set_size(g_list, g_width, g_height - 34);
	morpthis_list_set_select_cb(g_list, on_select, NULL);

	// The mini-player: the playing track, in the same row the library uses, on
	// the strip the status line occupies. It replaces the status line while a
	// track is loaded and opens the full page when tapped.
	g_now_bar = morpthis_list_create(g_main_screen);
	morpthis_widget_set_pos(g_now_bar, 0, 0);
	morpthis_widget_set_size(g_now_bar, g_width, 44);
	morpthis_list_set_select_cb(g_now_bar, now_bar_selected, NULL);
	morpthis_widget_set_visible(g_now_bar, false);

	// The title row carries both actions, beside the player's own down-chevron:
	// the now-playing page and the More menu.
	morpthis_screen_add_corner_button(g_main_screen, MORPTHIS_ICON_PLAY, show_now_playing, NULL);
	morpthis_screen_add_corner_button(g_main_screen, MORPTHIS_ICON_MORE, open_more, NULL);

	return true;
}

static bool create_settings_screen(void) {
	g_settings_screen = morpthis_screen_create("Subsonic settings");
	if (!g_settings_screen) {
		return false;
	}

	// The player's own settings page: a scrolling column of cards, each with the
	// value in force on the right. The edge swipe and the chevron are the way
	// back, so there is no Back button at the bottom.
	g_settings_container = morpthis_container_create(g_settings_screen);
	if (!g_settings_container) {
		return false;
	}
	morpthis_widget_set_pos(g_settings_container, 0, 0);
	morpthis_widget_set_size(g_settings_container, g_width, g_height);
	morpthis_widget_set_padding(g_settings_container, 12, 8, 12, 8);
	morpthis_container_set_layout(g_settings_container, MORPTHIS_LAYOUT_COLUMN, MORPTHIS_ALIGN_START,
								  MORPTHIS_ALIGN_CENTER, 6);
	morpthis_container_set_scrollable(g_settings_container, true);

	g_settings_tls = morpthis_settings_row_create(g_settings_screen, "TLS verification",
												  "Strict verifies the certificate", "Strict", tls_row_cb, NULL);
	g_settings_quality = morpthis_settings_row_create(g_settings_screen, "Streaming quality",
													  "What the server sends", "MP3 256 kbps",
													  quality_row_cb, NULL);
	morpthis_widget_t *server = morpthis_settings_row_create(
		g_settings_screen, "Server", "Change it in settings.json", "", account_row_cb, NULL);
	morpthis_widget_t *account = morpthis_settings_row_create(
		g_settings_screen, "Account", "Change it in settings.json", "", account_row_cb, NULL);
	morpthis_widget_t *reload = morpthis_settings_row_create(
		g_settings_screen, "Reload settings", "Re-read settings.json from the card", "",
		reload_settings, NULL);
	morpthis_widget_t *file = morpthis_settings_row_create(
		g_settings_screen, "Settings file", "Apps/merp_data/subsonic/settings.json", "On the card",
		file_row_cb, NULL);
	if (!g_settings_tls || !g_settings_quality || !server || !account || !reload || !file) {
		return false;
	}

	g_settings_server = server;
	g_settings_account = account;
	morpthis_widget_t *const rows[] = {
		g_settings_tls, g_settings_quality, server, account, reload, file,
	};
	for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
		morpthis_widget_set_parent(rows[i], g_settings_container);
	}

	update_settings_values();
	return true;
}

static bool create_more_screen(void) {
	g_more_screen = morpthis_screen_create("More");
	if (!g_more_screen) {
		return false;
	}

	g_more_list = morpthis_list_create(g_more_screen);
	if (!g_more_list) {
		return false;
	}
	morpthis_widget_set_pos(g_more_list, 0, 0);
	morpthis_widget_set_size(g_more_list, g_width, g_height);
	morpthis_list_set_select_cb(g_more_list, more_selected, NULL);
	return true;
}

static bool create_queue_screen(void) {
	g_queue_screen = morpthis_screen_create("Queue");
	if (!g_queue_screen) {
		return false;
	}

	g_queue_list = morpthis_list_create(g_queue_screen);
	morpthis_widget_set_pos(g_queue_list, 0, 0);
	morpthis_widget_set_size(g_queue_list, g_width, g_height - 42);
	morpthis_list_set_select_cb(g_queue_list, queue_selected, NULL);

	int button_width = g_width - 40;
	if (button_width < 80) {
		return false;
	}

	// Clear is an action, not navigation; the edge swipe is the way back.
	morpthis_widget_t *clear = morpthis_button_create(g_queue_screen, "Clear", queue_clear, NULL);
	morpthis_widget_set_pos(clear, 20, g_height - 38);
	morpthis_widget_set_size(clear, button_width, 32);

	return true;
}

// The player's now-playing page, as the V1 draws it: a centred square sleeve
// over a deck that holds the names, a thin progress bar with its two clocks,
// and the transport. The side glyphs put the queue and More where the player
// puts repeat and its overflow menu, so the row reads the same.
static bool create_now_screen(void) {
	g_now_screen = morpthis_screen_create_full("Now playing");
	if (!g_now_screen) {
		return false;
	}

	int full = g_full_height > 0 ? g_full_height : g_height;
	const int inset = 12; // the deck's side inset, as COMPACT_DECK_SIDE
	const int cover_margin = 10;
	const int cover_top = 2;
	const int cover_gap = 6; // breathing room between sleeve and names
	const int play_size = 44;
	const int skip_size = 42;
	const int corner_size = 36;
	const int gap = 8;

	// The deck's own height: padding, names, bar, clocks, transport, padding.
	// The eight pixels the slider's knob draws above its track are part of the
	// info row's height, so the knob never lands on the artist line.
	const int info_h = 45;
	const int bar_h = 10; // the player's PROGRESS_TRACK_HEIGHT
	const int clock_gap = 8;
	const int clock_h = 18;
	int deck_h = 4 + info_h + 4 + bar_h + clock_gap + clock_h + 4 + play_size + 4;

	// The sleeve takes what the deck and the margins leave, as the player's
	// compact layout does; an odd side is rounded down for a whole-pixel centre.
	int cover = g_width - 2 * cover_margin;
	int limit = full - deck_h - cover_top - cover_gap;
	if (cover > limit) {
		cover = limit;
	}
	if (cover < 64) {
		cover = 64;
	}
	cover &= ~1;

	g_now_cover = morpthis_image_create(g_now_screen, "");
	morpthis_widget_set_pos(g_now_cover, (g_width - cover) / 2, cover_top);
	morpthis_widget_set_size(g_now_cover, cover, cover);

	int deck = full - deck_h;
	int info_y = deck + 4;

	g_now_title = morpthis_label_create(g_now_screen, "Nothing playing");
	morpthis_widget_set_pos(g_now_title, inset, info_y);
	morpthis_widget_set_size(g_now_title, g_width - 2 * inset, 20);
	morpthis_widget_set_text_size(g_now_title, 16);

	// The format sits on the artist's line at the right edge, where the player
	// puts its star over the stream format; the artist gives it that column.
	const int format_w = 84;
	g_now_artist = morpthis_label_create(g_now_screen, "");
	morpthis_widget_set_pos(g_now_artist, inset, info_y + 19);
	morpthis_widget_set_size(g_now_artist, g_width - 2 * inset - format_w, 18);
	morpthis_widget_set_text_size(g_now_artist, 14);
	morpthis_widget_set_text_dim(g_now_artist, true);

	g_now_format = morpthis_label_create(g_now_screen, "");
	morpthis_widget_set_pos(g_now_format, g_width - inset - format_w, info_y + 19);
	morpthis_widget_set_size(g_now_format, format_w, 18);
	morpthis_widget_set_text_size(g_now_format, 14);
	morpthis_widget_set_text_dim(g_now_format, true);
	morpthis_widget_set_text_align(g_now_format, MORPTHIS_TEXT_RIGHT);

	// The player's ten-pixel bar, now a seek control: the thumb follows the
	// finger while it moves, and the stream restarts at the chosen offset when it
	// is let go. Subsonic takes the offset as `timeOffset`, in whole seconds.
	int bar_y = info_y + info_h;
	g_now_progress = morpthis_slider_create(g_now_screen, 0, 1000, 0, now_seek_cb, NULL);
	morpthis_widget_set_pos(g_now_progress, inset, bar_y);
	morpthis_widget_set_size(g_now_progress, g_width - 2 * inset, bar_h);
	morpthis_slider_set_release_cb(g_now_progress, now_seek_release_cb, NULL);

	// Elapsed under the left end of the bar, total under the right, and how far
	// through the queue the track is in the middle: the player's clock row.
	int clock_y = bar_y + bar_h + clock_gap;
	const int clock_w = 72;
	g_now_elapsed = morpthis_label_create(g_now_screen, "0:00");
	morpthis_widget_set_pos(g_now_elapsed, inset, clock_y);
	morpthis_widget_set_size(g_now_elapsed, clock_w, clock_h);
	morpthis_widget_set_text_size(g_now_elapsed, 14);

	g_now_queue_pos = morpthis_label_create(g_now_screen, "");
	morpthis_widget_set_pos(g_now_queue_pos, (g_width - clock_w) / 2, clock_y);
	morpthis_widget_set_size(g_now_queue_pos, clock_w, clock_h);
	morpthis_widget_set_text_size(g_now_queue_pos, 14);
	morpthis_widget_set_text_dim(g_now_queue_pos, true);
	morpthis_widget_set_text_align(g_now_queue_pos, MORPTHIS_TEXT_CENTER);

	g_now_total = morpthis_label_create(g_now_screen, "");
	morpthis_widget_set_pos(g_now_total, g_width - inset - clock_w, clock_y);
	morpthis_widget_set_size(g_now_total, clock_w, clock_h);
	morpthis_widget_set_text_size(g_now_total, 14);
	morpthis_widget_set_text_align(g_now_total, MORPTHIS_TEXT_RIGHT);

	// Transport: prev, play/pause and next centred, with the queue on the left
	// and More on the right, exactly where the player's repeat and overflow
	// glyphs sit. The side buttons are narrower so nothing overlaps.
	int transport_y = full - 4 - play_size;
	int row = skip_size * 2 + play_size + gap * 2;
	int x = (g_width - row) / 2;
	int side_y = transport_y + (play_size - corner_size) / 2;

	morpthis_widget_t *queue =
		morpthis_icon_button_create(g_now_screen, MORPTHIS_ICON_QUEUE, corner_size, open_queue, NULL);
	morpthis_widget_set_pos(queue, inset, side_y);

	morpthis_widget_t *prev =
		morpthis_icon_button_create(g_now_screen, MORPTHIS_ICON_PREV, skip_size, now_prev_cb, NULL);
	morpthis_widget_set_pos(prev, x, transport_y + (play_size - skip_size) / 2);

	g_now_play = morpthis_icon_button_create(g_now_screen, MORPTHIS_ICON_PLAY, play_size,
										 now_play_cb, NULL);
	morpthis_widget_set_pos(g_now_play, x + skip_size + gap, transport_y);

	morpthis_widget_t *next =
		morpthis_icon_button_create(g_now_screen, MORPTHIS_ICON_NEXT, skip_size, now_next_cb, NULL);
	morpthis_widget_set_pos(next, x + skip_size + gap + play_size + gap,
							transport_y + (play_size - skip_size) / 2);

	morpthis_widget_t *more =
		morpthis_icon_button_create(g_now_screen, MORPTHIS_ICON_MORE, corner_size, open_more, NULL);
	morpthis_widget_set_pos(more, g_width - inset - corner_size, side_y);

	return true;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

static void load_settings_and_start(void) {
	char error[192] = "";
	if (!settings_load(&g_settings, error, sizeof(error))) {
		g_settings_valid = false;
		update_settings_values();
		show_error("settings", error);
		return;
	}
	g_settings_valid = true;
	if (!apply_settings(error, sizeof(error))) {
		update_settings_values();
		show_error("settings", error);
		return;
	}
	update_settings_values();
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
	g_full_height = morpthis_ui_full_height();
	if (g_width < 200 || g_height < 240) {
		morpthis_loge("subsonic", "the display is too small (%dx%d)", g_width, g_height);
		return 1;
	}

	subsonic_client_init();
	settings_defaults(&g_settings);
	tls_set_card_root(morpthis_fs_card_root());

	if (!create_main_screen() || !create_settings_screen() || !create_more_screen() ||
		!create_queue_screen() || !create_now_screen()) {
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
	if (!cover_worker_init(on_cover, NULL)) {
		stream_player_shutdown();
		browse_worker_shutdown();
		star_worker_shutdown();
		return 1;
	}

	morpthis_input_set_button_cb(on_button, NULL);
	morpthis_screen_load(g_main_screen);
	load_settings_and_start();
	morpthis_logi("subsonic", "ready");
	return 0;
}

// The player calls this before unloading the shared object, including when USB
// mass-storage mode closes the app to share the card. It runs on a worker
// thread with a three-second bound: no UI calls, and it must be idempotent.
// The worker threads stop and are joined while their code is still mapped.
MORPTHIS_EXPORT void morpthis_app_shutdown(void) {
	if (g_shutting_down) {
		return;
	}
	g_shutting_down = true;
	stream_player_shutdown();
	cover_worker_shutdown();
	browse_worker_shutdown();
	star_worker_shutdown();
	morpthis_audio_set_track(NULL, false);
	morpthis_logi("subsonic", "stopped");
}
