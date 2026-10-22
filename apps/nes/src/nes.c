#include "nes.h"

#include <morpthis/morpthis.h>

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// The emulator thread itself, in nes_system.cpp.
extern void *nes_emulator_thread(void *arg);

#define NES_ERROR_MAX 160
#define NES_PATH_MAX 512

// ---------------------------------------------------------------------------
// state shared with the emulator thread
// ---------------------------------------------------------------------------

static pthread_mutex_t g_run_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_run_cond = PTHREAD_COND_INITIALIZER;
static bool g_quit;
static bool g_pause;
static bool g_alive;
static bool g_thread_started;
static pthread_t g_thread;

static pthread_mutex_t g_frame_lock = PTHREAD_MUTEX_INITIALIZER;
static uint16_t *g_frame_front;
static uint16_t *g_frame_back;
static bool g_frame_valid;
static bool g_notify_pending;
static nes_notify_fn g_notify;

static pthread_mutex_t g_error_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_error[NES_ERROR_MAX];

static _Atomic uint32_t g_pad;
static _Atomic bool g_reset;
static _Atomic bool g_turbo;
static _Atomic bool g_sound = true;

static bool g_audio_pacing;

static nes_geometry_t g_geo;
static char g_rom_rel[NES_PATH_MAX];
static char g_rom_abs[NES_PATH_MAX];
static char g_save_path[NES_PATH_MAX];

// ---------------------------------------------------------------------------
// geometry and frames
// ---------------------------------------------------------------------------

bool nes_set_geometry(int panel_w, int panel_h) {
	if (panel_w < 240 || panel_h < 240) {
		return false;
	}

	int scale = panel_h / 256;
	int by_width = panel_w / 240;
	if (by_width < scale) {
		scale = by_width;
	}
	if (scale < 1) {
		scale = 1;
	}

	size_t pixels = (size_t)panel_w * (size_t)panel_h;
	uint16_t *front = calloc(pixels, sizeof(uint16_t));
	uint16_t *back = calloc(pixels, sizeof(uint16_t));
	if (!front || !back) {
		free(front);
		free(back);
		return false;
	}

	pthread_mutex_lock(&g_frame_lock);
	free(g_frame_front);
	free(g_frame_back);
	g_frame_front = front;
	g_frame_back = back;
	g_frame_valid = false;
	g_notify_pending = false;
	pthread_mutex_unlock(&g_frame_lock);

	g_geo.panel_w = panel_w;
	g_geo.panel_h = panel_h;
	g_geo.scale = scale;
	g_geo.game_w = 256 * scale;
	g_geo.game_h = 240 * scale;
	g_geo.game_x0 = (panel_h - g_geo.game_w) / 2;
	g_geo.game_y0 = (panel_w - g_geo.game_h) / 2;
	return true;
}

const nes_geometry_t *nes_geometry(void) {
	return &g_geo;
}

void nes_set_notify(nes_notify_fn fn) {
	g_notify = fn;
}

// InfoNES keeps its frame in RGB555 (0RRRRRGGGGGBBBBB), and bit 15 is its
// backdrop flag, not colour. The panel is RGB565, so widen red and green and
// drop the flag; blue is five bits in both.
static inline uint16_t rgb555_to_565(uint16_t colour) {
	unsigned r = (colour >> 10) & 0x1F;
	unsigned g = (colour >> 5) & 0x1F;
	unsigned b = colour & 0x1F;
	unsigned g6 = (g << 1) | (g >> 4);
	return (uint16_t)((r << 11) | (g6 << 5) | b);
}

void nes_backend_frame(const uint16_t *work, int width, int height) {
	if (!work || !g_frame_back || g_geo.scale < 1) {
		return;
	}

	int scale = g_geo.scale;
	int src_w = width < 256 ? width : 256;
	int src_h = height < 240 ? height : 240;
	uint16_t *dst = g_frame_back;

	for (int sy = 0; sy < src_h; sy++) {
		const uint16_t *row = work + (size_t)sy * (size_t)width;
		for (int sx = 0; sx < src_w; sx++) {
			uint16_t colour = rgb555_to_565(row[sx]);
			for (int dy = 0; dy < scale; dy++) {
				int ly = sy * scale + dy;
				int x = g_geo.game_y0 + ly;
				if (x < 0 || x >= g_geo.panel_w) {
					continue;
				}
				for (int dx = 0; dx < scale; dx++) {
					int lx = sx * scale + dx;
					int y = g_geo.panel_h - 1 - (g_geo.game_x0 + lx);
					if (y < 0 || y >= g_geo.panel_h) {
						continue;
					}
					dst[(size_t)y * (size_t)g_geo.panel_w + (size_t)x] = colour;
				}
			}
		}
	}

	bool post = false;
	pthread_mutex_lock(&g_frame_lock);
	uint16_t *swap = g_frame_front;
	g_frame_front = g_frame_back;
	g_frame_back = swap;
	g_frame_valid = true;
	if (!g_notify_pending) {
		g_notify_pending = true;
		post = true;
	}
	pthread_mutex_unlock(&g_frame_lock);

	if (post && g_notify) {
		g_notify();
	}
}

