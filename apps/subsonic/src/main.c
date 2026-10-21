// Subsonic as a Morpthis app.
//
// The test this app exists for: can a network music client live in a package
// on the card instead of inside the player? It reaches the DAC through
// morpthis_audio_open/write, does its own HTTP and TLS, and carries dr_mp3.
//
// API 1 has no text-entry widget, so the server, username and password come
// from merp_data/subsonic/settings.json. The settings screen shows the path and
// toggles the two things that are pleasant to change without a PC: strict or
// relaxed TLS, and the MP3 bitrate.

#include <morpthis/morpthis.h>

#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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

static int g_width;
static int g_height;

static morpthis_screen_t *g_main_screen;
static morpthis_screen_t *g_settings_screen;
static morpthis_widget_t *g_status;
static morpthis_widget_t *g_list;
static morpthis_widget_t *g_stop_button;

static morpthis_widget_t *g_settings_label;
static morpthis_widget_t *g_tls_button;
static morpthis_widget_t *g_quality_button;

static subsonic_settings_t g_settings;
static subsonic_entry_t g_entries[MAX_ENTRIES];
static int g_entry_count;

static view_t g_views[MAX_DEPTH];
static int g_depth;
static unsigned g_view_generation;
static bool g_browsing;
static bool g_root_fallback_done;

static bool g_playing;
static bool g_paused;
static int g_play_index = -1;
static unsigned g_playlist_view;
static char g_now_playing[200];

static bool g_have_error;
static char g_last_error[128];
static bool g_shutting_down;
static bool g_settings_valid;

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

static void show_root(void) {
	g_depth = 0;
	g_root_fallback_done = false;
	view_t *view = &g_views[g_depth++];
	view->kind = SUBSONIC_BROWSE_ARTISTS;
	view->id[0] = '\0';
	snprintf(view->title, sizeof(view->title), "Artists");
	request_view();
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
		snprintf(out, size, "%s - %s", entry->title, entry->artist);
	} else {
		snprintf(out, size, "%s", entry->title);
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

	if (g_entry_count == 0 && g_depth == 1) {
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

static bool play_entry(int index) {
	if (index < 0 || index >= g_entry_count) {
		return false;
	}
	const subsonic_entry_t *entry = &g_entries[index];
	if (entry->kind != SUBSONIC_TRACK) {
		return false;
	}

	stream_track_t track;
	memset(&track, 0, sizeof(track));
	snprintf(track.id, sizeof(track.id), "%s", entry->id);
	snprintf(track.title, sizeof(track.title), "%s", entry->title);
	snprintf(track.artist, sizeof(track.artist), "%s", entry->artist);
	snprintf(track.album, sizeof(track.album), "%s", entry->album);

	if (!stream_player_start(&track)) {
		show_error("playback", "Could not start the player.");
		return false;
	}

	g_playing = true;
	g_paused = false;
	g_have_error = false;
	g_play_index = index;
	g_playlist_view = g_view_generation;
	snprintf(g_now_playing, sizeof(g_now_playing), "%s", entry->title);
	refresh_status();
	return true;
}

static void play_relative(int delta) {
	if (!g_playing) {
		return;
	}
	if (g_playlist_view != g_view_generation) {
		morpthis_toast("Open that list again to skip.");
		return;
	}
	for (int i = g_play_index + delta; i >= 0 && i < g_entry_count; i += delta) {
		if (g_entries[i].kind == SUBSONIC_TRACK) {
			play_entry(i);
			return;
		}
	}
	morpthis_toast(delta > 0 ? "End of the list." : "Start of the list.");
}

static void on_stream_event(stream_event_kind_t kind, const char *message,
							const stream_track_t *track, void *user) {
	(void)track;
	(void)user;
	if (g_shutting_down) {
		return;
	}
	if (kind == STREAM_EVENT_FINISHED) {
		g_playing = false;
		g_paused = false;
		if (g_playlist_view == g_view_generation && g_play_index >= 0) {
			for (int i = g_play_index + 1; i < g_entry_count; i++) {
				if (g_entries[i].kind == SUBSONIC_TRACK && play_entry(i)) {
					return;
				}
			}
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
// Settings
// ---------------------------------------------------------------------------

static bool apply_settings(char *error, size_t error_size) {
	tls_set_card_root(morpthis_fs_card_root());
	tls_set_verify_mode(g_settings.tls_relaxed ? TLS_VERIFY_RELAXED : TLS_VERIFY_STRICT);
	tls_set_ca_bundle(g_settings.ca_bundle);
	morpthis_logi("subsonic", "TLS %s, %d kbps MP3", g_settings.tls_relaxed ? "relaxed" : "strict",
				  g_settings.quality.max_bitrate);

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
	snprintf(quality, sizeof(quality), "Quality: %d kbps", g_settings.quality.max_bitrate);
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
	g_settings.quality.max_bitrate = g_settings.quality.max_bitrate == 320 ? 256 : 320;
	subsonic_set_quality(g_settings.quality);
	if (!settings_save(&g_settings)) {
		morpthis_toast("Could not save settings.json.");
	}
	update_settings_screen();
	char text[48];
	snprintf(text, sizeof(text), "Next track: %d kbps.", g_settings.quality.max_bitrate);
	morpthis_toast(text);
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
	} else {
		morpthis_toast("Top of the library.");
	}
}

static void on_select(morpthis_widget_t *list, int index, void *user) {
	(void)list;
	(void)user;
	if (index < 0 || index >= g_entry_count) {
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
			if (g_play_index >= 0) {
				play_entry(g_play_index);
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
	case MORPTHIS_BUTTON_BACK:
		// The player handles its own chrome, but the host runner and a player that
		// delivers BACK before quitting both need this to be the way out.
		morpthis_app_quit();
		break;
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

	morpthis_widget_t *more = morpthis_button_create(g_main_screen, "More", open_settings, NULL);
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

	morpthis_widget_t *back = morpthis_button_create(g_settings_screen, "Back", back_to_main, NULL);
	morpthis_widget_set_pos(back, 20, 204);
	morpthis_widget_set_size(back, button_width, 34);

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

	if (!create_main_screen() || !create_settings_screen()) {
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
	morpthis_logi("subsonic", "stopped");
}
