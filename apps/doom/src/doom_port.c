// doomgeneric platform layer for the Morpthis player.
//
// Doom is an in-process plugin: the SDK owns the display and the input panel,
// and the player's process must survive the game. Doom therefore runs on an
// app-owned thread and talks to the GUI thread through a key queue and an
// RGB565 frame buffer.

#include "doom_port.h"

#include <errno.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <morpthis/app.h>
#include <morpthis/log.h>

#include "doomgeneric.h"
#include "doomkeys.h"
#include "doomstat.h"
#include "i_sound.h"
#include "i_system.h"
#include "i_video.h"
#include "m_controls.h"
#include "w_file.h"
#include "w_wad.h"
#include "z_zone.h"

#define KEY_QUEUE_SIZE 128
#define PULSE_SLOTS 16

// ---------------------------------------------------------------------------
// key queue
// ---------------------------------------------------------------------------

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static uint16_t g_keys[KEY_QUEUE_SIZE];
static unsigned g_key_read;
static unsigned g_key_write;

typedef struct {
	unsigned char key;
	uint32_t deadline;
	bool used;
} pulse_t;

static pulse_t g_pulses[PULSE_SLOTS];

static void queue_key_locked(bool pressed, unsigned char key) {
	unsigned next = (g_key_write + 1) % KEY_QUEUE_SIZE;
	if (next == g_key_read) {
		// A release must never be dropped, so drop the oldest event instead.
		g_key_read = (g_key_read + 1) % KEY_QUEUE_SIZE;
	}
	g_keys[g_key_write] = (uint16_t)((pressed ? 1u : 0u) << 8) | key;
	g_key_write = next;
}

void dg_port_push_key(bool pressed, unsigned char key) {
	pthread_mutex_lock(&g_lock);
	queue_key_locked(pressed, key);
	pthread_mutex_unlock(&g_lock);
}

void dg_port_pulse_key(unsigned char key, uint32_t hold_ms) {
	uint32_t now = morpthis_app_time_ms();

	pthread_mutex_lock(&g_lock);

	for (int i = 0; i < PULSE_SLOTS; i++) {
		if (g_pulses[i].used && g_pulses[i].key == key) {
			// A repeated report of the same physical key extends the hold
			// instead of stacking another down event.
			g_pulses[i].deadline = now + hold_ms;
			pthread_mutex_unlock(&g_lock);
			return;
		}
	}

	int slot = -1;
	for (int i = 0; i < PULSE_SLOTS; i++) {
		if (!g_pulses[i].used) {
			slot = i;
			break;
		}
	}
	if (slot < 0) {
		// All slots are in use: release the oldest key first so it cannot
		// stay down forever.
		slot = 0;
		for (int i = 1; i < PULSE_SLOTS; i++) {
			if ((int32_t)(g_pulses[i].deadline - g_pulses[slot].deadline) < 0) {
				slot = i;
			}
		}
		queue_key_locked(false, g_pulses[slot].key);
	}

	queue_key_locked(true, key);
	g_pulses[slot].key = key;
	g_pulses[slot].deadline = now + hold_ms;
	g_pulses[slot].used = true;
	pthread_mutex_unlock(&g_lock);
}

static void release_due_pulses_locked(uint32_t now) {
	for (int i = 0; i < PULSE_SLOTS; i++) {
		if (!g_pulses[i].used) {
			continue;
		}
		if ((int32_t)(now - g_pulses[i].deadline) >= 0) {
			queue_key_locked(false, g_pulses[i].key);
			g_pulses[i].used = false;
		}
	}
}

int DG_GetKey(int *pressed, unsigned char *doomKey) {
	int result = 0;

	pthread_mutex_lock(&g_lock);
	release_due_pulses_locked(morpthis_app_time_ms());
	if (g_key_read != g_key_write) {
		uint16_t data = g_keys[g_key_read];
		g_key_read = (g_key_read + 1) % KEY_QUEUE_SIZE;
		*pressed = data >> 8;
		*doomKey = (unsigned char)(data & 0xff);
		result = 1;
	}
	pthread_mutex_unlock(&g_lock);

	return result;
}

// ---------------------------------------------------------------------------
// frame handoff
// ---------------------------------------------------------------------------

static int g_panel_w;
static int g_panel_h;
static int g_game_x0;
static int g_game_y0;
static int g_game_w;
static int g_game_h;
static int *g_sx;
static int *g_sy;

