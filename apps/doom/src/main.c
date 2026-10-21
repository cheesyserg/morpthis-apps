// Doom for the Morpthis player.
//
// The game itself is doomgeneric; this file owns the SDK side: a full-panel
// canvas, the translucent landscape touch controls over the picture, and the
// thread handoff. Doom renders on its own thread into an RGB565 frame; the GUI
// thread copies that frame into the canvas and never calls into the game.

#include <morpthis/morpthis.h>

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "doom_port.h"
#include "doomkeys.h"

MORPTHIS_DEFINE_API_VERSION();

// The panel is held sideways: the app renders a landscape 320x240 design into
// the portrait canvas, rotated 90 degrees counter-clockwise. On the larger
// 480x720 profile the same design is scaled by two.
#define BASE_LANDSCAPE_HEIGHT 240

#define COLOR_BUTTON 0x2945
#define COLOR_RING 0x6B4D
#define COLOR_TEXT 0xE71C
#define COLOR_ARROW 0xC618
#define COLOR_FIRE 0x8000
#define COLOR_FIRE_RING 0xF800

// Overlay opacity, 0..255. The game stays readable underneath.
#define ALPHA_FILL 90
#define ALPHA_RING 170
#define ALPHA_ARROW 190
#define ALPHA_TEXT 230

static morpthis_screen_t *g_screen;
static morpthis_widget_t *g_canvas;
static uint16_t *g_pixels;

static int g_panel_w;
static int g_panel_h;
static int g_ui_w; // landscape width  (= panel height)
static int g_ui_h; // landscape height (= panel width)
static int g_scale = 1;
static int g_game_x0;
static int g_game_y0;
static int g_game_w;
static int g_game_h;

static int g_dpad_cx;
static int g_dpad_cy;
static int g_dpad_r;
static int g_fire_cx;
static int g_fire_cy;
static int g_fire_r;
static int g_use_cx;
static int g_use_cy;
static int g_use_r;
static int g_run_cx;
static int g_run_cy;
static int g_run_r;
static int g_menu_cx;
static int g_menu_cy;
static int g_menu_r;
static int g_ok_cx;
static int g_ok_cy;
static int g_ok_r;
static int g_x_cx;
static int g_x_cy;
static int g_x_r;

static bool g_doom_started;
static bool g_quitting;
static bool g_gameplay_keys;
static char g_wad_name[32];

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

// ---------------------------------------------------------------------------
// landscape drawing
// ---------------------------------------------------------------------------
//
// The canvas is portrait; everything below is expressed in landscape
// coordinates, where the UI is panel_h wide and panel_w tall. Each write is
// rotated 90 degrees counter-clockwise, so the device is held with its top
// edge on the right.

static uint16_t blend565(uint16_t dst, uint16_t src, int alpha) {
	int inv = 256 - alpha;
	int dr = (dst >> 11) & 0x1f, dg = (dst >> 5) & 0x3f, db = dst & 0x1f;
	int sr = (src >> 11) & 0x1f, sg = (src >> 5) & 0x3f, sb = src & 0x1f;
	int r = (dr * inv + sr * alpha) >> 8;
	int g = (dg * inv + sg * alpha) >> 8;
	int b = (db * inv + sb * alpha) >> 8;
	return (uint16_t)((r << 11) | (g << 5) | b);
}

static void put_landscape(int lx, int ly, uint16_t colour, int alpha) {
	if (lx < 0 || lx >= g_ui_w || ly < 0 || ly >= g_ui_h) {
		return;
	}
	int x = ly;
	int y = g_panel_h - 1 - lx;
	if (x < 0 || x >= g_panel_w || y < 0 || y >= g_panel_h) {
		return;
	}

	size_t index = (size_t)y * (size_t)g_panel_w + (size_t)x;
	if (alpha >= 255) {
		g_pixels[index] = colour;
	} else if (alpha > 0) {
		g_pixels[index] = blend565(g_pixels[index], colour, alpha);
	}
}

static void fill_rect(int lx, int ly, int width, int height, uint16_t colour, int alpha) {
	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			put_landscape(lx + x, ly + y, colour, alpha);
		}
	}
}

static void fill_circle(int cx, int cy, int r, uint16_t colour, int alpha) {
	for (int dy = -r; dy <= r; dy++) {
		int dx = (int)(sqrt((double)(r * r - dy * dy)) + 0.5);
		fill_rect(cx - dx, cy + dy, dx * 2 + 1, 1, colour, alpha);
	}
}

