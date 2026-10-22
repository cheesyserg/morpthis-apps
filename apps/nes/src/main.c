// NES: a ROM library and a full-screen NES canvas for the Morpthis player.
//
// The library, pause card and controls page are built from the SDK's own
// widgets. The game screen is a player canvas: the emulator hands over 256x240
// RGB565 frames, the app rotates them into the portrait panel (so the picture
// reads upright when the player is held with its top edge on the right) and
// draws the touch pad over it in the same landscape coordinates.

#include <morpthis/morpthis.h>

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "nes.h"

MORPTHIS_DEFINE_API_VERSION();

#define ROM_DIR "roms"
#define MAX_ROMS 250
#define START_MEMORY (6u * 1024u * 1024u)

#define NES_A (1u << 0)
#define NES_B (1u << 1)
#define NES_SELECT (1u << 2)
#define NES_START (1u << 3)
#define NES_UP (1u << 4)
#define NES_DOWN (1u << 5)
#define NES_LEFT (1u << 6)
#define NES_RIGHT (1u << 7)
#define NES_DIRS (NES_UP | NES_DOWN | NES_LEFT | NES_RIGHT)

// The pad's picture-relative positions, in 256x240 coordinates. Everything is
// multiplied by the integer scale the canvas ended up using.
#define DPAD_CX 52
#define DPAD_CY 176
#define DPAD_R 32
#define B_CX 188
#define B_CY 208
#define A_CX 228
#define A_CY 180
#define BUTTON_R 21
#define PAUSE_CX 240
#define PAUSE_CY 18
#define PAUSE_R 12

// The touch overlay's palette, the same one the Doom port uses: a dark disc,
// a light ring and arrows over it, all translucent so the picture stays
// readable underneath.
#define OVERLAY_DISC 0x2945
#define OVERLAY_RING 0x6B4D
#define OVERLAY_ARROW 0xC618
#define ALPHA_DISC 90
#define ALPHA_RING 170
#define ALPHA_ARROW 190

// Widget text sizes the player offers: 14, 16, 18, 20, 22, 24, 26, 32.
#define SIZE_SMALL 14
#define SIZE_BODY 16
#define SIZE_TITLE 22
#define SIZE_HERO 18

typedef struct {
	char title[96];
	char detail[96];
	char rel[192];
} rom_entry_t;

typedef struct {
	int count;
	bool truncated;
	char error[96];
	rom_entry_t *items;
} scan_result_t;

typedef enum {
	ST_LIBRARY,
	ST_SCANNING,
	ST_STARTING,
	ST_GAME,
	ST_PAUSED,
	ST_CONTROLS,
	ST_EXITING
} app_state_t;

typedef enum {
	OWN_NONE,
	OWN_DPAD,
	OWN_A,
	OWN_B
} touch_owner_t;

typedef struct {
	int dcx, dcy, dr;
	int acx, acy, ar;
	int bcx, bcy, br;
	int pcx, pcy, pr;
} controls_t;

// ---------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------

static app_state_t state = ST_LIBRARY;

static morpthis_screen_t *library_screen;
static morpthis_widget_t *library_back;
static morpthis_widget_t *library_title;
static morpthis_widget_t *library_count;
static morpthis_widget_t *library_info;
static morpthis_widget_t *library_refresh;
static morpthis_widget_t *library_accent;
static morpthis_widget_t *library_list;
static morpthis_widget_t *library_spinner;
static morpthis_widget_t *library_scan_label;
static morpthis_widget_t *library_cart;
static morpthis_widget_t *library_empty_title;
static morpthis_widget_t *library_empty_detail;
static morpthis_widget_t *library_rescan;

static morpthis_screen_t *game_screen;
static morpthis_widget_t *game_canvas;
static uint16_t *game_pixels;
static int panel_w;
static int panel_h;
static nes_geometry_t geometry;

static morpthis_screen_t *pause_screen;
static morpthis_widget_t *pause_back;
static morpthis_widget_t *pause_title;
static morpthis_widget_t *pause_game;
static morpthis_widget_t *pause_resume;
static morpthis_widget_t *pause_reset;
static morpthis_widget_t *pause_sound;
static morpthis_widget_t *pause_controls;
static morpthis_widget_t *pause_exit;

static morpthis_screen_t *controls_screen;
static morpthis_widget_t *controls_back;
static morpthis_widget_t *controls_title;
static morpthis_widget_t *controls_scroll;
static morpthis_widget_t *controls_touch;

static rom_entry_t *roms;
static int rom_count;
static char current_title[96];
static char current_rel[192];

static pthread_mutex_t scan_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t scan_thread;
static bool scan_thread_started;
static volatile bool scan_quit;
static bool scan_running;

static uint32_t pad_phys;
static uint32_t pad_touch;
static uint32_t pad_deadline[4];
static bool turbo_held;
static uint32_t turbo_deadline;
static touch_owner_t touch_owner;
static morpthis_timer_t *pulse_timer;

static bool sound_on = true;
static bool touch_controls = true;
static volatile bool shutting_down;

static uint32_t accent_rgb;
static uint16_t accent_565;

// ---------------------------------------------------------------------------
// forward declarations
// ---------------------------------------------------------------------------

static void on_scan_result(void *arg);
static void on_nes_event(void *arg);
static void post_nes_event(void);
static void open_pause(void);
static void resume_game(void);
static void exit_to_library(void);
static void cancel_start(void);
static void start_game(int index);
static void start_scan(void);
static void show_library_empty(const char *title, const char *detail);

static void on_library_back(morpthis_widget_t *widget, void *user);
static void on_library_info(morpthis_widget_t *widget, void *user);
static void on_library_rescan(morpthis_widget_t *widget, void *user);
static void on_rom_selected(morpthis_widget_t *widget, int index, void *user);
static void on_pause_back(morpthis_widget_t *widget, void *user);
static void on_resume(morpthis_widget_t *widget, void *user);
static void on_reset(morpthis_widget_t *widget, void *user);
static void on_sound(morpthis_widget_t *widget, void *user);
static void on_controls(morpthis_widget_t *widget, void *user);
static void on_exit_game(morpthis_widget_t *widget, void *user);
static void on_controls_back(morpthis_widget_t *widget, void *user);
static void on_touch_controls(morpthis_widget_t *widget, void *user);
static void no_op(morpthis_widget_t *widget, void *user);

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------

static void show(morpthis_widget_t *widget, bool visible) {
	if (widget) {
		morpthis_widget_set_visible(widget, visible);
	}
}

static void set_label(morpthis_widget_t *widget, const char *text, int size, bool dim) {
	if (!widget) {
		return;
	}
	morpthis_widget_set_text(widget, text ? text : "");
	morpthis_widget_set_text_size(widget, size);
	morpthis_widget_set_text_dim(widget, dim);
}