bool nes_frame_copy(uint16_t *dst, int stride) {
	if (!dst) {
		return false;
	}

	bool copied = false;
	pthread_mutex_lock(&g_frame_lock);
	if (g_frame_valid && g_frame_front) {
		for (int y = 0; y < g_geo.panel_h; y++) {
			memcpy(dst + (size_t)y * (size_t)stride,
				   g_frame_front + (size_t)y * (size_t)g_geo.panel_w,
				   (size_t)g_geo.panel_w * sizeof(uint16_t));
		}
		copied = true;
	}
	g_notify_pending = false;
	pthread_mutex_unlock(&g_frame_lock);
	return copied;
}

// ---------------------------------------------------------------------------
// errors
// ---------------------------------------------------------------------------

void nes_backend_error(const char *message) {
	pthread_mutex_lock(&g_error_lock);
	snprintf(g_error, sizeof g_error, "%s", message ? message : "Emulator error.");
	pthread_mutex_unlock(&g_error_lock);

	bool post = false;
	pthread_mutex_lock(&g_frame_lock);
	if (!g_notify_pending) {
		g_notify_pending = true;
		post = true;
	}
	pthread_mutex_unlock(&g_frame_lock);

	if (post && g_notify) {
		g_notify();
	}
}

const char *nes_last_error(void) {
	static char copy[NES_ERROR_MAX];
	pthread_mutex_lock(&g_error_lock);
	memcpy(copy, g_error, sizeof copy);
	pthread_mutex_unlock(&g_error_lock);
	return copy;
}

void nes_clear_error(void) {
	pthread_mutex_lock(&g_error_lock);
	g_error[0] = '\0';
	pthread_mutex_unlock(&g_error_lock);
}

// ---------------------------------------------------------------------------
// the emulator thread
// ---------------------------------------------------------------------------

static void sanitise_stem(const char *relative, char *out, size_t size) {
	const char *base = strrchr(relative, '/');
	base = base ? base + 1 : relative;

	size_t length = strlen(base);
	if (length > 4 && strcasecmp(base + length - 4, ".nes") == 0) {
		length -= 4;
	}

	// Keep the cartridge's name; the card cannot contain a slash in it. This
	// keeps two ROMs whose names differ only in punctuation from sharing a
	// save file.
	size_t j = 0;
	for (size_t i = 0; i < length && j + 1 < size; i++) {
		char c = base[i];
		out[j++] = (c == '/' || c == '\\') ? '_' : c;
	}
	while (j > 0 && out[j - 1] == ' ') {
		j--;
	}
	out[j] = '\0';
	if (j == 0) {
		snprintf(out, size, "game");
	}
}

bool nes_start(const char *relative_path) {
	nes_stop();
	nes_clear_error();

	if (!relative_path || !relative_path[0]) {
		nes_backend_error("No game was selected.");
		return false;
	}

	if (!nes_set_geometry(morpthis_ui_width(), morpthis_ui_height())) {
		nes_backend_error("Not enough memory for the game screen.");
		return false;
	}

	snprintf(g_rom_rel, sizeof g_rom_rel, "%s", relative_path);
	if (!morpthis_fs_resolve(g_rom_rel, g_rom_abs, sizeof g_rom_abs)) {
		nes_backend_error("The game file is no longer on the card.");
		return false;
	}

	char stem[128];
	sanitise_stem(g_rom_rel, stem, sizeof stem);
	morpthis_fs_mkdir("saves");
	snprintf(g_save_path, sizeof g_save_path, "%s/saves/%s.srm",
			 morpthis_fs_data_folder(), stem);

	atomic_store(&g_pad, 0);
	atomic_store(&g_reset, false);
	atomic_store(&g_turbo, false);

	pthread_mutex_lock(&g_frame_lock);
	g_frame_valid = false;
	g_notify_pending = false;
	pthread_mutex_unlock(&g_frame_lock);

	pthread_mutex_lock(&g_run_lock);
	g_quit = false;
	g_pause = false;
	g_alive = true;
	pthread_mutex_unlock(&g_run_lock);

	if (pthread_create(&g_thread, NULL, nes_emulator_thread, g_rom_abs) != 0) {
		pthread_mutex_lock(&g_run_lock);
		g_alive = false;
		pthread_mutex_unlock(&g_run_lock);
		nes_backend_error("Could not start the emulator thread.");
		return false;
	}
	g_thread_started = true;
	return true;
}