static bool in_circle(int lx, int ly, int cx, int cy, int r) {
	int dx = lx - cx;
	int dy = ly - cy;
	return dx * dx + dy * dy <= r * r;
}

static void draw_text(int lx, int ly, const char *text, uint16_t colour, int alpha, int scale) {
	for (; *text; text++) {
		const glyph_t *glyph = glyph_for(*text);
		if (glyph) {
			for (int row = 0; row < 7; row++) {
				for (int col = 0; col < 5; col++) {
					if (glyph->rows[row] & (1u << (4 - col))) {
						for (int sy = 0; sy < scale; sy++) {
							for (int sx = 0; sx < scale; sx++) {
								put_landscape(lx + col * scale + sx, ly + row * scale + sy,
											  colour, alpha);
							}
						}
					}
				}
			}
		}
		lx += 6 * scale;
	}
}

static void draw_text_centered(int cx, int cy, const char *text, uint16_t colour, int alpha,
							   int scale) {
	draw_text(cx - text_width(text, scale) / 2, cy - (7 * scale) / 2, text, colour, alpha, scale);
}

static void fill_arrow(int cx, int cy, int direction, int r, uint16_t colour, int alpha) {
	// direction: 0 left, 1 right, 2 up, 3 down. Each arm is a triangle whose
	// apex points away from the centre of the pad.
	int span = r / 4;
	int length = r * 3 / 5;

	for (int d = 0; d <= length; d++) {
		switch (direction) {
		case 0: {
			int half = span * d / length;
			fill_rect(cx - span - length + d, cy - half, 1, half * 2 + 1, colour, alpha);
			break;
		}
		case 1: {
			int half = span * (length - d) / length;
			fill_rect(cx + span + d, cy - half, 1, half * 2 + 1, colour, alpha);
			break;
		}
		case 2: {
			int half = span * d / length;
			fill_rect(cx - half, cy - span - length + d, half * 2 + 1, 1, colour, alpha);
			break;
		}
		default: {
			int half = span * (length - d) / length;
			fill_rect(cx - half, cy + span + d, half * 2 + 1, 1, colour, alpha);
			break;
		}
		}
	}
}

static void draw_button(int cx, int cy, int r, const char *label, uint16_t fill, uint16_t ring) {
	fill_circle(cx, cy, r, ring, ALPHA_RING);
	fill_circle(cx, cy, r - 2 * g_scale, fill, ALPHA_FILL);
	draw_text_centered(cx, cy, label, COLOR_TEXT, ALPHA_TEXT, g_scale);
}

static void draw_overlay(void) {
	// Movement, bottom-left.
	fill_circle(g_dpad_cx, g_dpad_cy, g_dpad_r, COLOR_RING, ALPHA_RING);
	fill_circle(g_dpad_cx, g_dpad_cy, g_dpad_r - 2 * g_scale, COLOR_BUTTON, ALPHA_FILL);
	for (int direction = 0; direction < 4; direction++) {
		fill_arrow(g_dpad_cx, g_dpad_cy, direction, g_dpad_r, COLOR_ARROW, ALPHA_ARROW);
	}

	// Actions, bottom-right.
	draw_button(g_fire_cx, g_fire_cy, g_fire_r, "FIRE", COLOR_FIRE, COLOR_FIRE_RING);
	draw_button(g_use_cx, g_use_cy, g_use_r, "USE", COLOR_BUTTON, COLOR_RING);
	draw_button(g_run_cx, g_run_cy, g_run_r, "RUN", COLOR_BUTTON, COLOR_RING);

	// Menu and confirm, top-right; leave, top-left.
	draw_button(g_menu_cx, g_menu_cy, g_menu_r, "MENU", COLOR_BUTTON, COLOR_RING);
	draw_button(g_ok_cx, g_ok_cy, g_ok_r, "OK", COLOR_BUTTON, COLOR_RING);
	draw_button(g_x_cx, g_x_cy, g_x_r, "X", COLOR_BUTTON, COLOR_RING);
}

static void draw_loading(void) {
	memset(g_pixels, 0, (size_t)g_panel_w * (size_t)g_panel_h * sizeof(uint16_t));

	int cx = g_ui_w / 2;
	int cy = g_ui_h / 2;
	draw_text_centered(cx, cy - 16 * g_scale, "DOOM", COLOR_TEXT, 255, 2 * g_scale);
	draw_text_centered(cx, cy + 10 * g_scale, "LOADING", COLOR_RING, 255, 2 * g_scale);
	draw_overlay();
}

// ---------------------------------------------------------------------------
// layout
// ---------------------------------------------------------------------------

