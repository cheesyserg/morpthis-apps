// Doom for the Morpthis player.
//
// The game itself is doomgeneric; this file owns the SDK side: a full-panel
// canvas, the touch controls below the viewport, and the thread handoff. Doom
// renders on its own thread into an RGB565 frame; the GUI thread copies that
// frame into the canvas and never calls into the game.

#include <morpthis/morpthis.h>

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "doom_port.h"
#include "doomkeys.h"

MORPTHIS_DEFINE_API_VERSION();

// The player's V1 panel is 240x320; the larger profile is 480x720. Layout
// scales by whole multiples of the 240-wide design.
#define BASE_WIDTH 240

#define COLOR_BG 0x0841
#define COLOR_PANEL 0x10A2
#define COLOR_BUTTON 0x2945
#define COLOR_RING 0x6B4D
#define COLOR_TEXT 0xE71C
#define COLOR_ARROW 0xC618
#define COLOR_FIRE 0x8000
#define COLOR_FIRE_RING 0xF800

static morpthis_screen_t *g_screen;
static morpthis_widget_t *g_canvas;
static uint16_t *g_pixels;

static int g_panel_w;
static int g_panel_h;
static int g_scale = 1;
static int g_game_w;
static int g_game_h;
static int g_row_y;
static int g_row_r;
static int g_dpad_cx;
static int g_dpad_cy;
static int g_dpad_r;
static int g_fire_cx;
static int g_fire_cy;
static int g_fire_r;

static bool g_doom_started;
static bool g_quitting;

// ---------------------------------------------------------------------------
// a 5x7 font, enough for the labels this app draws
// ---------------------------------------------------------------------------

typedef struct {
	char ch;
	uint8_t rows[7];
} glyph_t;

