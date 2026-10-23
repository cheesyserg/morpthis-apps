// Internet radio as a Morpthis app.
//
// The directory is radio-browser.info; the transport is app-owned (vendor
// HTTP/TLS, HLS, MP3 and ADTS AAC). The player only sees the SDK: raw PCM out,
// the published track for its control centre, and a NETWORK power hold while a
// station is playing.
//
// The page model is three lists and one full now-playing screen. Root and the
// two lists are built once; the list contents are replaced when a job finishes
// or a stored list is opened. BACK walks the app's own stack, and at the root
// it backgrounds the app while a station is loaded, exactly as the local
// player leaves its music running.

#include <morpthis/morpthis.h>

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "radio.h"
#include "radio_player.h"
#include "radio_store.h"
#include "tls.h"

MORPTHIS_DEFINE_API_VERSION();

#define NAV_MAX 8
#define TICK_MS 250
#define STATUS_HEIGHT 44
#define BAR_HEIGHT 48

typedef enum {
	ACT_SEARCH = 0,
	ACT_TOP,
	ACT_FAVOURITES,
	ACT_RECENT,
	ACT_GENRES,
	ACT_COUNTRIES,
	ACT_LANGUAGES,
	ACT_CUSTOM,
	ACT_ADD,
} action_t;

static const struct {
	const char *title;
	const char *detail;
	bool chevron;
} ROOT_ITEMS[] = {
	{"Search", "Find a station by name", false},
	{"Popular stations", "What people are listening to", true},
	{"Favourites", "Stations you starred", true},
	{"Recently played", "The last stations you opened", true},
	{"Genres", "Browse by what is playing", true},
	{"Countries", "Browse by where it plays", true},
	{"Languages", "Browse by the language on air", true},
	{"Custom stations", "Addresses you added", true},
	{"Add a station", "Name and stream address", false},
};

#define ROOT_COUNT ((int)(sizeof(ROOT_ITEMS) / sizeof(ROOT_ITEMS[0])))

static int width;
static int height;
static int full_height;

static morpthis_screen_t *main_screen;
static morpthis_screen_t *terms_screen;
static morpthis_screen_t *stations_screen;
static morpthis_screen_t *now_screen;

static morpthis_widget_t *root_list;
static morpthis_widget_t *now_bar;
static bool bar_shown;

static morpthis_widget_t *terms_status;
static morpthis_widget_t *terms_list;
static morpthis_widget_t *stations_status;
static morpthis_widget_t *stations_list;

static morpthis_widget_t *now_icon;
static morpthis_widget_t *now_station;
static morpthis_widget_t *now_track;
static morpthis_widget_t *now_status;
static morpthis_widget_t *now_play;
static morpthis_widget_t *now_fav;

static morpthis_screen_t *nav[NAV_MAX];
static int nav_depth;

static radio_term_t terms[RADIO_TERM_LIST_MAX];
static int term_count;
static radio_station_t stations[RADIO_STATION_MAX];
static int station_count;
static radio_browse_t current_browse;
static unsigned client_seen;

static bool waiting_terms;
static bool waiting_stations;
static unsigned waiting_request;

static radio_station_t queue[RADIO_STATION_MAX];
static int queue_count;
static int queue_index = -1;

static unsigned now_seen;
static volatile bool shutting_down;

static char add_name[RADIO_NAME_MAX];

static void on_search_text(bool accepted, const char *text, void *user);
static void on_add_name(bool accepted, const char *text, void *user);
static void on_add_url(bool accepted, const char *text, void *user);

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static void navigate_to(morpthis_screen_t *target) {
	morpthis_screen_t *active = morpthis_screen_active();
	if (active && active != target) {
		if (!(nav_depth > 0 && nav[nav_depth - 1] == active)) {
			if (nav_depth < NAV_MAX) {
				nav[nav_depth++] = active;
			} else {
				memmove(&nav[0], &nav[1], sizeof(nav[0]) * (NAV_MAX - 1));
				nav[NAV_MAX - 1] = active;
			}
		}
	}
	morpthis_screen_load(target);
}