static void compute_layout(void) {
	g_panel_w = morpthis_ui_width();
	g_panel_h = morpthis_ui_height();
	g_ui_w = g_panel_h;
	g_ui_h = g_panel_w;

	g_scale = g_ui_h / BASE_LANDSCAPE_HEIGHT;
	if (g_scale < 1) {
		g_scale = 1;
	}

	// Doom's 320x200 pixels are 4:3 on screen. A 240x320 panel rotated gives
	// exactly 320x240; a 480x720 one gives 720x480, so the game is centred
	// with 40-pixel pillars.
	g_game_w = g_ui_w;
	g_game_h = g_game_w * 3 / 4;
	if (g_game_h > g_ui_h) {
		g_game_h = g_ui_h;
		g_game_w = g_game_h * 4 / 3;
	}
	if (g_game_w > g_ui_w) {
		g_game_w = g_ui_w;
		g_game_h = g_game_w * 3 / 4;
	}
	g_game_x0 = (g_ui_w - g_game_w) / 2;
	g_game_y0 = (g_ui_h - g_game_h) / 2;

	int s = g_scale;
	g_dpad_cx = 58 * s;
	g_dpad_cy = 146 * s;
	g_dpad_r = 44 * s;

	g_fire_cx = g_ui_w - 52 * s;
	g_fire_cy = g_ui_h - 70 * s;
	g_fire_r = 36 * s;

	g_use_cx = g_ui_w - 52 * s;
	g_use_cy = g_ui_h - 128 * s;
	g_use_r = 20 * s;

	g_run_cx = g_ui_w - 116 * s;
	g_run_cy = g_use_cy;
	g_run_r = 20 * s;

	g_menu_cx = g_ui_w - 28 * s;
	g_menu_cy = 26 * s;
	g_menu_r = 15 * s;

	g_ok_cx = g_ui_w - 74 * s;
	g_ok_cy = g_menu_cy;
	g_ok_r = 15 * s;

	g_x_cx = 24 * s;
	g_x_cy = 24 * s;
	g_x_r = 15 * s;
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

static void touch_down(int lx, int ly) {
	if (g_owner != OWN_NONE) {
		release_owner();
	}

	int slop = 2 * g_scale;

	if (in_circle(lx, ly, g_x_cx, g_x_cy, g_x_r + slop)) {
		request_quit();
		return;
	}
	if (in_circle(lx, ly, g_menu_cx, g_menu_cy, g_menu_r + slop)) {
		dg_port_pulse_key(KEY_ESCAPE, 120);
		return;
	}
	if (in_circle(lx, ly, g_ok_cx, g_ok_cy, g_ok_r + slop)) {
		// Confirm, including Doom's y/n prompts.
		dg_port_pulse_key('y', 100);
		dg_port_pulse_key(KEY_ENTER, 100);
		return;
	}
	if (in_circle(lx, ly, g_use_cx, g_use_cy, g_use_r + slop)) {
		dg_port_push_key(true, KEY_USE);
		g_owner = OWN_USE;
		return;
	}
	if (in_circle(lx, ly, g_run_cx, g_run_cy, g_run_r + slop)) {
		dg_port_push_key(true, KEY_RSHIFT);
		g_owner = OWN_RUN;
		return;
	}
	if (in_circle(lx, ly, g_fire_cx, g_fire_cy, g_fire_r + slop)) {
		dg_port_push_key(true, KEY_FIRE);
		g_owner = OWN_FIRE;
		return;
	}
	if (in_circle(lx, ly, g_dpad_cx, g_dpad_cy, g_dpad_r + slop)) {
		g_owner = OWN_DPAD;
		update_dpad(lx, ly);
		return;
	}

	if (lx >= g_game_x0 && lx < g_game_x0 + g_game_w && ly >= g_game_y0 &&
		ly < g_game_y0 + g_game_h) {
		// A tap on the picture fires, so the title screen and demos can be
		// dismissed without reaching for a button.
		dg_port_push_key(true, KEY_FIRE);
		g_owner = OWN_VIEW;
	}
}

static void touch_cb(morpthis_touch_action_t action, morpthis_point_t point, void *user) {
	(void)user;

	// Touch arrives in portrait canvas pixels; the controls live in landscape
	// coordinates. See put_landscape() for the forward rotation.
	int lx = g_panel_h - 1 - point.y;
	int ly = point.x;

	switch (action) {
	case MORPTHIS_TOUCH_DOWN:
		touch_down(lx, ly);
		break;
	case MORPTHIS_TOUCH_MOVE:
		if (g_owner == OWN_DPAD) {
			update_dpad(lx, ly);
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
		dg_port_pulse_key(KEY_USE, 120);
		break;
	case MORPTHIS_BUTTON_VOLUME_UP:
		dg_port_pulse_key(KEY_STRAFE_R, 100);
		break;
	case MORPTHIS_BUTTON_VOLUME_DOWN:
		dg_port_pulse_key(KEY_STRAFE_L, 100);
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
		bool gameplay = dg_port_gameplay_active();
		if (gameplay != g_gameplay_keys) {
			g_gameplay_keys = gameplay;
			morpthis_logd("doom", "gameplay controls %s", gameplay ? "on" : "off");
			morpthis_input_set_gameplay(gameplay);
		}

		draw_overlay();
		morpthis_canvas_invalidate(g_canvas, 0, 0, g_panel_w, g_panel_h);
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
	g_gameplay_keys = false;
	morpthis_input_set_gameplay(false);

	char error[512];
	if (!g_quitting && dg_port_take_error(error, sizeof(error))) {
		morpthis_loge("doom", "stopped: %s (WAD %s)", error, g_wad_name);
		char message[768];
		snprintf(message, sizeof(message), "%s\n\nWAD: %s", error,
				 g_wad_name[0] ? g_wad_name : "(unknown)");
		if (!show_error_screen(message)) {
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

// A WAD header is 12 bytes: "IWAD"/"PWAD", then a little-endian lump count
// and directory offset. Directory entries are 16 bytes, with an 8-byte name at
// offset 8. Doom fails with "W_GetNumForName: STBAR not found" when the file
// is a PWAD, a partial download or another game's IWAD renamed, so check the
// lumps Doom's startup actually needs and skip a bad candidate.
static uint32_t wad_u32(const unsigned char *p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
		   ((uint32_t)p[3] << 24);
}

static bool wad_is_doom2_name(const char *name) {
	return strcasecmp(name, "doom2.wad") == 0 ||
		   strcasecmp(name, "freedoom2.wad") == 0 ||
		   strcasecmp(name, "plutonia.wad") == 0 ||
		   strcasecmp(name, "tnt.wad") == 0 || strcasecmp(name, "hacx.wad") == 0;
}

static void wad_why(char *why, size_t why_size, const char *reason, long size) {
	if (size >= 0) {
		snprintf(why, why_size, "%s (%ld bytes)", reason, size);
	} else {
		snprintf(why, why_size, "%s", reason);
	}
}

static bool check_wad(const char *name, char *why, size_t why_size) {
	morpthis_file_t *file = morpthis_fs_open(name, MORPTHIS_FS_READ);
	if (!file) {
		snprintf(why, why_size, "cannot open");
		return false;
	}

	long size = -1;
	if (morpthis_fs_seek(file, 0, SEEK_END) == 0) {
		size = morpthis_fs_tell(file);
	}
	if (morpthis_fs_seek(file, 0, SEEK_SET) != 0) {
		snprintf(why, why_size, "cannot read");
		morpthis_fs_close(file);
		return false;
	}

	unsigned char header[12];
	if (morpthis_fs_read(file, header, sizeof(header)) != sizeof(header)) {
		wad_why(why, why_size, "not a WAD file", size);
		morpthis_fs_close(file);
		return false;
	}
	if (memcmp(header, "IWAD", 4) != 0) {
		wad_why(why, why_size,
				memcmp(header, "PWAD", 4) == 0 ? "a PWAD, not a standalone IWAD"
											  : "not a Doom IWAD",
				size);
		morpthis_fs_close(file);
		return false;
	}

	uint32_t lumps = wad_u32(header + 4);
	uint32_t table = wad_u32(header + 8);
	if (lumps == 0 || lumps > 100000 || table < sizeof(header)) {
		wad_why(why, why_size, "broken WAD directory", size);
		morpthis_fs_close(file);
		return false;
	}
	if (morpthis_fs_seek(file, (long)table, SEEK_SET) != 0) {
		wad_why(why, why_size, "broken WAD directory", size);
		morpthis_fs_close(file);
		return false;
	}

	bool playpal = false;
	bool stbar = false;
	bool e1m1 = false;
	bool map01 = false;
	unsigned char chunk[16 * 64];

	while (lumps > 0 && !(playpal && stbar && (e1m1 || map01))) {
		uint32_t entries = lumps > 64 ? 64 : lumps;
		size_t want = (size_t)entries * 16;
		if (morpthis_fs_read(file, chunk, want) != want) {
			wad_why(why, why_size, "truncated WAD directory", size);
			morpthis_fs_close(file);
			return false;
		}
		for (uint32_t i = 0; i < entries; i++) {
			char lump[9];
			memcpy(lump, chunk + i * 16 + 8, 8);
			lump[8] = '\0';
			if (strcmp(lump, "PLAYPAL") == 0) {
				playpal = true;
			} else if (strcmp(lump, "STBAR") == 0) {
				stbar = true;
			} else if (strcmp(lump, "E1M1") == 0) {
				e1m1 = true;
			} else if (strcmp(lump, "MAP01") == 0) {
				map01 = true;
			}
		}
		lumps -= entries;
	}
	morpthis_fs_close(file);

	if (!playpal) {
		wad_why(why, why_size, "missing PLAYPAL", size);
		return false;
	}
	if (!stbar) {
		wad_why(why, why_size, "missing STBAR", size);
		return false;
	}
	if (wad_is_doom2_name(name)) {
		if (!map01) {
			wad_why(why, why_size, "no MAP01 map", size);
			return false;
		}
	} else if (!e1m1) {
		wad_why(why, why_size, "no E1M1 map", size);
		return false;
	}
	return true;
}

static bool find_wad(char *out, size_t size, char *why, size_t why_size) {
	static const char *names[] = {
		"doom2.wad", "doom.wad", "doom1.wad", "freedoom2.wad", "freedoom1.wad",
		"plutonia.wad", "tnt.wad", "chex.wad", "hacx.wad",
	};

	why[0] = '\0';
	const char *folder = morpthis_fs_app_folder();
	if (!folder || !folder[0]) {
		snprintf(why, why_size, "the app folder is not available");
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

		const char *actual = NULL;
		if (morpthis_fs_exists(names[i])) {
			actual = names[i];
		} else if (morpthis_fs_exists(upper)) {
			actual = upper;
		}
		if (!actual) {
			continue;
		}

		char reason[96];
		if (check_wad(actual, reason, sizeof(reason))) {
			snprintf(out, size, "%s/%s", folder, actual);
			snprintf(g_wad_name, sizeof(g_wad_name), "%s", actual);
			return true;
		}

		morpthis_logw("doom", "ignoring %s: %s", actual, reason);
		if (why[0] == '\0') {
			snprintf(why, why_size, "%s: %s", actual, reason);
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
	char why[256];
	if (!find_wad(wad, sizeof(wad), why, sizeof(why))) {
		if (why[0]) {
			morpthis_loge("doom", "no usable IWAD: %s", why);
			char message[512];
			snprintf(message, sizeof(message),
					 "%s\n\nDoom needs a complete IWAD: the shareware doom1.wad, "
					 "a doom.wad/doom2.wad you own, or a Freedoom WAD.",
					 why);
			if (!show_error_screen(message)) {
				return 1;
			}
		} else {
			morpthis_logw("doom", "no IWAD in %s", morpthis_fs_app_folder());
			if (!show_error_screen("No WAD file was found. Copy the shareware doom1.wad "
								   "(or your own doom.wad / doom2.wad) into the app "
								   "folder on the card, then start Doom again.")) {
				return 1;
			}
		}
		return 0;
	}
	morpthis_logi("doom", "using IWAD %s", wad);

	size_t available = morpthis_app_mem_available();
	size_t frame_bytes = (size_t)g_panel_w * (size_t)g_panel_h * sizeof(uint16_t) * 2;
	size_t needed = (size_t)6 * 1024 * 1024 + frame_bytes + (size_t)2 * 1024 * 1024;
	if (available != SIZE_MAX && available < needed) {
		morpthis_loge("doom", "only %zu bytes available, need about %zu", available, needed);
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

	dg_geometry_t geometry = {
		.panel_w = g_panel_w,
		.panel_h = g_panel_h,
		.game_x0 = g_game_x0,
		.game_y0 = g_game_y0,
		.game_w = g_game_w,
		.game_h = g_game_h,
	};
	if (!dg_port_start(wad, morpthis_fs_app_folder(), &geometry, post_frame, doom_exited)) {
		morpthis_loge("doom", "could not start the game thread");
		if (!show_error_screen("Could not start the Doom thread.")) {
			return 1;
		}
		return 0;
	}

	g_doom_started = true;
	morpthis_logi("doom", "running %dx%d landscape at (%d,%d), panel %dx%d", g_game_w,
				  g_game_h, g_game_x0, g_game_y0, g_panel_w, g_panel_h);
	return 0;
}