static const glyph_t g_font[] = {
	{'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
	{'D', {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E}},
	{'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}},
	{'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
	{'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}},
	{'I', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F}},
	{'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}},
	{'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
	{'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}},
	{'N', {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}},
	{'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
	{'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
	{'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}},
	{'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
	{'X', {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}},
};

static const glyph_t *glyph_for(char ch) {
	for (size_t i = 0; i < sizeof(g_font) / sizeof(g_font[0]); i++) {
		if (g_font[i].ch == ch) {
			return &g_font[i];
		}
	}
	return NULL;
}

static int text_width(const char *text, int scale) {
	int chars = (int)strlen(text);
	return chars > 0 ? (chars * 6 - 1) * scale : 0;
}

static void draw_text(uint16_t *pixels, int x, int y, const char *text, uint16_t color, int scale) {
	for (; *text; text++) {
		const glyph_t *glyph = glyph_for(*text);
		if (glyph) {
			for (int row = 0; row < 7; row++) {
				for (int col = 0; col < 5; col++) {
					if (glyph->rows[row] & (1u << (4 - col))) {
						int px = x + col * scale;
						int py = y + row * scale;
						for (int sy = 0; sy < scale; sy++) {
							for (int sx = 0; sx < scale; sx++) {
								int dx = px + sx;
								int dy = py + sy;
								if (dx >= 0 && dx < g_panel_w && dy >= 0 && dy < g_panel_h) {
									pixels[(size_t)dy * (size_t)g_panel_w + dx] = color;
								}
							}
						}
					}
				}
			}
		}
		x += 6 * scale;
	}
}

static void draw_text_centered(int cx, int cy, const char *text, uint16_t color, int scale) {
	draw_text(g_pixels, cx - text_width(text, scale) / 2, cy - (7 * scale) / 2, text, color, scale);
}

// ---------------------------------------------------------------------------
// drawing primitives
// ---------------------------------------------------------------------------

static void fill_rect(int x, int y, int width, int height, uint16_t color) {
	if (x < 0) {
		width += x;
		x = 0;
	}
	if (y < 0) {
		height += y;
		y = 0;
	}
	if (x + width > g_panel_w) {
		width = g_panel_w - x;
	}
	if (y + height > g_panel_h) {
		height = g_panel_h - y;
	}
	if (width <= 0 || height <= 0) {
		return;
	}

	for (int row = 0; row < height; row++) {
		uint16_t *line = g_pixels + (size_t)(y + row) * (size_t)g_panel_w + x;
		for (int col = 0; col < width; col++) {
			line[col] = color;
		}
	}
}

static void fill_circle(int cx, int cy, int r, uint16_t color) {
	for (int dy = -r; dy <= r; dy++) {
		int dx = (int)(sqrt((double)(r * r - dy * dy)) + 0.5);
		fill_rect(cx - dx, cy + dy, dx * 2 + 1, 1, color);
	}
}

static bool in_circle(int x, int y, int cx, int cy, int r) {
	int dx = x - cx;
	int dy = y - cy;
	return dx * dx + dy * dy <= r * r;
}

static void draw_button(int cx, int cy, int r, const char *label, uint16_t fill, uint16_t ring,
						int scale) {
	fill_circle(cx, cy, r, ring);
	fill_circle(cx, cy, r - 2, fill);
	draw_text_centered(cx, cy, label, COLOR_TEXT, scale);
}

static void fill_arrow(int cx, int cy, int direction, int r, uint16_t color) {
	// direction: 0 left, 1 right, 2 up, 3 down. Each arm is a triangle
	// whose apex points away from the centre of the pad.
	int span = r / 4;      // half the base width
	int length = r * 3 / 5; // apex distance from the base

	for (int d = 0; d <= length; d++) {
		switch (direction) {
		case 0: {
			int half = span * d / length;
			fill_rect(cx - span - length + d, cy - half, 1, half * 2 + 1, color);
			break;
		}
		case 1: {
			int half = span * (length - d) / length;
			fill_rect(cx + span + d, cy - half, 1, half * 2 + 1, color);
			break;
		}
		case 2: {
			int half = span * d / length;
			fill_rect(cx - half, cy - span - length + d, half * 2 + 1, 1, color);
			break;
		}
		default: {
			int half = span * (length - d) / length;
			fill_rect(cx - half, cy + span + d, half * 2 + 1, 1, color);
			break;
		}
		}
	}
}

static void draw_controls(void) {
	fill_rect(0, g_game_h, g_panel_w, g_panel_h - g_game_h, COLOR_PANEL);

	const char *labels[5] = {"X", "MENU", "OK", "USE", "RUN"};
	for (int i = 0; i < 5; i++) {
		int cx = g_panel_w * (2 * i + 1) / 10;
		draw_button(cx, g_row_y, g_row_r, labels[i], COLOR_BUTTON, COLOR_RING, g_scale);
	}

	fill_circle(g_dpad_cx, g_dpad_cy, g_dpad_r, COLOR_RING);
	fill_circle(g_dpad_cx, g_dpad_cy, g_dpad_r - 2, COLOR_BUTTON);
	for (int direction = 0; direction < 4; direction++) {
		fill_arrow(g_dpad_cx, g_dpad_cy, direction, g_dpad_r, COLOR_ARROW);
	}

	draw_button(g_fire_cx, g_fire_cy, g_fire_r, "FIRE", COLOR_FIRE, COLOR_FIRE_RING, g_scale);
}

static void draw_loading(void) {
	fill_rect(0, 0, g_panel_w, g_panel_h, COLOR_BG);
	fill_rect(0, g_game_h, g_panel_w, g_panel_h - g_game_h, COLOR_PANEL);

	int cx = g_game_w / 2;
	int cy = g_game_h / 2;
	draw_text_centered(cx, cy - 12 * g_scale, "DOOM", COLOR_TEXT, g_scale);
	draw_text_centered(cx, cy + 10 * g_scale, "LOADING", COLOR_RING, g_scale);
	draw_controls();
}

// ---------------------------------------------------------------------------
// layout
// ---------------------------------------------------------------------------

static void compute_layout(void) {
	g_panel_w = morpthis_ui_width();
	g_panel_h = morpthis_ui_height();

	g_scale = g_panel_w / BASE_WIDTH;
	if (g_scale < 1) {
		g_scale = 1;
	}

	// Doom's 320x200 pixels are 4:3 on screen. The viewport is as wide as the
	// panel and leaves room for controls below it.
	g_game_w = g_panel_w;
	int game_h = g_panel_w * 3 / 4;
	int max_h = g_panel_h - 120 * g_scale;
	if (max_h < 60) {
		max_h = g_panel_h * 2 / 3;
	}
	if (game_h > max_h) {
		game_h = max_h;
	}
	if (game_h < 40) {
		game_h = g_panel_h * 2 / 3;
	}
	g_game_h = game_h;

	int ctrl_h = g_panel_h - g_game_h;
	g_row_y = g_game_h + ctrl_h / 7;
	g_row_r = ctrl_h / 8;
	if (g_row_r > 22 * g_scale) {
		g_row_r = 22 * g_scale;
	}
	if (g_row_r < 12) {
		g_row_r = 12;
	}

	g_dpad_cx = g_panel_w / 4;
	g_dpad_cy = g_game_h + ctrl_h * 5 / 8;
	g_dpad_r = ctrl_h * 3 / 8;
	if (g_dpad_r > g_panel_w / 5) {
		g_dpad_r = g_panel_w / 5;
	}
	if (g_dpad_r < 24) {
		g_dpad_r = 24;
	}

	g_fire_cx = g_panel_w * 78 / 100;
	g_fire_cy = g_dpad_cy;
	g_fire_r = ctrl_h * 3 / 10;
	if (g_fire_r > g_panel_w / 6) {
		g_fire_r = g_panel_w / 6;
	}
	if (g_fire_r < 24) {
		g_fire_r = 24;
	}
}

// ---------------------------------------------------------------------------
// input
// ---------------------------------------------------------------------------

typedef enum {
	OWN_NONE = 0,
	OWN_DPAD,
	OWN_FIRE,
	OWN_USE,
	OWN_RUN,
	OWN_VIEW,
} owner_t;

static owner_t g_owner;
static bool g_dir_left;
static bool g_dir_right;
static bool g_dir_up;
static bool g_dir_down;

static void request_quit(void);

static void set_dir(bool *state, bool want, unsigned char key) {
	if (*state == want) {
		return;
	}
	*state = want;
	dg_port_push_key(want, key);
}

static void update_dpad(int x, int y) {
	int dx = x - g_dpad_cx;
	int dy = y - g_dpad_cy;
	int dead = g_dpad_r / 4;

	set_dir(&g_dir_left, dx < -dead, KEY_LEFTARROW);
	set_dir(&g_dir_right, dx > dead, KEY_RIGHTARROW);
	set_dir(&g_dir_up, dy < -dead, KEY_UPARROW);
	set_dir(&g_dir_down, dy > dead, KEY_DOWNARROW);
}

static void release_owner(void) {
	switch (g_owner) {
	case OWN_DPAD:
		set_dir(&g_dir_left, false, KEY_LEFTARROW);
		set_dir(&g_dir_right, false, KEY_RIGHTARROW);
		set_dir(&g_dir_up, false, KEY_UPARROW);
		set_dir(&g_dir_down, false, KEY_DOWNARROW);
		break;
	case OWN_FIRE:
	case OWN_VIEW:
		dg_port_push_key(false, KEY_FIRE);
		break;
	case OWN_USE:
		dg_port_push_key(false, KEY_USE);
		break;
	case OWN_RUN:
		dg_port_push_key(false, KEY_RSHIFT);
		break;
	default:
		break;
	}
	g_owner = OWN_NONE;
}

static void touch_down(int x, int y) {
	if (g_owner != OWN_NONE) {
		release_owner();
	}

	for (int i = 0; i < 5; i++) {
		int cx = g_panel_w * (2 * i + 1) / 10;
		if (!in_circle(x, y, cx, g_row_y, g_row_r + 2 * g_scale)) {
			continue;
		}
		switch (i) {
		case 0: // X: leave the app
			request_quit();
			break;
		case 1: // menu
			dg_port_pulse_key(KEY_ESCAPE, 120);
			break;
		case 2: // confirm, including Doom's "y/n" prompts
			dg_port_pulse_key('y', 100);
			dg_port_pulse_key(KEY_ENTER, 100);
			break;
		case 3: // use / open
			dg_port_push_key(true, KEY_USE);
			g_owner = OWN_USE;
			break;
		default: // run
			dg_port_push_key(true, KEY_RSHIFT);
			g_owner = OWN_RUN;
			break;
		}
		return;
	}

	if (in_circle(x, y, g_fire_cx, g_fire_cy, g_fire_r + 2 * g_scale)) {
		dg_port_push_key(true, KEY_FIRE);
		g_owner = OWN_FIRE;
		return;
	}

	if (in_circle(x, y, g_dpad_cx, g_dpad_cy, g_dpad_r + 2 * g_scale)) {
		g_owner = OWN_DPAD;
		update_dpad(x, y);
		return;
	}

	if (y >= 0 && y < g_game_h && x >= 0 && x < g_game_w) {
		// A tap on the picture fires, so the first screen (title, demo) can be
		// dismissed without reaching for a button.
		dg_port_push_key(true, KEY_FIRE);
		g_owner = OWN_VIEW;
	}
}

static void touch_cb(morpthis_touch_action_t action, morpthis_point_t point, void *user) {
	(void)user;

	switch (action) {
	case MORPTHIS_TOUCH_DOWN:
		touch_down(point.x, point.y);
		break;
	case MORPTHIS_TOUCH_MOVE:
		if (g_owner == OWN_DPAD) {
			update_dpad(point.x, point.y);
		}
		break;
	case MORPTHIS_TOUCH_UP:
	case MORPTHIS_TOUCH_CANCEL:
		release_owner();
		break;
	}
}

static void button_cb(morpthis_button_t button, morpthis_input_action_t action, void *user) {
	(void)user;
	if (action == MORPTHIS_INPUT_RELEASE) {
		return;
	}

	switch (button) {
	case MORPTHIS_BUTTON_PLAY_PAUSE:
		dg_port_pulse_key(KEY_FIRE, 150);
		break;
	case MORPTHIS_BUTTON_NEXT:
		dg_port_pulse_key(']', 120);
		break;
	case MORPTHIS_BUTTON_PREV:
		dg_port_pulse_key('[', 120);
		break;
	case MORPTHIS_BUTTON_POWER:
		dg_port_pulse_key(KEY_ESCAPE, 120);
		break;
	case MORPTHIS_BUTTON_BACK:
		request_quit();
		break;
	default:
		break;
	}
}

// ---------------------------------------------------------------------------
// frames and lifecycle
// ---------------------------------------------------------------------------

static void frame_posted(void *unused) {
	(void)unused;
	if (!g_pixels) {
		return;
	}

	dg_frame_result_t result = dg_port_copy_frame(g_pixels, g_panel_w);
	if (result & DG_FRAME_COPIED) {
		morpthis_canvas_invalidate(g_canvas, 0, 0, g_game_w, g_game_h);
		morpthis_ui_refresh();
	}
}

static void post_frame(void) {
	morpthis_app_post(frame_posted, NULL);
}

static void on_doom_exited(void *unused);

static void doom_exited(void) {
	morpthis_app_post(on_doom_exited, NULL);
}

static void quit_clicked(morpthis_widget_t *widget, void *user) {
	(void)widget;
	(void)user;
	morpthis_app_quit();
}

static void error_button_cb(morpthis_button_t button, morpthis_input_action_t action, void *user) {
	(void)user;
	if (action != MORPTHIS_INPUT_RELEASE && button == MORPTHIS_BUTTON_BACK) {
		morpthis_app_quit();
	}
}

static bool show_error_screen(const char *message) {
	morpthis_screen_t *screen = morpthis_screen_create("Doom");
	if (!screen) {
		return false;
	}

	int width = morpthis_ui_width();
	int height = morpthis_ui_height();
	morpthis_widget_t *title = morpthis_label_create(screen, "Doom could not start");
	morpthis_widget_set_pos(title, 20, 20);
	morpthis_widget_set_size(title, width - 40, 28);

	morpthis_widget_t *body = morpthis_label_create(screen, message);
	morpthis_widget_set_pos(body, 20, 60);
	morpthis_widget_set_size(body, width - 40, height - 150);

	morpthis_widget_t *button = morpthis_button_create(screen, "Quit", quit_clicked, NULL);
	morpthis_widget_set_pos(button, (width - 120) / 2, height - 64);
	morpthis_widget_set_size(button, 120, 44);

	morpthis_input_set_touch_cb(NULL, NULL);
	morpthis_input_set_button_cb(error_button_cb, NULL);
	morpthis_screen_load(screen);
	return true;
}

static void on_doom_exited(void *unused) {
	(void)unused;

	dg_port_join();
	g_doom_started = false;

	char error[512];
	if (!g_quitting && dg_port_take_error(error, sizeof(error))) {
		morpthis_loge("doom", "stopped: %s", error);
		if (!show_error_screen(error)) {
			morpthis_toast("Doom stopped; see the log");
			morpthis_app_quit();
		}
		return;
	}

	morpthis_logi("doom", "exit");
	morpthis_app_quit();
}

static void request_quit(void) {
	if (g_quitting) {
		return;
	}
	g_quitting = true;

	if (g_doom_started) {
		morpthis_toast("Leaving Doom");
		dg_port_request_quit();
		return;
	}
	morpthis_app_quit();
}

static bool find_wad(char *out, size_t size) {
	static const char *names[] = {
		"doom2.wad", "doom.wad", "doom1.wad", "freedoom2.wad", "freedoom1.wad",
		"plutonia.wad", "tnt.wad", "chex.wad", "hacx.wad",
	};

	const char *folder = morpthis_fs_app_folder();
	if (!folder || !folder[0]) {
		return false;
	}

	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		char upper[32];
		snprintf(upper, sizeof(upper), "%s", names[i]);
		for (char *p = upper; *p; p++) {
			if (*p >= 'a' && *p <= 'z') {
				*p = (char)(*p - 'a' + 'A');
			}
		}

		if (morpthis_fs_exists(names[i])) {
			snprintf(out, size, "%s/%s", folder, names[i]);
			return true;
		}
		if (morpthis_fs_exists(upper)) {
			snprintf(out, size, "%s/%s", folder, upper);
			return true;
		}
	}
	return false;
}

static bool game_setup(void) {
	g_screen = morpthis_screen_create("Doom");
	if (!g_screen) {
		return false;
	}

	g_canvas = morpthis_canvas_create(g_screen, g_panel_w, g_panel_h);
	if (!g_canvas) {
		return false;
	}
	morpthis_widget_set_pos(g_canvas, 0, 0);

	g_pixels = morpthis_canvas_buffer(g_canvas);
	if (!g_pixels) {
		return false;
	}

	draw_loading();

	morpthis_input_set_button_cb(button_cb, NULL);
	morpthis_input_set_touch_cb(touch_cb, NULL);
	morpthis_screen_load(g_screen);

	morpthis_canvas_invalidate(g_canvas, 0, 0, g_panel_w, g_panel_h);
	morpthis_ui_refresh();
	return true;
}

int morpthis_app_main(const morpthis_app_t *app) {
	(void)app;
	morpthis_logi("doom", "starting");

	compute_layout();

	char wad[512];
	if (!find_wad(wad, sizeof(wad))) {
		morpthis_logw("doom", "no IWAD in %s", morpthis_fs_app_folder());
		if (!show_error_screen("No WAD file was found. Copy the shareware doom1.wad "
							   "(or your own doom.wad / doom2.wad) into the app "
							   "folder on the card, then start Doom again.")) {
			return 1;
		}
		return 0;
	}

	size_t available = morpthis_app_mem_available();
	if (available != SIZE_MAX && available < (size_t)8 * 1024 * 1024) {
		morpthis_loge("doom", "only %zu bytes available", available);
		if (!show_error_screen("Not enough free memory to start Doom. Close other "
							   "apps and try again.")) {
			return 1;
		}
		return 0;
	}

	if (!game_setup()) {
		morpthis_loge("doom", "could not create the canvas");
		return 1;
	}

	if (!dg_port_start(wad, morpthis_fs_app_folder(), g_game_w, g_game_h, post_frame,
					   doom_exited)) {
		morpthis_loge("doom", "could not start the game thread");
		if (!show_error_screen("Could not start the Doom thread.")) {
			return 1;
		}
		return 0;
	}

	g_doom_started = true;
	morpthis_logi("doom", "running %dx%d (panel %dx%d)", g_game_w, g_game_h, g_panel_w,
				  g_panel_h);
	return 0;
}