static uint16_t *g_frame_front;
static uint16_t *g_frame_back;
static bool g_frame_valid;
static bool g_notify_pending;

static uint16_t g_palette[256];
static bool g_palette_ready;

static dg_port_notify_fn g_notify;
static dg_port_exit_fn g_exit_cb;

static void update_palette(void) {
#ifdef CMAP256
	if (g_palette_ready && !palette_changed) {
		return;
	}
	for (int i = 0; i < 256; i++) {
		uint16_t r = (uint16_t)colors[i].r;
		uint16_t g = (uint16_t)colors[i].g;
		uint16_t b = (uint16_t)colors[i].b;
		g_palette[i] = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
	}
	palette_changed = false;
	g_palette_ready = true;
#endif
}

void DG_DrawFrame(void) {
	if (!DG_ScreenBuffer || !g_frame_back) {
		return;
	}

	update_palette();

	// Doom's 320x200 picture is scaled to the landscape game rectangle and
	// rotated 90 degrees counter-clockwise into the portrait canvas. With the
	// device's top edge on the right, the result reads upright in landscape.
	for (int ly = 0; ly < g_game_h; ly++) {
		const uint8_t *src = (const uint8_t *)DG_ScreenBuffer + g_sy[ly] * DOOMGENERIC_RESX;
		for (int lx = 0; lx < g_game_w; lx++) {
			uint16_t colour = g_palette[src[g_sx[lx]]];
			int x = g_game_y0 + ly;
			int y = g_panel_h - 1 - (g_game_x0 + lx);
			g_frame_back[(size_t)y * (size_t)g_panel_w + (size_t)x] = colour;
		}
	}

	bool post = false;
	pthread_mutex_lock(&g_lock);
	uint16_t *swap = g_frame_front;
	g_frame_front = g_frame_back;
	g_frame_back = swap;
	g_frame_valid = true;
	if (!g_notify_pending) {
		g_notify_pending = true;
		post = true;
	}
	pthread_mutex_unlock(&g_lock);

	if (post && g_notify) {
		g_notify();
	}
}

dg_frame_result_t dg_port_copy_frame(uint16_t *dst, int stride) {
	dg_frame_result_t result = DG_FRAME_NONE;

	pthread_mutex_lock(&g_lock);
	if (g_frame_valid && g_frame_front) {
		for (int y = 0; y < g_panel_h; y++) {
			memcpy(dst + (size_t)y * (size_t)stride, g_frame_front + (size_t)y * (size_t)g_panel_w,
				   (size_t)g_panel_w * sizeof(uint16_t));
		}
		result = DG_FRAME_COPIED;
	}

	// A frame that arrives after this copy is posted by DG_DrawFrame, which
	// sees the flag cleared here. A frame that arrived before it is already in
	// the buffer just copied, so no second callback is needed.
	g_notify_pending = false;
	pthread_mutex_unlock(&g_lock);

	return result;
}

// ---------------------------------------------------------------------------
// game thread
// ---------------------------------------------------------------------------

static pthread_t g_thread;
static bool g_thread_started;
static volatile bool g_quit;
static volatile bool g_gameplay;

static char *g_wad_path;
static char *g_base_dir;

static bool g_fatal_ready;
static jmp_buf g_fatal_jmp;
static char g_error[512];
static bool g_cleanup_done;
static bool g_exit_funcs_ran;

static void doom_cleanup(void) {
	extern lumpinfo_t *lumpinfo;
	extern unsigned int numlumps;

	if (g_cleanup_done) {
		return;
	}
	g_cleanup_done = true;

	// Stop the mixer before the WAD handles and the zone go away. I_Quit()
	// normally does this through S_Shutdown; a fatal error does not.
	I_ShutdownSound();

	// D_AddFile discards the handle, but every lump points at it. Close each
	// distinct WAD so the card's file descriptors do not leak between launches.
	for (unsigned int i = 0; i < numlumps; i++) {
		wad_file_t *wad = lumpinfo[i].wad_file;
		if (wad == NULL) {
			continue;
		}
		bool seen = false;
		for (unsigned int j = 0; j < i; j++) {
			if (lumpinfo[j].wad_file == wad) {
				seen = true;
				break;
			}
		}
		if (!seen) {
			W_CloseFile(wad);
		}
	}

	free(lumpinfo);
	lumpinfo = NULL;
	numlumps = 0;

	// The zone holds Doom's heap; the process stays alive, so give it back.
	Z_Shutdown();
}