static bool navigate_back(void) {
	while (nav_depth > 0) {
		morpthis_screen_t *target = nav[--nav_depth];
		if (target && target != morpthis_screen_active()) {
			morpthis_screen_load(target);
			return true;
		}
	}
	return false;
}

static void set_status(morpthis_widget_t *status, morpthis_widget_t *list, const char *text) {
	bool show = text && text[0];
	morpthis_widget_set_visible(status, show);
	if (show) {
		morpthis_widget_set_text(status, text);
		morpthis_widget_set_pos(list, 0, STATUS_HEIGHT);
		morpthis_widget_set_size(list, width, height - STATUS_HEIGHT);
	} else {
		morpthis_widget_set_pos(list, 0, 0);
		morpthis_widget_set_size(list, width, height);
	}
}

static void trim(char *text) {
	size_t len = strlen(text);
	while (len > 0 && (text[len - 1] == ' ' || text[len - 1] == '\t' || text[len - 1] == '\r')) {
		text[--len] = '\0';
	}
	char *start = text;
	while (*start == ' ' || *start == '\t') {
		start++;
	}
	if (start != text) {
		memmove(text, start, strlen(start) + 1);
	}
}

static bool station_playable(const radio_station_t *station) {
	return strncasecmp(station->url, "http://", 7) == 0 ||
		   strncasecmp(station->url, "https://", 8) == 0;
}

// The directory and every station need the network. The player owns the Wi-Fi
// worker, so the app asks it rather than starting a second supplicant; when it
// is off or not associated, the page says what to do instead of waiting out a
// request that cannot work.
static const char *network_hint(void) {
	if (!morpthis_wifi_available()) {
		return NULL;
	}
	if (!morpthis_wifi_enabled()) {
		return "Wi-Fi is off. Turn it on in Wireless > Wi-Fi.";
	}
	if (!morpthis_wifi_connected()) {
		return "Wi-Fi is not connected. Open Wireless > Wi-Fi.";
	}
	return NULL;
}

static void station_detail(const radio_station_t *station, char *out, size_t size) {
	char country[16];
	if (station->country[0]) {
		snprintf(country, sizeof(country), "%s", station->country);
	} else {
		country[0] = '\0';
	}

	if (station->codec[0] && station->bitrate > 0) {
		snprintf(out, size, "%s%s%s · %d kbps", country, country[0] ? " · " : "",
				 station->codec, station->bitrate);
	} else if (station->codec[0]) {
		snprintf(out, size, "%s%s%s", country, country[0] ? " · " : "", station->codec);
	} else if (station->bitrate > 0) {
		snprintf(out, size, "%s%s%d kbps", country, country[0] ? " · " : "", station->bitrate);
	} else {
		snprintf(out, size, "%s", country);
	}
}