void nes_stop(void) {
	if (!g_thread_started) {
		atomic_store(&g_pad, 0);
		return;
	}

	pthread_mutex_lock(&g_run_lock);
	g_quit = true;
	g_pause = false;
	pthread_cond_broadcast(&g_run_cond);
	pthread_mutex_unlock(&g_run_lock);

	pthread_join(g_thread, NULL);
	g_thread_started = false;

	pthread_mutex_lock(&g_run_lock);
	g_alive = false;
	pthread_mutex_unlock(&g_run_lock);

	atomic_store(&g_pad, 0);
	atomic_store(&g_turbo, false);
}

void nes_shutdown(void) {
	nes_stop();

	pthread_mutex_lock(&g_frame_lock);
	free(g_frame_front);
	free(g_frame_back);
	g_frame_front = NULL;
	g_frame_back = NULL;
	g_frame_valid = false;
	g_notify_pending = false;
	pthread_mutex_unlock(&g_frame_lock);

	memset(&g_geo, 0, sizeof g_geo);
}

bool nes_running(void) {
	pthread_mutex_lock(&g_run_lock);
	bool alive = g_alive;
	pthread_mutex_unlock(&g_run_lock);
	return alive;
}

void nes_pause(bool paused) {
	pthread_mutex_lock(&g_run_lock);
	g_pause = paused;
	pthread_cond_broadcast(&g_run_cond);
	pthread_mutex_unlock(&g_run_lock);

	if (paused) {
		morpthis_audio_set_playing(false);
	}
}

bool nes_is_paused(void) {
	pthread_mutex_lock(&g_run_lock);
	bool paused = g_pause;
	pthread_mutex_unlock(&g_run_lock);
	return paused;
}

void nes_reset(void) {
	atomic_store(&g_reset, true);
}

void nes_set_pad(uint32_t pad) {
	atomic_store(&g_pad, pad);
}

void nes_set_turbo(bool on) {
	atomic_store(&g_turbo, on);
	if (on) {
		morpthis_audio_set_playing(false);
	}
}

bool nes_turbo(void) {
	return atomic_load(&g_turbo);
}

void nes_set_sound(bool on) {
	atomic_store(&g_sound, on);
	if (!on) {
		morpthis_audio_set_playing(false);
	}
}

bool nes_sound_enabled(void) {
	return atomic_load(&g_sound);
}

const char *nes_rom_relative(void) {
	return g_rom_rel;
}

const char *nes_save_path(void) {
	return g_save_path;
}

// ---------------------------------------------------------------------------
// called by nes_system.cpp
// ---------------------------------------------------------------------------

uint32_t nes_backend_pad(void) {
	return atomic_load(&g_pad);
}

void nes_backend_set_audio_pacing(bool on) {
	g_audio_pacing = on;
}

bool nes_backend_audio_pacing(void) {
	return g_audio_pacing;
}

void nes_backend_wait(void) {
	pthread_mutex_lock(&g_run_lock);
	while (g_pause && !g_quit) {
		pthread_cond_wait(&g_run_cond, &g_run_lock);
	}
	pthread_mutex_unlock(&g_run_lock);
}

bool nes_backend_should_quit(void) {
	pthread_mutex_lock(&g_run_lock);
	bool quit = g_quit;
	pthread_mutex_unlock(&g_run_lock);
	return quit;
}

bool nes_backend_take_reset(void) {
	return atomic_exchange(&g_reset, false);
}

void nes_backend_thread_done(void) {
	pthread_mutex_lock(&g_run_lock);
	g_alive = false;
	pthread_cond_broadcast(&g_run_cond);
	pthread_mutex_unlock(&g_run_lock);
}

bool nes_backend_sound_wanted(void) {
	return atomic_load(&g_sound);
}

bool nes_backend_paused(void) {
	pthread_mutex_lock(&g_run_lock);
	bool paused = g_pause;
	pthread_mutex_unlock(&g_run_lock);
	return paused;
}

bool nes_backend_turbo(void) {
	return atomic_load(&g_turbo);
}