static void *doom_thread_main(void *unused) {
	(void)unused;

	extern boolean advancedemo;

	char *argv[8];
	argv[0] = (char *)"doom";
	argv[1] = (char *)"-iwad";
	argv[2] = g_wad_path;
	argv[3] = (char *)"-nomusic";
	argv[4] = (char *)"-nogui";
	argv[5] = (char *)"-mb";
	argv[6] = (char *)"6";
	argv[7] = NULL;

	g_fatal_ready = true;
	if (setjmp(g_fatal_jmp) == 0) {
		doomgeneric_Create(7, argv);

		// Chocolate Doom leaves the next/previous weapon keys unbound. The
		// player's side keys send these, so bind them once the config has
		// been read and before the loop starts.
		key_nextweapon = ']';
		key_prevweapon = '[';

		while (!g_quit) {
			doomgeneric_Tick();

			// Active play is a running level with no menu or pause over it. The
			// app offers the volume keys to the game only in this state;
			// intermission, the title screen, demos and menus get the rocker back.
			g_gameplay = gamestate == GS_LEVEL && !paused && !menuactive &&
						 !demoplayback && !advancedemo;
		}

		// A quit requested by the host (BACK, the X control) ends the loop
		// without going through Doom's menu. Run the normal exit functions
		// here; Doom's own I_Quit already did this. The jump target stays
		// armed in case an exit function fails.
		if (!g_exit_funcs_ran) {
			g_exit_funcs_ran = true;
			I_Quit();
		}
	} else {
		g_quit = true;
	}
	g_fatal_ready = false;

	doom_cleanup();

	if (g_exit_cb) {
		g_exit_cb();
	}
	return NULL;
}

bool dg_port_start(const char *wad_path, const char *base_dir, const dg_geometry_t *geometry,
				   dg_port_notify_fn notify, dg_port_exit_fn exited) {
	if (!wad_path || !base_dir || !geometry) {
		return false;
	}
	if (geometry->panel_w <= 0 || geometry->panel_h <= 0 || geometry->game_w <= 0 ||
		geometry->game_h <= 0) {
		return false;
	}
	// The landscape UI is panel_h wide and panel_w tall; the game rectangle
	// must fit inside it.
	if (geometry->game_x0 < 0 || geometry->game_y0 < 0 ||
		geometry->game_x0 + geometry->game_w > geometry->panel_h ||
		geometry->game_y0 + geometry->game_h > geometry->panel_w) {
		return false;
	}
	if (g_thread_started) {
		return false;
	}

	g_panel_w = geometry->panel_w;
	g_panel_h = geometry->panel_h;
	g_game_x0 = geometry->game_x0;
	g_game_y0 = geometry->game_y0;
	g_game_w = geometry->game_w;
	g_game_h = geometry->game_h;

	// Doom's config code concatenates this string with a file name, so the
	// directory must end with a separator.
	size_t base_len = strlen(base_dir);
	g_base_dir = malloc(base_len + 2);
	if (g_base_dir) {
		snprintf(g_base_dir, base_len + 2, "%s%s", base_dir,
				 (base_len > 0 && base_dir[base_len - 1] == '/') ? "" : "/");
	}
	g_wad_path = strdup(wad_path);
	g_sx = malloc((size_t)g_game_w * sizeof(int));
	g_sy = malloc((size_t)g_game_h * sizeof(int));
	size_t frame_pixels = (size_t)g_panel_w * (size_t)g_panel_h;
	g_frame_front = malloc(frame_pixels * sizeof(uint16_t));
	g_frame_back = malloc(frame_pixels * sizeof(uint16_t));

	if (!g_wad_path || !g_base_dir || !g_sx || !g_sy || !g_frame_front || !g_frame_back) {
		free(g_wad_path);
		free(g_base_dir);
		free(g_sx);
		free(g_sy);
		free(g_frame_front);
		free(g_frame_back);
		g_wad_path = NULL;
		g_base_dir = NULL;
		g_sx = NULL;
		g_sy = NULL;
		g_frame_front = NULL;
		g_frame_back = NULL;
		return false;
	}

	for (int x = 0; x < g_game_w; x++) {
		g_sx[x] = x * DOOMGENERIC_RESX / g_game_w;
	}
	for (int y = 0; y < g_game_h; y++) {
		g_sy[y] = y * DOOMGENERIC_RESY / g_game_h;
	}

	// The letterbox bars stay black: only the game rectangle is written each
	// frame.
	memset(g_frame_front, 0, frame_pixels * sizeof(uint16_t));
	memset(g_frame_back, 0, frame_pixels * sizeof(uint16_t));

	g_frame_valid = false;
	g_notify_pending = false;
	g_palette_ready = false;
	g_notify = notify;
	g_exit_cb = exited;
	g_quit = false;
	g_gameplay = false;
	g_cleanup_done = false;
	g_exit_funcs_ran = false;
	g_error[0] = '\0';

	pthread_mutex_lock(&g_lock);
	g_key_read = g_key_write = 0;
	memset(g_pulses, 0, sizeof(g_pulses));
	pthread_mutex_unlock(&g_lock);

	if (pthread_create(&g_thread, NULL, doom_thread_main, NULL) != 0) {
		free(g_wad_path);
		free(g_base_dir);
		free(g_sx);
		free(g_sy);
		free(g_frame_front);
		free(g_frame_back);
		g_wad_path = NULL;
		g_base_dir = NULL;
		g_sx = NULL;
		g_sy = NULL;
		g_frame_front = NULL;
		g_frame_back = NULL;
		return false;
	}

	g_thread_started = true;
	return true;
}