static morpthis_widget_t *make_label(morpthis_screen_t *screen, const char *text, int x, int y,
									 int size, bool dim) {
	morpthis_widget_t *widget = morpthis_label_create(screen, text ? text : "");
	if (!widget) {
		return NULL;
	}
	morpthis_widget_set_pos(widget, x, y);
	morpthis_widget_set_text_size(widget, size);
	morpthis_widget_set_text_dim(widget, dim);
	return widget;
}

static morpthis_widget_t *make_center_label(morpthis_screen_t *screen, const char *text, int y,
											int width, int height, int size, bool dim, bool wrap) {
	morpthis_widget_t *widget = make_label(screen, text, 0, y, size, dim);
	if (!widget) {
		return NULL;
	}
	morpthis_widget_set_size(widget, width, height);
	morpthis_widget_set_text_align(widget, MORPTHIS_TEXT_CENTER);
	morpthis_widget_set_text_wrap(widget, wrap);
	return widget;
}

static morpthis_widget_t *make_icon_button(morpthis_screen_t *screen, morpthis_icon_t icon,
										   int x, int y, int size, morpthis_click_cb cb) {
	morpthis_widget_t *widget = morpthis_icon_button_create(screen, icon, size, cb, NULL);
	if (!widget) {
		return NULL;
	}
	morpthis_widget_set_pos(widget, x, y);
	return widget;
}