static const char *state_word(radio_player_state_t state) {
	switch (state) {
	case RADIO_PLAYER_CONNECTING:
		return "Connecting";
	case RADIO_PLAYER_PLAYING:
		return "Playing";
	case RADIO_PLAYER_PAUSED:
		return "Paused";
	case RADIO_PLAYER_ERROR:
		return "Stopped";
	case RADIO_PLAYER_IDLE:
	default:
		return "Stopped";
	}
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

static void render_root(void) {
	morpthis_list_clear(root_list);
	for (int i = 0; i < ROOT_COUNT; i++) {
		morpthis_list_add_entry(root_list, ROOT_ITEMS[i].title, ROOT_ITEMS[i].detail, NULL,
								ROOT_ITEMS[i].chevron);
	}
}

static void render_terms(void) {
	morpthis_list_clear(terms_list);
	for (int i = 0; i < term_count; i++) {
		char detail[48];
		if (terms[i].station_count > 0) {
			snprintf(detail, sizeof(detail), "%d station%s", terms[i].station_count,
					 terms[i].station_count == 1 ? "" : "s");
		} else {
			detail[0] = '\0';
		}
		morpthis_list_add_entry(terms_list, terms[i].label, detail, NULL, true);
	}
}

static int find_station_row(const radio_station_t *station) {
	if (!station || !station->url[0]) {
		return -1;
	}
	for (int i = 0; i < station_count; i++) {
		if (strcmp(stations[i].url, station->url) == 0) {
			return i;
		}
	}
	return -1;
}

static void render_stations(void) {
	morpthis_list_clear(stations_list);
	for (int i = 0; i < station_count; i++) {
		char detail[160];
		station_detail(&stations[i], detail, sizeof(detail));
		morpthis_list_add_entry(stations_list, stations[i].name, detail, NULL, false);
	}

	radio_now_t now;
	radio_player_get(&now);
	int index = find_station_row(&now.station);
	if (now.state == RADIO_PLAYER_PLAYING || now.state == RADIO_PLAYER_CONNECTING ||
		now.state == RADIO_PLAYER_PAUSED) {
		morpthis_list_set_playing(stations_list, index);
	} else {
		morpthis_list_set_playing(stations_list, -1);
	}
}

static void refresh_now_screen(void) {
	if (!now_screen) {
		return;
	}
	radio_now_t now;
	radio_player_get(&now);

	morpthis_widget_set_text(now_station, now.station.name[0] ? now.station.name : "Radio");
	if (now.title[0] && strcmp(now.title, now.station.name) != 0) {
		morpthis_widget_set_text(now_track, now.title);
	} else if (now.state == RADIO_PLAYER_IDLE && !now.station.url[0]) {
		morpthis_widget_set_text(now_track, "Choose a station to start");
	} else {
		morpthis_widget_set_text(now_track, "Live radio");
	}

	char status[320];
	switch (now.state) {
	case RADIO_PLAYER_ERROR: {
		const char *hint = network_hint();
		snprintf(status, sizeof(status), "%s",
				 hint ? hint : (now.error[0] ? now.error : "The station stopped."));
		break;
	}
	case RADIO_PLAYER_CONNECTING:
		snprintf(status, sizeof(status), "Connecting...");
		break;
	case RADIO_PLAYER_PAUSED:
		snprintf(status, sizeof(status), "Paused");
		break;
	case RADIO_PLAYER_PLAYING:
		if (now.codec[0] && now.bitrate > 0) {
			snprintf(status, sizeof(status), "Playing · %s · %d kbps", now.codec, now.bitrate);
		} else if (now.codec[0]) {
			snprintf(status, sizeof(status), "Playing · %s", now.codec);
		} else {
			snprintf(status, sizeof(status), "Playing");
		}
		break;
	case RADIO_PLAYER_IDLE:
	default:
		snprintf(status, sizeof(status), "Stopped");
		break;
	}
	morpthis_widget_set_text(now_status, status);

	bool active = now.state == RADIO_PLAYER_PLAYING || now.state == RADIO_PLAYER_CONNECTING;
	morpthis_widget_set_icon(now_play, active ? MORPTHIS_ICON_PAUSE : MORPTHIS_ICON_PLAY);
	morpthis_widget_set_icon(now_fav, radio_store_is_favourite(&now.station) ? MORPTHIS_ICON_HEART_FILLED
																			: MORPTHIS_ICON_HEART);
	morpthis_widget_set_enabled(now_play, now.station.url[0] != '\0');
	morpthis_widget_set_enabled(now_fav, now.station.url[0] != '\0');
}

static void refresh_root_bar(void) {
	radio_now_t now;
	radio_player_get(&now);

	bool show = now.station.url[0] && (now.state == RADIO_PLAYER_CONNECTING ||
									   now.state == RADIO_PLAYER_PLAYING ||
									   now.state == RADIO_PLAYER_PAUSED);
	if (show != bar_shown) {
		bar_shown = show;
		morpthis_widget_set_visible(now_bar, show);
		if (show) {
			morpthis_widget_set_pos(root_list, 0, BAR_HEIGHT);
			morpthis_widget_set_size(root_list, width, height - BAR_HEIGHT);
		} else {
			morpthis_widget_set_pos(root_list, 0, 0);
			morpthis_widget_set_size(root_list, width, height);
		}
	}
	if (!show) {
		return;
	}

	char detail[192];
	const char *title = now.title[0] ? now.title : now.station.name;
	snprintf(detail, sizeof(detail), "%s · %s", now.station.name, state_word(now.state));
	morpthis_list_clear(now_bar);
	morpthis_list_add_entry(now_bar, title, detail, NULL, false);
}

// ---------------------------------------------------------------------------
// Playing
// ---------------------------------------------------------------------------

static void remember_queue(int index) {
	queue_count = station_count;
	if (queue_count > RADIO_STATION_MAX) {
		queue_count = RADIO_STATION_MAX;
	}
	memcpy(queue, stations, sizeof(queue[0]) * (size_t)queue_count);
	queue_index = index;
}

static void play_index(int index) {
	if (index < 0 || index >= station_count) {
		return;
	}
	if (!station_playable(&stations[index])) {
		morpthis_toast_error("This station's address is not an HTTP stream.");
		return;
	}

	remember_queue(index);
	radio_store_note_played(&stations[index]);
	if (!radio_player_play(&stations[index])) {
		morpthis_toast_error("Could not start this station.");
		return;
	}
	render_stations();
	navigate_to(now_screen);
}

static void play_relative(int delta) {
	if (queue_count <= 0) {
		return;
	}
	if (queue_index < 0) {
		queue_index = 0;
	} else {
		queue_index = (queue_index + delta + queue_count) % queue_count;
	}
	radio_store_note_played(&queue[queue_index]);
	radio_player_play(&queue[queue_index]);
	refresh_now_screen();
}

static void show_stored(const char *title, radio_station_t *list, int count, const char *empty) {
	morpthis_screen_set_title(stations_screen, title);
	station_count = count;
	if (station_count > RADIO_STATION_MAX) {
		station_count = RADIO_STATION_MAX;
	}
	if (station_count > 0 && list != stations) {
		memcpy(stations, list, sizeof(stations[0]) * (size_t)station_count);
	}
	waiting_stations = false;
	if (station_count == 0) {
		morpthis_list_clear(stations_list);
		set_status(stations_status, stations_list, empty);
	} else {
		render_stations();
		set_status(stations_status, stations_list, NULL);
	}
	navigate_to(stations_screen);
}

// ---------------------------------------------------------------------------
// Directory requests
// ---------------------------------------------------------------------------

static void begin_terms(const char *title, radio_browse_t browse) {
	current_browse = browse;
	morpthis_screen_set_title(terms_screen, title);
	term_count = 0;
	morpthis_list_clear(terms_list);
	waiting_terms = false;
	waiting_stations = false;

	const char *hint = network_hint();
	if (hint) {
		set_status(terms_status, terms_list, hint);
		navigate_to(terms_screen);
		return;
	}

	set_status(terms_status, terms_list, "Loading...");
	waiting_terms = true;
	client_seen = radio_client_serial();
	waiting_request = radio_client_terms(browse);
	navigate_to(terms_screen);
}

static void begin_stations(const char *title) {
	morpthis_screen_set_title(stations_screen, title);
	station_count = 0;
	morpthis_list_clear(stations_list);
	waiting_stations = false;
	waiting_terms = false;
	// The play queue is the list the last station came from, not whatever is
	// being browsed now. Opening a genre must not make the control centre's
	// Next button forget what is playing.

	const char *hint = network_hint();
	if (hint) {
		set_status(stations_status, stations_list, hint);
		navigate_to(stations_screen);
		return;
	}

	set_status(stations_status, stations_list, "Loading...");
	waiting_stations = true;
	client_seen = radio_client_serial();
	navigate_to(stations_screen);
}

static void begin_search(const char *text) {
	char title[256];
	snprintf(title, sizeof(title), "Search: %s", text);
	begin_stations(title);
	if (waiting_stations) {
		waiting_request = radio_client_search(text);
	}
}

static void begin_top(void) {
	begin_stations("Popular stations");
	if (waiting_stations) {
		waiting_request = radio_client_top();
	}
}

static void on_terms_done(void) {
	radio_job_t state = radio_client_state();
	if (state == RADIO_JOB_BUSY) {
		return;
	}
	// A request that was in flight when this page opened can still finish
	// first. It is not this page's answer; the request it is waiting for will
	// bump the serial again.
	if (state == RADIO_JOB_OK && !radio_client_result_is_terms()) {
		return;
	}
	if (radio_client_result_request() != waiting_request) {
		return;
	}
	waiting_terms = false;
	if (state == RADIO_JOB_OK) {
		term_count = radio_client_copy_terms(terms, RADIO_TERM_LIST_MAX);
		render_terms();
		set_status(terms_status, terms_list, term_count > 0 ? NULL : "Nothing here.");
	} else {
		term_count = 0;
		morpthis_list_clear(terms_list);
		const char *hint = network_hint();
		set_status(terms_status, terms_list, hint ? hint : radio_client_error());
	}
}

static void on_stations_done(void) {
	radio_job_t state = radio_client_state();
	if (state == RADIO_JOB_BUSY) {
		return;
	}
	if (state == RADIO_JOB_OK && radio_client_result_is_terms()) {
		return;
	}
	if (radio_client_result_request() != waiting_request) {
		return;
	}
	waiting_stations = false;
	if (state == RADIO_JOB_OK) {
		station_count = radio_client_copy_stations(stations, RADIO_STATION_MAX);
		render_stations();
		set_status(stations_status, stations_list, station_count > 0 ? NULL : "No stations were found.");
	} else {
		station_count = 0;
		morpthis_list_clear(stations_list);
		const char *hint = network_hint();
		set_status(stations_status, stations_list, hint ? hint : radio_client_error());
	}
}

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------

static void on_root_select(morpthis_widget_t *list, int index, void *user) {
	(void)list;
	(void)user;
	if (index < 0 || index >= ROOT_COUNT) {
		return;
	}

	switch ((action_t)index) {
	case ACT_SEARCH:
		if (!morpthis_ui_prompt_text("Search stations", "", false, on_search_text, NULL)) {
			morpthis_toast_error("The keyboard is not available here.");
		}
		break;
	case ACT_TOP:
		begin_top();
		break;
	case ACT_FAVOURITES: {
		int count = radio_store_favourites(stations, RADIO_STATION_MAX);
		show_stored("Favourites", stations, count,
					"No favourites yet.\nStar a station on the Now Playing page.");
		break;
	}
	case ACT_RECENT: {
		int count = radio_store_recent(stations, RADIO_STATION_MAX);
		show_stored("Recently played", stations, count, "Nothing has been played yet.");
		break;
	}
	case ACT_GENRES:
		begin_terms("Genres", RADIO_BROWSE_GENRES);
		break;
	case ACT_COUNTRIES:
		begin_terms("Countries", RADIO_BROWSE_COUNTRIES);
		break;
	case ACT_LANGUAGES:
		begin_terms("Languages", RADIO_BROWSE_LANGUAGES);
		break;
	case ACT_CUSTOM: {
		int count = radio_store_custom(stations, RADIO_STATION_MAX);
		show_stored("Custom stations", stations, count,
					"No stations added yet.\nUse \"Add a station\".");
		break;
	}
	case ACT_ADD:
		if (!morpthis_ui_prompt_text("Station name", "", false, on_add_name, NULL)) {
			morpthis_toast_error("The keyboard is not available here.");
		}
		break;
	default:
		break;
	}
}

// The prompt callbacks cannot take user data in this SDK, so the search path
// and the two-step add path are separate functions.
static void on_search_text(bool accepted, const char *text, void *user) {
	(void)user;
	if (!accepted || !text || !text[0]) {
		return;
	}
	char query[192];
	snprintf(query, sizeof(query), "%s", text);
	trim(query);
	if (!query[0]) {
		return;
	}
	begin_search(query);
}

static void on_add_url(bool accepted, const char *text, void *user);

static void on_add_name(bool accepted, const char *text, void *user) {
	(void)user;
	if (!accepted || !text || !text[0]) {
		return;
	}
	snprintf(add_name, sizeof(add_name), "%s", text);
	trim(add_name);
	if (!add_name[0]) {
		return;
	}
	if (!morpthis_ui_prompt_text("Stream address", "", false, on_add_url, NULL)) {
		morpthis_toast_error("The keyboard could not be opened.");
	}
}

static void on_add_url(bool accepted, const char *text, void *user) {
	(void)user;
	if (!accepted || !text || !text[0]) {
		return;
	}
	char url[RADIO_URL_MAX];
	snprintf(url, sizeof(url), "%s", text);
	trim(url);
	if (strncasecmp(url, "http://", 7) != 0 && strncasecmp(url, "https://", 8) != 0) {
		morpthis_ui_notice("Add a station", "The address must begin with http:// or https://.");
		return;
	}

	radio_station_t station;
	memset(&station, 0, sizeof(station));
	snprintf(station.name, sizeof(station.name), "%s", add_name);
	snprintf(station.url, sizeof(station.url), "%s", url);
	if (!radio_store_add_custom(&station)) {
		morpthis_toast_error("The station list is full.");
		return;
	}
	morpthis_toast_success("Station added");

	int count = radio_store_custom(stations, RADIO_STATION_MAX);
	station_count = count;
	queue_count = count;
	queue_index = count > 0 ? count - 1 : 0;
	for (int i = 0; i < count; i++) {
		if (strcmp(stations[i].url, station.url) == 0) {
			queue_index = i;
			break;
		}
	}
	radio_player_play(&station);
	navigate_to(now_screen);
}

static void on_terms_select(morpthis_widget_t *list, int index, void *user) {
	(void)list;
	(void)user;
	if (index < 0 || index >= term_count) {
		return;
	}
	char title[128];
	snprintf(title, sizeof(title), "%s", terms[index].label);
	begin_stations(title);
	if (waiting_stations) {
		waiting_request = radio_client_stations(current_browse, terms[index].name);
	}
}

static void on_stations_select(morpthis_widget_t *list, int index, void *user) {
	(void)list;
	(void)user;
	play_index(index);
}

static void on_now_bar_select(morpthis_widget_t *list, int index, void *user) {
	(void)list;
	(void)index;
	(void)user;
	navigate_to(now_screen);
}

static void on_now_play(morpthis_widget_t *widget, void *user) {
	(void)widget;
	(void)user;
	radio_player_toggle_pause();
	refresh_now_screen();
	refresh_root_bar();
}

static void on_now_stop(morpthis_widget_t *widget, void *user) {
	(void)widget;
	(void)user;
	radio_player_stop();
	refresh_now_screen();
	refresh_root_bar();
	if (!navigate_back()) {
		morpthis_screen_load(main_screen);
	}
}

static void on_now_fav(morpthis_widget_t *widget, void *user) {
	(void)widget;
	(void)user;
	radio_now_t now;
	radio_player_get(&now);
	if (!now.station.url[0]) {
		return;
	}
	bool favourite = radio_store_toggle_favourite(&now.station);
	morpthis_toast(favourite ? "Added to favourites" : "Removed from favourites");
	refresh_now_screen();
	render_stations();
}

static void on_button(morpthis_button_t button, morpthis_input_action_t action, void *user) {
	(void)user;
	if (action != MORPTHIS_INPUT_PRESS) {
		return;
	}

	switch (button) {
	case MORPTHIS_BUTTON_PLAY_PAUSE:
		radio_player_toggle_pause();
		refresh_now_screen();
		refresh_root_bar();
		break;
	case MORPTHIS_BUTTON_NEXT:
		play_relative(1);
		refresh_now_screen();
		refresh_root_bar();
		break;
	case MORPTHIS_BUTTON_PREV:
		play_relative(-1);
		refresh_now_screen();
		refresh_root_bar();
		break;
	case MORPTHIS_BUTTON_NOW_PLAYING:
		navigate_to(now_screen);
		refresh_now_screen();
		break;
	case MORPTHIS_BUTTON_BACK: {
		morpthis_screen_t *active = morpthis_screen_active();
		if (active && active != main_screen) {
			if (!navigate_back()) {
				morpthis_screen_load(main_screen);
			}
			morpthis_app_back_handled();
			break;
		}
		// At the root: a loaded station stays in the background, the way the
		// local player's music does. With nothing loaded, BACK leaves.
		if (radio_player_active() && morpthis_app_background()) {
			break;
		}
		morpthis_app_quit();
		break;
	}
	default:
		break;
	}
}

static void on_tick(morpthis_timer_t *timer, void *user) {
	(void)timer;
	(void)user;
	if (shutting_down) {
		return;
	}

	unsigned serial = radio_client_serial();
	if (serial != client_seen) {
		client_seen = serial;
		if (waiting_terms) {
			on_terms_done();
		}
		if (waiting_stations) {
			on_stations_done();
		}
	}

	radio_now_t now;
	radio_player_get(&now);
	if (now.serial != now_seen) {
		now_seen = now.serial;
		refresh_now_screen();
		refresh_root_bar();
		render_stations();
	}
}

// ---------------------------------------------------------------------------
// Screens
// ---------------------------------------------------------------------------

static morpthis_widget_t *make_status(morpthis_screen_t *screen) {
	morpthis_widget_t *label = morpthis_label_create(screen, "");
	morpthis_widget_set_pos(label, 0, 0);
	morpthis_widget_set_size(label, width, STATUS_HEIGHT);
	morpthis_widget_set_text_size(label, 18);
	morpthis_widget_set_text_dim(label, true);
	morpthis_widget_set_text_align(label, MORPTHIS_TEXT_CENTER);
	morpthis_widget_set_text_wrap(label, true);
	morpthis_widget_set_visible(label, false);
	return label;
}

static morpthis_widget_t *make_list(morpthis_screen_t *screen) {
	morpthis_widget_t *list = morpthis_list_create(screen);
	morpthis_widget_set_pos(list, 0, 0);
	morpthis_widget_set_size(list, width, height);
	return list;
}

static bool create_screens(void) {
	main_screen = morpthis_screen_create("Radio");
	terms_screen = morpthis_screen_create("Browse");
	stations_screen = morpthis_screen_create("Stations");
	now_screen = morpthis_screen_create_full("Now Playing");
	if (!main_screen || !terms_screen || !stations_screen || !now_screen) {
		return false;
	}

	// --- root ---
	now_bar = morpthis_list_create(main_screen);
	morpthis_widget_set_pos(now_bar, 0, 0);
	morpthis_widget_set_size(now_bar, width, BAR_HEIGHT);
	morpthis_list_set_select_cb(now_bar, on_now_bar_select, NULL);
	morpthis_widget_set_visible(now_bar, false);

	root_list = make_list(main_screen);
	morpthis_list_set_select_cb(root_list, on_root_select, NULL);
	render_root();

	// --- terms ---
	terms_status = make_status(terms_screen);
	terms_list = make_list(terms_screen);
	morpthis_list_set_select_cb(terms_list, on_terms_select, NULL);

	// --- stations ---
	stations_status = make_status(stations_screen);
	stations_list = make_list(stations_screen);
	morpthis_list_set_select_cb(stations_list, on_stations_select, NULL);

	// --- now playing ---
	now_icon = morpthis_image_create(now_screen, "icon.png");
	morpthis_widget_set_size(now_icon, 56, 56);
	morpthis_widget_set_pos(now_icon, (width - 56) / 2, 4);

	now_station = morpthis_label_create(now_screen, "Radio");
	morpthis_widget_set_pos(now_station, 0, 66);
	morpthis_widget_set_size(now_station, width, 36);
	morpthis_widget_set_text_size(now_station, 24);
	morpthis_widget_set_text_align(now_station, MORPTHIS_TEXT_CENTER);
	morpthis_widget_set_text_scroll(now_station, true);

	now_track = morpthis_label_create(now_screen, "");
	morpthis_widget_set_pos(now_track, 0, 106);
	morpthis_widget_set_size(now_track, width, 26);
	morpthis_widget_set_text_size(now_track, 20);
	morpthis_widget_set_text_dim(now_track, true);
	morpthis_widget_set_text_align(now_track, MORPTHIS_TEXT_CENTER);
	morpthis_widget_set_text_scroll(now_track, true);

	now_status = morpthis_label_create(now_screen, "");
	morpthis_widget_set_pos(now_status, 0, 138);
	morpthis_widget_set_size(now_status, width, 52);
	morpthis_widget_set_text_size(now_status, 18);
	morpthis_widget_set_text_dim(now_status, true);
	morpthis_widget_set_text_align(now_status, MORPTHIS_TEXT_CENTER);
	morpthis_widget_set_text_wrap(now_status, true);

	int button_y = full_height - 90;
	now_play = morpthis_icon_button_create(now_screen, MORPTHIS_ICON_PLAY, 72, on_now_play, NULL);
	morpthis_widget_set_pos(now_play, (width - 72) / 2, button_y - 8);

	morpthis_widget_t *stop = morpthis_icon_button_create(now_screen, MORPTHIS_ICON_STOP, 56,
													 on_now_stop, NULL);
	morpthis_widget_set_pos(stop, (width - 72) / 2 - 80, button_y);

	now_fav = morpthis_icon_button_create(now_screen, MORPTHIS_ICON_HEART, 56, on_now_fav, NULL);
	morpthis_widget_set_pos(now_fav, (width - 72) / 2 + 72 + 24, button_y);

	refresh_now_screen();
	return true;
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int morpthis_app_main(const morpthis_app_t *app) {
	signal(SIGPIPE, SIG_IGN);

	width = morpthis_ui_width();
	height = morpthis_ui_height();
	full_height = morpthis_ui_full_height();
	if (width <= 0 || height <= 0 || full_height <= 0) {
		morpthis_loge("radio", "the display reported an unusable size");
		return 1;
	}

	// The app's TLS bundle search includes <card>/.local/cacert.pem, which is
	// where the player keeps the bundle the stock firmware cannot update.
	tls_set_card_root(morpthis_fs_card_root());

	if (!radio_client_init()) {
		morpthis_loge("radio", "could not start the directory worker");
		return 1;
	}
	if (!radio_player_init()) {
		morpthis_loge("radio", "could not start the playback worker");
		radio_client_shutdown();
		return 1;
	}

	if (!radio_store_load()) {
		morpthis_logd("radio", "no saved stations yet");
	}

	if (!create_screens()) {
		morpthis_loge("radio", "could not build the screens");
		radio_player_shutdown();
		radio_client_shutdown();
		return 1;
	}

	morpthis_input_set_button_cb(on_button, NULL);
	morpthis_screen_load(main_screen);
	morpthis_timer_create(TICK_MS, on_tick, NULL);

	morpthis_logi("radio", "ready, api %d", morpthis_app_api_version(app));
	return 0;
}

MORPTHIS_EXPORT void morpthis_app_shutdown(void) {
	if (shutting_down) {
		return;
	}
	shutting_down = true;
	radio_player_shutdown();
	radio_client_shutdown();
	morpthis_power_release_all();
	morpthis_audio_set_track(NULL, false);
	morpthis_logi("radio", "stopped");
}