void dg_port_join(void) {
	// A no-op when the thread has already stopped; if the caller got here
	// early, this makes the join bounded by one game tick.
	g_quit = true;
	if (g_thread_started) {
		pthread_join(g_thread, NULL);
		g_thread_started = false;
	}

	pthread_mutex_lock(&g_lock);
	free(g_frame_front);
	free(g_frame_back);
	g_frame_front = NULL;
	g_frame_back = NULL;
	g_frame_valid = false;
	g_notify_pending = false;
	pthread_mutex_unlock(&g_lock);

	free(g_sx);
	free(g_sy);
	g_sx = NULL;
	g_sy = NULL;

	free(g_wad_path);
	free(g_base_dir);
	g_wad_path = NULL;
	g_base_dir = NULL;
}

void dg_port_request_quit(void) {
	g_quit = true;
}

bool dg_port_running(void) {
	return g_thread_started;
}

bool dg_port_gameplay_active(void) {
	return g_gameplay;
}

bool dg_port_take_error(char *buffer, size_t size) {
	bool have;
	pthread_mutex_lock(&g_lock);
	have = g_error[0] != '\0';
	if (have && buffer && size > 0) {
		snprintf(buffer, size, "%s", g_error);
	}
	pthread_mutex_unlock(&g_lock);
	return have;
}

// ---------------------------------------------------------------------------
// doomgeneric hooks
// ---------------------------------------------------------------------------

void DG_Init(void) {
	if (DG_ScreenBuffer == NULL) {
		I_Error("Could not allocate the Doom screen buffer");
	}
	morpthis_logi("doom", "core started, %dx%d", DOOMGENERIC_RESX, DOOMGENERIC_RESY);
}

void DG_SleepMs(uint32_t ms) {
	struct timespec delay = {
		.tv_sec = (time_t)(ms / 1000),
		.tv_nsec = (long)(ms % 1000) * 1000000L,
	};
	while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
	}
}

uint32_t DG_GetTicksMs(void) {
	return morpthis_app_time_ms();
}

void DG_SetWindowTitle(const char *title) {
	morpthis_logd("doom", "title: %s", title ? title : "");
}

void DG_RequestQuit(void) {
	// Called by Doom's I_Quit after it has run the exit functions.
	g_exit_funcs_ran = true;
	g_quit = true;
}

void DG_FatalError(const char *message) {
	pthread_mutex_lock(&g_lock);
	snprintf(g_error, sizeof(g_error), "%s", message ? message : "Unknown error");
	pthread_mutex_unlock(&g_lock);

	g_quit = true;
	morpthis_loge("doom", "fatal: %s", message ? message : "unknown error");

	if (g_fatal_ready) {
		longjmp(g_fatal_jmp, 1);
	}

	// There is no jump target (a failure during shutdown, after the jump was
	// disarmed). Still let the app leave instead of taking the player down.
	if (g_exit_cb) {
		g_exit_cb();
	}
	pthread_exit(NULL);
}

const char *DG_GetBaseDir(void) {
	return g_base_dir ? g_base_dir : ".";
}