static uint16_t to_565(uint32_t rgb) {
	uint32_t r = (rgb >> 16) & 0xFF;
	uint32_t g = (rgb >> 8) & 0xFF;
	uint32_t b = rgb & 0xFF;
	return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static uint16_t blend_565(uint16_t dst, uint16_t src, int alpha) {
	if (alpha <= 0) {
		return dst;
	}
	if (alpha >= 255) {
		return src;
	}
	int dr = (dst >> 11) & 0x1F;
	int dg = (dst >> 5) & 0x3F;
	int db = dst & 0x1F;
	int sr = (src >> 11) & 0x1F;
	int sg = (src >> 5) & 0x3F;
	int sb = src & 0x1F;
	int r = (dr * (255 - alpha) + sr * alpha) / 255;
	int g = (dg * (255 - alpha) + sg * alpha) / 255;
	int b = (db * (255 - alpha) + sb * alpha) / 255;
	return (uint16_t)((r << 11) | (g << 5) | b);
}

static void refresh_theme(void) {
	morpthis_theme_t theme;
	morpthis_ui_theme(&theme);
	accent_rgb = theme.accent;
	accent_565 = to_565(theme.accent);
}

static void append_text(char *out, size_t size, const char *text) {
	size_t used = strlen(out);
	if (used + 1 >= size) {
		return;
	}
	snprintf(out + used, size - used, "%s", text);
}

static void size_text(unsigned kib, char *out, size_t size) {
	if (kib >= 1024 && kib % 1024 == 0) {
		snprintf(out, size, "%uM", kib / 1024);
	} else {
		snprintf(out, size, "%uK", kib);
	}
}

static void display_name(const char *file, char *out, size_t size) {
	size_t length = strlen(file);
	if (length > 4 && strcasecmp(file + length - 4, ".nes") == 0) {
		length -= 4;
	}

	size_t j = 0;
	bool last_space = true;
	for (size_t i = 0; i < length && j + 1 < size; i++) {
		char c = file[i];
		if (c == '_') {
			c = ' ';
		}
		if (c == ' ' && last_space) {
			continue;
		}
		out[j++] = c;
		last_space = c == ' ';
	}
	while (j > 0 && out[j - 1] == ' ') {
		j--;
	}
	out[j] = '\0';
	if (j == 0) {
		snprintf(out, size, "Untitled");
	}
}

static void rom_detail(const unsigned char *header, size_t length, char *out, size_t size) {
	if (length < 16 || memcmp(header, "NES\x1a", 4) != 0) {
		snprintf(out, size, "Not an iNES file");
		return;
	}

	unsigned mapper = ((unsigned)header[6] >> 4) | ((unsigned)header[7] & 0xF0);
	unsigned prg_kib = (unsigned)header[4] * 16u;
	unsigned chr_kib = (unsigned)header[5] * 8u;
	char prg[16];
	char chr[16];
	size_text(prg_kib, prg, sizeof prg);
	size_text(chr_kib, chr, sizeof chr);

	char text[96];
	snprintf(text, sizeof text, "Mapper %u - %s PRG", mapper, prg);
	if (chr_kib > 0) {
		char part[32];
		snprintf(part, sizeof part, " - %s CHR", chr);
		append_text(text, sizeof text, part);
	}
	if (header[6] & 0x02) {
		append_text(text, sizeof text, " - save");
	}
	if (header[6] & 0x04) {
		append_text(text, sizeof text, " - trainer");
	}
	if (header[6] & 0x08) {
		append_text(text, sizeof text, " - 4 screen");
	}
	snprintf(out, size, "%s", text);
}

// ---------------------------------------------------------------------------
// scanning
// ---------------------------------------------------------------------------

static int rom_compare(const void *a, const void *b) {
	const rom_entry_t *left = (const rom_entry_t *)a;
	const rom_entry_t *right = (const rom_entry_t *)b;
	int order = strcasecmp(left->title, right->title);
	return order ? order : strcasecmp(left->rel, right->rel);
}

static bool take_scan_thread(pthread_t *out) {
	bool taken = false;
	pthread_mutex_lock(&scan_lock);
	if (scan_thread_started) {
		*out = scan_thread;
		scan_thread_started = false;
		taken = true;
	}
	pthread_mutex_unlock(&scan_lock);
	return taken;
}

static void *scan_worker(void *arg) {
	(void)arg;

	scan_result_t *result = calloc(1, sizeof *result);
	if (!result) {
		morpthis_app_post(on_scan_result, NULL);
		return NULL;
	}
	result->items = calloc(MAX_ROMS, sizeof(rom_entry_t));
	if (!result->items) {
		snprintf(result->error, sizeof result->error, "Not enough memory for the game list.");
		morpthis_app_post(on_scan_result, result);
		return NULL;
	}

	// The ROM folder lives beside the app's writable data, never in the
	// read-only package.
	morpthis_fs_mkdir(ROM_DIR);

	char folder[512];
	snprintf(folder, sizeof folder, "%s/%s", morpthis_fs_data_folder(), ROM_DIR);

	morpthis_dir_t *dir = morpthis_fs_open_dir(folder);
	if (!dir) {
		if (errno != ENOENT) {
			snprintf(result->error, sizeof result->error,
					 "Could not open the ROM folder on the card.");
		}
	} else {
		const char *name;
		while ((name = morpthis_fs_dir_next(dir)) != NULL) {
			if (scan_quit) {
				break;
			}
			if (morpthis_fs_dir_is_dir(dir) || name[0] == '.') {
				continue;
			}
			size_t length = strlen(name);
			if (length <= 4 || strcasecmp(name + length - 4, ".nes") != 0) {
				continue;
			}
			if (result->count >= MAX_ROMS) {
				result->truncated = true;
				break;
			}

			char absolute[768];
			if ((size_t)snprintf(absolute, sizeof absolute, "%s/%s", folder, name) >= sizeof absolute) {
				continue;
			}

			rom_entry_t *entry = &result->items[result->count];
			display_name(name, entry->title, sizeof entry->title);
			snprintf(entry->rel, sizeof entry->rel, "%s/%s", ROM_DIR, name);

			unsigned char header[17];
			size_t got = 0;
			long read = morpthis_fs_read_all_absolute(absolute, (char *)header, sizeof header, &got);
			if (read < 0) {
				snprintf(entry->detail, sizeof entry->detail, "Could not read this file");
			} else {
				rom_detail(header, got, entry->detail, sizeof entry->detail);
			}
			morpthis_logd("nes", "found %s: %s", entry->title, entry->detail);
			result->count++;
		}
		morpthis_fs_dir_close(dir);
	}

	if (scan_quit) {
		free(result->items);
		free(result);
		return NULL;
	}

	if (result->count > 1) {
		qsort(result->items, (size_t)result->count, sizeof(rom_entry_t), rom_compare);
	}
	morpthis_app_post(on_scan_result, result);
	return NULL;
}

static void start_scan(void) {
	if (scan_running) {
		morpthis_toast("Already looking for games.");
		return;
	}

	state = ST_SCANNING;
	scan_running = true;
	scan_quit = false;

	show(library_list, false);
	show(library_cart, false);
	show(library_empty_title, false);
	show(library_empty_detail, false);
	show(library_spinner, true);
	show(library_scan_label, true);
	show(library_rescan, false);
	morpthis_widget_set_enabled(library_rescan, false);
	morpthis_widget_set_enabled(library_refresh, false);
	set_label(library_count, "Scanning...", SIZE_SMALL, true);

	pthread_t thread;
	if (pthread_create(&thread, NULL, scan_worker, NULL) != 0) {
		scan_running = false;
		state = ST_LIBRARY;
		show(library_spinner, false);
		show(library_scan_label, false);
		morpthis_widget_set_enabled(library_rescan, true);
		morpthis_widget_set_enabled(library_refresh, true);
		if (rom_count > 0) {
			show(library_list, true);
			set_label(library_count, "Rescan failed", SIZE_SMALL, true);
		} else {
			set_label(library_count, "No games", SIZE_SMALL, true);
			show_library_empty("Could not read the card",
							   "Could not start the scan. Tap Rescan to try again.");
		}
		morpthis_toast_error("Could not start the card scan.");
		return;
	}

	pthread_mutex_lock(&scan_lock);
	scan_thread = thread;
	scan_thread_started = true;
	pthread_mutex_unlock(&scan_lock);
}

static void show_library_empty(const char *title, const char *detail) {
	show(library_list, false);
	show(library_spinner, false);
	show(library_scan_label, false);
	show(library_cart, true);
	show(library_empty_title, true);
	show(library_empty_detail, true);
	show(library_rescan, true);
	set_label(library_empty_title, title, SIZE_HERO, false);
	set_label(library_empty_detail, detail, SIZE_SMALL, true);
}

static void on_scan_result(void *arg) {
	scan_result_t *result = (scan_result_t *)arg;

	pthread_t finished;
	if (take_scan_thread(&finished)) {
		pthread_join(finished, NULL);
	}

	scan_running = false;

	if (shutting_down || scan_quit) {
		if (result) {
			free(result->items);
			free(result);
		}
		return;
	}

	morpthis_widget_set_enabled(library_rescan, true);
	morpthis_widget_set_enabled(library_refresh, true);
	state = ST_LIBRARY;
	show(library_spinner, false);
	show(library_scan_label, false);

	if (!result) {
		set_label(library_count, "No games", SIZE_SMALL, true);
		show_library_empty("Could not read the card", "The card is not available. Put it back and tap Rescan.");
		return;
	}

	free(roms);
	roms = result->items;
	rom_count = result->count;
	result->items = NULL;

	char error[96];
	snprintf(error, sizeof error, "%s", result->error);
	bool truncated = result->truncated;
	free(result);

	if (error[0]) {
		set_label(library_count, "No games", SIZE_SMALL, true);
		show_library_empty("Could not read the card", error);
		return;
	}

	morpthis_list_clear(library_list);
	for (int i = 0; i < rom_count; i++) {
		if (morpthis_list_add_entry(library_list, roms[i].title,
									roms[i].detail[0] ? roms[i].detail : NULL,
									"cart.png", true) < 0) {
			break;
		}
	}
	rom_count = morpthis_list_count(library_list);

	if (rom_count == 0) {
		set_label(library_count, "No games", SIZE_SMALL, true);
		show_library_empty("No games yet",
						   "Copy .nes files into\nApps/merp_data/nes/roms on the card,\nthen tap Rescan.");
		return;
	}

	char count[32];
	snprintf(count, sizeof count, "%d game%s", rom_count, rom_count == 1 ? "" : "s");
	set_label(library_count, count, SIZE_SMALL, true);
	show(library_cart, false);
	show(library_empty_title, false);
	show(library_empty_detail, false);
	show(library_rescan, false);
	show(library_list, true);
	morpthis_list_scroll_to(library_list, 0);

#ifdef NES_HOST_TEST
	// The headless host cannot click a list row. A host-only smoke test can
	// still exercise the emulator by putting a ROM in build/host/data/roms and
	// setting NES_AUTOSTART=1.
	const char *autostart = getenv("NES_AUTOSTART");
	if (autostart && autostart[0] == '1') {
		start_game(0);
	}
#endif

	if (truncated) {
		morpthis_toast("Showing the first 250 games.");
	}
}

// ---------------------------------------------------------------------------
// overlays
// ---------------------------------------------------------------------------

static controls_t controls_geometry(void) {
	int scale = geometry.scale < 1 ? 1 : geometry.scale;
	int gx = geometry.game_x0;
	int gy = geometry.game_y0;
	controls_t c;
	c.dcx = gx + DPAD_CX * scale;
	c.dcy = gy + DPAD_CY * scale;
	c.dr = DPAD_R * scale;
	c.acx = gx + A_CX * scale;
	c.acy = gy + A_CY * scale;
	c.ar = BUTTON_R * scale;
	c.bcx = gx + B_CX * scale;
	c.bcy = gy + B_CY * scale;
	c.br = BUTTON_R * scale;
	c.pcx = gx + PAUSE_CX * scale;
	c.pcy = gy + PAUSE_CY * scale;
	c.pr = PAUSE_R * scale;
	return c;
}

static void put_landscape(int lx, int ly, uint16_t colour, int alpha) {
	if (!game_pixels || geometry.scale < 1) {
		return;
	}
	int x = geometry.game_y0 + ly;
	int y = geometry.panel_h - 1 - lx;
	if (x < 0 || x >= geometry.panel_w || y < 0 || y >= geometry.panel_h) {
		return;
	}
	uint16_t *pixel = &game_pixels[(size_t)y * (size_t)geometry.panel_w + (size_t)x];
	*pixel = blend_565(*pixel, colour, alpha);
}

static void fill_rect(int x0, int y0, int x1, int y1, uint16_t colour, int alpha) {
	if (x0 > x1) {
		int swap = x0;
		x0 = x1;
		x1 = swap;
	}
	if (y0 > y1) {
		int swap = y0;
		y0 = y1;
		y1 = swap;
	}
	for (int y = y0; y < y1; y++) {
		for (int x = x0; x < x1; x++) {
			put_landscape(x, y, colour, alpha);
		}
	}
}

static void fill_circle(int cx, int cy, int radius, uint16_t colour, int alpha) {
	int radius2 = radius * radius;
	for (int y = -radius; y <= radius; y++) {
		for (int x = -radius; x <= radius; x++) {
			if (x * x + y * y <= radius2) {
				put_landscape(cx + x, cy + y, colour, alpha);
			}
		}
	}
}

static void ring(int cx, int cy, int radius, int thickness, uint16_t colour, int alpha) {
	int outer = radius * radius;
	int inner_radius = radius - thickness;
	int inner = inner_radius > 0 ? inner_radius * inner_radius : 0;
	for (int y = -radius; y <= radius; y++) {
		for (int x = -radius; x <= radius; x++) {
			int distance = x * x + y * y;
			if (distance <= outer && distance >= inner) {
				put_landscape(cx + x, cy + y, colour, alpha);
			}
		}
	}
}

// A triangular arm whose apex points away from the pad's centre. Direction:
// 0 left, 1 right, 2 up, 3 down.
static void fill_arrow(int cx, int cy, int direction, int radius, uint16_t colour, int alpha) {
	int span = radius / 4;
	int length = radius * 3 / 5;

	for (int d = 0; d <= length; d++) {
		switch (direction) {
		case 0: {
			int half = span * d / length;
			fill_rect(cx - span - length + d, cy - half, cx - span - length + d + 1,
					  cy + half + 1, colour, alpha);
			break;
		}
		case 1: {
			int half = span * (length - d) / length;
			fill_rect(cx + span + d, cy - half, cx + span + d + 1, cy + half + 1,
					  colour, alpha);
			break;
		}
		case 2: {
			int half = span * d / length;
			fill_rect(cx - half, cy - span - length + d, cx + half + 1,
					  cy - span - length + d + 1, colour, alpha);
			break;
		}
		default: {
			int half = span * (length - d) / length;
			fill_rect(cx - half, cy + span + d, cx + half + 1,
					  cy + span + d + 1, colour, alpha);
			break;
		}
		}
	}
}

static const uint8_t glyph_a[5] = {
	0x0E, 0x11, 0x11, 0x1F, 0x11,
};
static const uint8_t glyph_b[5] = {
	0x1E, 0x11, 0x11, 0x1E, 0x11,
};

static void glyph(char letter, int cx, int cy, int dot, uint16_t colour, int alpha) {
	const uint8_t *rows = letter == 'A' ? glyph_a : glyph_b;
	int width = 5 * dot;
	int height = 5 * dot;
	int x0 = cx - width / 2;
	int y0 = cy - height / 2;
	for (int row = 0; row < 5; row++) {
		for (int column = 0; column < 5; column++) {
			if (rows[row] & (1 << (4 - column))) {
				fill_rect(x0 + column * dot, y0 + row * dot,
						  x0 + (column + 1) * dot, y0 + (row + 1) * dot,
						  colour, alpha);
			}
		}
	}
}

static void draw_pad_button(int cx, int cy, int radius, char letter, bool pressed, int scale) {
	fill_circle(cx, cy, radius, OVERLAY_RING, ALPHA_RING);
	fill_circle(cx, cy, radius - 2 * scale, OVERLAY_DISC, ALPHA_DISC);
	if (pressed) {
		fill_circle(cx, cy, radius - 2 * scale, accent_565, 140);
	}
	glyph(letter, cx, cy, 3 * scale, 0xFFFF, 230);
}

static void draw_overlay(void) {
	if (!game_pixels || geometry.scale < 1) {
		return;
	}
	int scale = geometry.scale;
	controls_t c = controls_geometry();

	// The pause target is always there, even with the pad hidden.
	fill_circle(c.pcx, c.pcy, c.pr, 0x0000, 110);
	ring(c.pcx, c.pcy, c.pr, 2 * scale, 0xFFFF, 150);
	fill_rect(c.pcx - 4 * scale, c.pcy - 6 * scale, c.pcx - scale, c.pcy + 6 * scale,
			  0xFFFF, 220);
	fill_rect(c.pcx + scale, c.pcy - 6 * scale, c.pcx + 4 * scale, c.pcy + 6 * scale,
			  0xFFFF, 220);

	if (!touch_controls) {
		return;
	}

	// D-pad: a translucent disc with four arrows, like the Doom port; the
	// arrow of a held direction takes the accent colour.
	fill_circle(c.dcx, c.dcy, c.dr, OVERLAY_RING, ALPHA_RING);
	fill_circle(c.dcx, c.dcy, c.dr - 2 * scale, OVERLAY_DISC, ALPHA_DISC);
	for (int direction = 0; direction < 4; direction++) {
		bool pressed = false;
		switch (direction) {
		case 0:
			pressed = (pad_touch & NES_LEFT) != 0;
			break;
		case 1:
			pressed = (pad_touch & NES_RIGHT) != 0;
			break;
		case 2:
			pressed = (pad_touch & NES_UP) != 0;
			break;
		default:
			pressed = (pad_touch & NES_DOWN) != 0;
			break;
		}
		fill_arrow(c.dcx, c.dcy, direction, c.dr, pressed ? accent_565 : OVERLAY_ARROW,
				   pressed ? 225 : ALPHA_ARROW);
	}

	uint32_t buttons = pad_phys | pad_touch;
	draw_pad_button(c.acx, c.acy, c.ar, 'A', (buttons & NES_A) != 0, scale);
	draw_pad_button(c.bcx, c.bcy, c.br, 'B', (buttons & NES_B) != 0, scale);
}

// ---------------------------------------------------------------------------
// input
// ---------------------------------------------------------------------------

static int pad_slot(uint32_t bit) {
	switch (bit) {
	case NES_A:
		return 0;
	case NES_B:
		return 1;
	case NES_START:
		return 2;
	case NES_SELECT:
		return 3;
	default:
		return 0;
	}
}

static void push_pad(void) {
	nes_set_pad(pad_phys | pad_touch);
}

static bool time_reached(uint32_t now, uint32_t deadline) {
	return (int32_t)(now - deadline) >= 0;
}

static void phys_press(uint32_t bit, uint32_t milliseconds) {
	pad_phys |= bit;
	pad_deadline[pad_slot(bit)] = morpthis_app_time_ms() + milliseconds;
	push_pad();
}

static void phys_release(uint32_t bit) {
	pad_phys &= ~bit;
	push_pad();
}

static void clear_input(void) {
	pad_phys = 0;
	pad_touch = 0;
	touch_owner = OWN_NONE;
	turbo_held = false;
	nes_set_turbo(false);
	nes_set_pad(0);
}

static void pulse_tick(morpthis_timer_t *timer, void *user) {
	(void)timer;
	(void)user;
	uint32_t now = morpthis_app_time_ms();
	bool changed = false;
	const uint32_t bits[4] = {NES_A, NES_B, NES_START, NES_SELECT};
	for (int i = 0; i < 4; i++) {
		if ((pad_phys & bits[i]) && time_reached(now, pad_deadline[i])) {
			pad_phys &= ~bits[i];
			changed = true;
		}
	}
	if (turbo_held && time_reached(now, turbo_deadline)) {
		turbo_held = false;
		nes_set_turbo(false);
	}
	if (changed) {
		push_pad();
	}
}

static void update_dpad(int lx, int ly, controls_t c) {
	int dx = lx - c.dcx;
	int dy = ly - c.dcy;
	uint32_t directions = 0;

	int dead = c.dr / 4;
	if (dx * dx + dy * dy > dead * dead) {
		int threshold = c.dr / 3;
		if (dy < -threshold) {
			directions |= NES_UP;
		}
		if (dy > threshold) {
			directions |= NES_DOWN;
		}
		if (dx < -threshold) {
			directions |= NES_LEFT;
		}
		if (dx > threshold) {
			directions |= NES_RIGHT;
		}
	}

	pad_touch = (pad_touch & ~NES_DIRS) | directions;
	push_pad();
}

static bool in_circle(int x, int y, int cx, int cy, int radius) {
	int dx = x - cx;
	int dy = y - cy;
	return dx * dx + dy * dy <= radius * radius;
}

static void release_touch(void) {
	if (touch_owner == OWN_DPAD) {
		pad_touch &= ~NES_DIRS;
	} else if (touch_owner == OWN_A) {
		pad_touch &= ~NES_A;
	} else if (touch_owner == OWN_B) {
		pad_touch &= ~NES_B;
	}
	touch_owner = OWN_NONE;
	push_pad();
}

static void touch_cb(morpthis_touch_action_t action, morpthis_point_t point, void *user) {
	(void)user;
	if (state != ST_GAME) {
		return;
	}

	// Touch arrives in portrait pixels; the controls live in landscape pixels.
	int lx = panel_h - 1 - point.y;
	int ly = point.x;
	controls_t c = controls_geometry();

	switch (action) {
	case MORPTHIS_TOUCH_DOWN:
		if (in_circle(lx, ly, c.pcx, c.pcy, c.pr + 8 * geometry.scale)) {
			touch_owner = OWN_NONE;
			open_pause();
			return;
		}
		if (!touch_controls) {
			touch_owner = OWN_NONE;
			return;
		}
		if (in_circle(lx, ly, c.dcx, c.dcy, c.dr + 6 * geometry.scale)) {
			touch_owner = OWN_DPAD;
			update_dpad(lx, ly, c);
		} else if (in_circle(lx, ly, c.acx, c.acy, c.ar + 5 * geometry.scale)) {
			touch_owner = OWN_A;
			pad_touch |= NES_A;
			push_pad();
		} else if (in_circle(lx, ly, c.bcx, c.bcy, c.br + 5 * geometry.scale)) {
			touch_owner = OWN_B;
			pad_touch |= NES_B;
			push_pad();
		} else {
			touch_owner = OWN_NONE;
		}
		break;

	case MORPTHIS_TOUCH_MOVE:
		if (touch_owner == OWN_DPAD) {
			update_dpad(lx, ly, c);
		}
		break;

	case MORPTHIS_TOUCH_UP:
	case MORPTHIS_TOUCH_CANCEL:
		release_touch();
		break;
	}
}

static void button_cb(morpthis_button_t button, morpthis_input_action_t action, void *user) {
	(void)user;

	if (button == MORPTHIS_BUTTON_BACK) {
		if (action != MORPTHIS_INPUT_PRESS) {
			return;
		}
		switch (state) {
		case ST_GAME:
			morpthis_app_back_handled();
			open_pause();
			break;
		case ST_STARTING:
			morpthis_app_back_handled();
			cancel_start();
			break;
		case ST_PAUSED:
			morpthis_app_back_handled();
			resume_game();
			break;
		case ST_CONTROLS:
			morpthis_app_back_handled();
			morpthis_screen_load(pause_screen);
			state = ST_PAUSED;
			break;
		case ST_LIBRARY:
		case ST_SCANNING:
			morpthis_app_quit();
			break;
		default:
			break;
		}
		return;
	}

	if (button == MORPTHIS_BUTTON_POWER) {
		if (action != MORPTHIS_INPUT_PRESS) {
			return;
		}
		if (state == ST_GAME) {
			open_pause();
		} else if (state == ST_STARTING) {
			cancel_start();
		} else if (state == ST_PAUSED) {
			resume_game();
		} else if (state == ST_CONTROLS) {
			morpthis_screen_load(pause_screen);
			state = ST_PAUSED;
		}
		return;
	}

	if (state != ST_GAME) {
		return;
	}

	bool pressed = action == MORPTHIS_INPUT_PRESS || action == MORPTHIS_INPUT_REPEAT;
	bool released = action == MORPTHIS_INPUT_RELEASE;

	switch (button) {
	case MORPTHIS_BUTTON_NEXT:
		if (pressed) {
			phys_press(NES_SELECT, 140);
		} else if (released) {
			phys_release(NES_SELECT);
		}
		break;
	case MORPTHIS_BUTTON_PREV:
		if (pressed) {
			turbo_held = true;
			turbo_deadline = morpthis_app_time_ms() + 350;
			nes_set_turbo(true);
		} else if (released) {
			turbo_held = false;
			nes_set_turbo(false);
		}
		break;
	case MORPTHIS_BUTTON_PLAY_PAUSE:
		if (pressed) {
			phys_press(NES_START, 140);
		} else if (released) {
			phys_release(NES_START);
		}
		break;
	case MORPTHIS_BUTTON_VOLUME_UP:
		if (pressed) {
			phys_press(NES_A, 120);
		} else if (released) {
			phys_release(NES_A);
		}
		break;
	case MORPTHIS_BUTTON_VOLUME_DOWN:
		if (pressed) {
			phys_press(NES_B, 160);
		} else if (released) {
			phys_release(NES_B);
		}
		break;
	default:
		break;
	}
}

// ---------------------------------------------------------------------------
// screens
// ---------------------------------------------------------------------------

static bool build_library(void) {
	library_screen = morpthis_screen_create("NES");
	if (!library_screen) {
		return false;
	}

	library_back = make_icon_button(library_screen, MORPTHIS_ICON_CHEVRON_LEFT, 4, 4, 40,
									on_library_back);
	library_title = make_label(library_screen, "NES", 52, 8, SIZE_TITLE, false);
	library_count = make_label(library_screen, "Starting...", 52, 38, SIZE_SMALL, true);
	library_info = make_icon_button(library_screen, MORPTHIS_ICON_INFO, 196, 4, 40,
									on_library_info);
	library_refresh = make_icon_button(library_screen, MORPTHIS_ICON_REFRESH, 152, 4, 40,
									  on_library_rescan);

	library_accent = morpthis_container_create(library_screen);
	if (library_accent) {
		morpthis_widget_set_pos(library_accent, 16, 58);
		morpthis_widget_set_size(library_accent, 208, 2);
		morpthis_widget_set_bg_color(library_accent, accent_rgb, 100);
		morpthis_widget_set_radius(library_accent, 1);
	}

	library_list = morpthis_list_create(library_screen);
	if (!library_list) {
		return false;
	}
	morpthis_widget_set_pos(library_list, 8, 66);
	morpthis_widget_set_size(library_list, 224, 246);
	morpthis_list_set_select_cb(library_list, on_rom_selected, NULL);
	show(library_list, false);

	library_spinner = morpthis_spinner_create(library_screen);
	if (library_spinner) {
		morpthis_widget_set_pos(library_spinner, 104, 92);
		morpthis_widget_set_size(library_spinner, 32, 32);
		show(library_spinner, false);
	}
	library_scan_label = make_center_label(library_screen, "Looking for games...", 136, 240,
										   MORPTHIS_SIZE_CONTENT, SIZE_BODY, true, false);
	show(library_scan_label, false);

	library_cart = morpthis_image_create(library_screen, "icon.png");
	if (library_cart) {
		morpthis_widget_set_pos(library_cart, 88, 80);
		morpthis_widget_set_size(library_cart, 64, 64);
		show(library_cart, false);
	}
	library_empty_title = make_center_label(library_screen, "No games yet", 158, 240,
											MORPTHIS_SIZE_CONTENT, SIZE_HERO, false, false);
	library_empty_detail = make_center_label(library_screen,
											 "Copy .nes files into\nApps/merp_data/nes/roms on the card,\nthen tap Rescan.",
											 186, 200, MORPTHIS_SIZE_CONTENT, SIZE_SMALL, true, true);
	show(library_empty_title, false);
	show(library_empty_detail, false);

	library_rescan = morpthis_button_create(library_screen, "Rescan", on_library_rescan, NULL);
	if (!library_rescan) {
		return false;
	}
	morpthis_widget_set_pos(library_rescan, 16, 270);
	morpthis_widget_set_size(library_rescan, 208, 40);
	morpthis_widget_set_text_size(library_rescan, SIZE_BODY);
	show(library_rescan, false);

	(void)library_title;
	(void)library_back;
	return true;
}

static bool build_pause(void) {
	pause_screen = morpthis_screen_create("Paused");
	if (!pause_screen) {
		return false;
	}

	pause_back = make_icon_button(pause_screen, MORPTHIS_ICON_CHEVRON_LEFT, 4, 4, 40,
								  on_pause_back);
	pause_title = make_label(pause_screen, "Paused", 52, 8, SIZE_TITLE, false);
	pause_game = make_label(pause_screen, "", 52, 38, SIZE_SMALL, true);
	if (pause_game) {
		morpthis_widget_set_size(pause_game, 180, MORPTHIS_SIZE_CONTENT);
		morpthis_widget_set_text_scroll(pause_game, true);
	}

	pause_resume = morpthis_button_create(pause_screen, "Resume", on_resume, NULL);
	pause_reset = morpthis_button_create(pause_screen, "Reset", on_reset, NULL);
	pause_sound = morpthis_button_create(pause_screen, "Sound: On", on_sound, NULL);
	pause_controls = morpthis_button_create(pause_screen, "Controls", on_controls, NULL);
	pause_exit = morpthis_button_create(pause_screen, "Exit game", on_exit_game, NULL);
	morpthis_widget_t *rows[5] = {pause_resume, pause_reset, pause_sound, pause_controls, pause_exit};
	for (int i = 0; i < 5; i++) {
		if (!rows[i]) {
			return false;
		}
		morpthis_widget_set_pos(rows[i], 16, 64 + i * 48);
		morpthis_widget_set_size(rows[i], 208, 40);
		morpthis_widget_set_text_size(rows[i], SIZE_BODY);
	}

	(void)pause_back;
	(void)pause_title;
	return true;
}

static morpthis_widget_t *add_control_row(const char *title, const char *value,
										 morpthis_click_cb cb) {
	morpthis_widget_t *row = morpthis_settings_row_create(controls_screen, title, NULL, value,
													  cb ? cb : no_op, NULL);
	if (!row) {
		return NULL;
	}
	morpthis_widget_set_parent(row, controls_scroll);
	return row;
}

static bool build_controls(void) {
	controls_screen = morpthis_screen_create("Controls");
	if (!controls_screen) {
		return false;
	}

	controls_back = make_icon_button(controls_screen, MORPTHIS_ICON_CHEVRON_LEFT, 4, 4, 40,
									 on_controls_back);
	controls_title = make_label(controls_screen, "Controls", 52, 8, SIZE_TITLE, false);

	controls_scroll = morpthis_container_create(controls_screen);
	if (!controls_scroll) {
		return false;
	}
	morpthis_widget_set_pos(controls_scroll, 8, 56);
	morpthis_widget_set_size(controls_scroll, 224, 256);
	morpthis_container_set_layout(controls_scroll, MORPTHIS_LAYOUT_COLUMN,
								  MORPTHIS_ALIGN_START, MORPTHIS_ALIGN_CENTER, 4);
	morpthis_container_set_scrollable(controls_scroll, true);

	controls_touch = add_control_row("Touch overlay", touch_controls ? "On" : "Off",
									 on_touch_controls);
	if (!controls_touch) {
		return false;
	}

	if (!add_control_row("Touch pad", "Move", NULL) ||
		!add_control_row("VOLUME UP / DOWN", "A / B", NULL) ||
		!add_control_row("PLAY/PAUSE", "Start", NULL) ||
		!add_control_row("NEXT", "Select", NULL) ||
		!add_control_row("PREV", "Turbo", NULL) ||
		!add_control_row("POWER", "Pause", NULL)) {
		return false;
	}

	morpthis_widget_t *note = morpthis_label_create(controls_screen,
												   "Hold the player sideways with its top edge on the right. "
												   "The picture is 1:1. The volume rocker is A and B, so "
												   "a finger can stay on the pad while the other hand runs "
												   "and jumps.");
	if (!note) {
		return false;
	}
	morpthis_widget_set_size(note, 208, MORPTHIS_SIZE_CONTENT);
	morpthis_widget_set_text_wrap(note, true);
	morpthis_widget_set_text_dim(note, true);
	morpthis_widget_set_text_size(note, SIZE_SMALL);
	morpthis_widget_set_parent(note, controls_scroll);

	(void)controls_back;
	(void)controls_title;
	return true;
}

static bool build_game(void) {
	if (game_screen) {
		return true;
	}

	panel_w = morpthis_ui_width();
	panel_h = morpthis_ui_height();
	if (panel_w < 200 || panel_h < 280) {
		return false;
	}
	if (!nes_set_geometry(panel_w, panel_h)) {
		return false;
	}
	geometry = *nes_geometry();

	game_screen = morpthis_screen_create("NES");
	if (!game_screen) {
		return false;
	}
	game_canvas = morpthis_canvas_create(game_screen, panel_w, panel_h);
	if (!game_canvas) {
		return false;
	}
	morpthis_widget_set_pos(game_canvas, 0, 0);
	game_pixels = morpthis_canvas_buffer(game_canvas);
	if (!game_pixels) {
		return false;
	}
	memset(game_pixels, 0, (size_t)panel_w * (size_t)panel_h * sizeof(uint16_t));
	morpthis_canvas_invalidate(game_canvas, 0, 0, panel_w, panel_h);
	return true;
}

// ---------------------------------------------------------------------------
// transitions
// ---------------------------------------------------------------------------

static void start_game(int index) {
	if (state != ST_LIBRARY || index < 0 || index >= rom_count) {
		return;
	}

	size_t available = morpthis_app_mem_available();
	if (available > 0 && available < START_MEMORY) {
		morpthis_toast_error("Not enough free memory to start a game.");
		return;
	}

	if (!build_game()) {
		morpthis_toast_error("Could not create the game screen.");
		return;
	}

	snprintf(current_title, sizeof current_title, "%s", roms[index].title);
	snprintf(current_rel, sizeof current_rel, "%s", roms[index].rel);

	clear_input();
	memset(game_pixels, 0, (size_t)panel_w * (size_t)panel_h * sizeof(uint16_t));
	geometry = *nes_geometry();
	draw_overlay();
	morpthis_canvas_invalidate(game_canvas, 0, 0, panel_w, panel_h);

	state = ST_STARTING;
	morpthis_ui_modal_show("Starting", current_title);
	morpthis_ui_modal_progress(-1);
	morpthis_screen_load(game_screen);

	if (!nes_start(current_rel)) {
		char error[160];
		snprintf(error, sizeof error, "%s", nes_last_error());
		morpthis_ui_modal_hide();
		state = ST_LIBRARY;
		morpthis_screen_load(library_screen);
		morpthis_toast_error(error);
	}
}

static void open_pause(void) {
	if (state != ST_GAME) {
		return;
	}
	clear_input();
	state = ST_PAUSED;
	nes_pause(true);
	morpthis_input_set_gameplay(false);
	set_label(pause_game, current_title, SIZE_SMALL, true);
	morpthis_widget_set_text(pause_sound, sound_on ? "Sound: On" : "Sound: Off");
	morpthis_logd("nes", "paused");
	morpthis_screen_load(pause_screen);
}

static void resume_game(void) {
	if (state != ST_PAUSED && state != ST_CONTROLS) {
		return;
	}
	state = ST_GAME;
	morpthis_screen_load(game_screen);
	morpthis_input_set_gameplay(true);
	nes_pause(false);
	morpthis_logd("nes", "resumed");
}

static void exit_to_library(void) {
	clear_input();
	nes_stop();
	morpthis_ui_modal_hide();
	state = ST_LIBRARY;
	morpthis_input_set_gameplay(false);
	morpthis_screen_load(library_screen);
}

static void cancel_start(void) {
	clear_input();
	nes_stop();
	morpthis_ui_modal_hide();
	state = ST_LIBRARY;
	morpthis_input_set_gameplay(false);
	morpthis_screen_load(library_screen);
	morpthis_toast("Game cancelled.");
}

static void fail_to_library(const char *message) {
	char error[160];
	snprintf(error, sizeof error, "%s", message && message[0] ? message : "The emulator stopped.");
	clear_input();
	nes_stop();
	morpthis_ui_modal_hide();
	state = ST_LIBRARY;
	morpthis_input_set_gameplay(false);
	morpthis_screen_load(library_screen);
	morpthis_toast_error(error);
}

#ifdef NES_HOST_TEST
// A headless host cannot show the canvas. NES_DUMP_FRAME writes the game
// screen once, as a BMP, so the picture and its controls can be inspected.
static void dump_frame_bmp(const char *path) {
	FILE *file = fopen(path, "wb");
	if (!file || !game_pixels) {
		return;
	}
	int row_bytes = (panel_w * 3 + 3) & ~3;
	unsigned image_size = (unsigned)row_bytes * (unsigned)panel_h;
	unsigned char header[54] = {0};
	header[0] = 'B';
	header[1] = 'M';
	unsigned file_size = 54u + image_size;
	header[2] = (unsigned char)(file_size & 0xFF);
	header[3] = (unsigned char)((file_size >> 8) & 0xFF);
	header[4] = (unsigned char)((file_size >> 16) & 0xFF);
	header[5] = (unsigned char)((file_size >> 24) & 0xFF);
	header[10] = 54;
	header[14] = 40;
	header[18] = (unsigned char)(panel_w & 0xFF);
	header[19] = (unsigned char)((panel_w >> 8) & 0xFF);
	header[22] = (unsigned char)(panel_h & 0xFF);
	header[23] = (unsigned char)((panel_h >> 8) & 0xFF);
	header[26] = 1;
	header[28] = 24;
	header[34] = (unsigned char)(image_size & 0xFF);
	header[35] = (unsigned char)((image_size >> 8) & 0xFF);
	header[36] = (unsigned char)((image_size >> 16) & 0xFF);
	header[37] = (unsigned char)((image_size >> 24) & 0xFF);
	fwrite(header, 1, sizeof header, file);

	unsigned char *row = calloc((size_t)row_bytes, 1);
	if (!row) {
		fclose(file);
		return;
	}
	for (int y = panel_h - 1; y >= 0; y--) {
		memset(row, 0, (size_t)row_bytes);
		for (int x = 0; x < panel_w; x++) {
			uint16_t colour = game_pixels[(size_t)y * (size_t)panel_w + (size_t)x];
			unsigned r = (colour >> 11) & 0x1F;
			unsigned g = (colour >> 5) & 0x3F;
			unsigned b = colour & 0x1F;
			row[x * 3 + 0] = (unsigned char)((b << 3) | (b >> 2));
			row[x * 3 + 1] = (unsigned char)((g << 2) | (g >> 4));
			row[x * 3 + 2] = (unsigned char)((r << 3) | (r >> 2));
		}
		fwrite(row, 1, (size_t)row_bytes, file);
	}
	free(row);
	fclose(file);
}
#endif

static void on_nes_event(void *arg) {
	(void)arg;
	if (shutting_down) {
		return;
	}

	if (state == ST_PAUSED || state == ST_CONTROLS) {
		// A frame can complete between the pause request and the emulator
		// blocking at a scanline. Copy it so the pending flag is cleared, and
		// keep the canvas current for the resume.
		nes_frame_copy(game_pixels, panel_w);
		return;
	}

	if (state == ST_STARTING) {
		if (nes_frame_copy(game_pixels, panel_w)) {
			morpthis_ui_modal_hide();
			state = ST_GAME;
			morpthis_input_set_gameplay(true);
			refresh_theme();
			draw_overlay();
			morpthis_canvas_invalidate(game_canvas, 0, 0, panel_w, panel_h);
			morpthis_ui_refresh();
			morpthis_logd("nes", "first frame: %dx%d at scale %d", geometry.game_w,
						  geometry.game_h, geometry.scale);
#ifdef NES_HOST_TEST
			const char *dump = getenv("NES_DUMP_FRAME");
			if (dump && dump[0]) {
				dump_frame_bmp(dump);
			}
#endif
		}
		const char *error = nes_last_error();
		if (error && error[0]) {
			fail_to_library(error);
		}
		return;
	}

	if (state == ST_GAME) {
		bool copied = nes_frame_copy(game_pixels, panel_w);
		if (copied) {
			draw_overlay();
			morpthis_canvas_invalidate(game_canvas, 0, 0, panel_w, panel_h);
			morpthis_ui_refresh();
		}
		const char *error = nes_last_error();
		if (error && error[0]) {
			fail_to_library(error);
		}
	}
}

static void post_nes_event(void) {
	morpthis_app_post(on_nes_event, NULL);
}

// ---------------------------------------------------------------------------
// callbacks
// ---------------------------------------------------------------------------

static void on_library_back(morpthis_widget_t *widget, void *user) {
	(void)widget;
	(void)user;
	morpthis_app_quit();
}

static void on_library_info(morpthis_widget_t *widget, void *user) {
	(void)widget;
	(void)user;
	morpthis_ui_notice("Add games",
					   "Copy .nes files into\n"
					   "Apps/merp_data/nes/roms\n"
					   "on the card, then tap the refresh button.\n\n"
					   "ROMs are not included. Most iNES mappers are supported, "
					   "including MMC1/3/5, VRC6/7, Namco 163 and FME-7.");
}

static void on_library_rescan(morpthis_widget_t *widget, void *user) {
	(void)widget;
	(void)user;
	if (state == ST_LIBRARY) {
		start_scan();
	}
}

static void on_rom_selected(morpthis_widget_t *widget, int index, void *user) {
	(void)widget;
	(void)user;
	start_game(index);
}

static void on_pause_back(morpthis_widget_t *widget, void *user) {
	(void)widget;
	(void)user;
	resume_game();
}

static void on_resume(morpthis_widget_t *widget, void *user) {
	(void)widget;
	(void)user;
	resume_game();
}

static void on_reset(morpthis_widget_t *widget, void *user) {
	(void)widget;
	(void)user;
	nes_reset();
	morpthis_toast("Game reset.");
	resume_game();
}

static void on_sound(morpthis_widget_t *widget, void *user) {
	(void)widget;
	(void)user;
	sound_on = !sound_on;
	morpthis_widget_set_text(pause_sound, sound_on ? "Sound: On" : "Sound: Off");
	morpthis_config_set_bool("nes", "sound", sound_on);
	morpthis_config_save();
	nes_set_sound(sound_on);
	morpthis_toast(sound_on ? "Sound on." : "Sound off.");
}

static void on_controls(morpthis_widget_t *widget, void *user) {
	(void)widget;
	(void)user;
	if (state != ST_PAUSED) {
		return;
	}
	state = ST_CONTROLS;
	morpthis_settings_row_set_value(controls_touch, touch_controls ? "On" : "Off");
	morpthis_screen_load(controls_screen);
}

static void on_exit_game(morpthis_widget_t *widget, void *user);

static void exit_confirmed(void *user) {
	(void)user;
	if (state == ST_PAUSED) {
		exit_to_library();
	}
}

static void on_exit_game(morpthis_widget_t *widget, void *user) {
	(void)widget;
	(void)user;
	morpthis_ui_confirm("Exit game",
						"Battery saves are written before returning to the library.",
						"Exit", exit_confirmed, NULL);
}

static void on_controls_back(morpthis_widget_t *widget, void *user) {
	(void)widget;
	(void)user;
	if (state == ST_CONTROLS) {
		state = ST_PAUSED;
		morpthis_screen_load(pause_screen);
	}
}

static void on_touch_controls(morpthis_widget_t *widget, void *user) {
	(void)widget;
	(void)user;
	touch_controls = !touch_controls;
	morpthis_settings_row_set_value(controls_touch, touch_controls ? "On" : "Off");
	morpthis_config_set_bool("nes", "touch", touch_controls);
	morpthis_config_save();
	morpthis_toast(touch_controls ? "Touch overlay on." : "Touch overlay off.");
}

static void no_op(morpthis_widget_t *widget, void *user) {
	(void)widget;
	(void)user;
}

// ---------------------------------------------------------------------------
// lifecycle
// ---------------------------------------------------------------------------

static void load_settings(void) {
	sound_on = morpthis_config_get_bool("nes", "sound", true);
	touch_controls = morpthis_config_get_bool("nes", "touch", true);
}

int morpthis_app_main(const morpthis_app_t *app) {
	(void)app;
	morpthis_logi("nes", "starting");

	load_settings();
	refresh_theme();
	panel_w = morpthis_ui_width();
	panel_h = morpthis_ui_height();
	if (panel_w < 200 || panel_h < 280) {
		morpthis_loge("nes", "the display is too small");
		return 1;
	}

	if (!build_library() || !build_pause() || !build_controls()) {
		morpthis_loge("nes", "could not build the UI");
		return 1;
	}

	morpthis_input_set_button_cb(button_cb, NULL);
	morpthis_input_set_touch_cb(touch_cb, NULL);
	nes_set_notify(post_nes_event);
	nes_set_sound(sound_on);
	pulse_timer = morpthis_timer_create(10, pulse_tick, NULL);
	if (!pulse_timer) {
		morpthis_logw("nes", "the input timer is not available");
	}

	morpthis_screen_load(library_screen);
	start_scan();
	return 0;
}

void morpthis_app_shutdown(void) {
	if (shutting_down) {
		return;
	}
	shutting_down = true;

	scan_quit = true;

	pthread_t finished;
	if (take_scan_thread(&finished)) {
		pthread_join(finished, NULL);
	}

	nes_shutdown();
	morpthis_logi("nes", "stopped");
}
