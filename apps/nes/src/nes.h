#ifndef NES_H
#define NES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The geometry of the game picture inside the portrait panel. The UI and the
// emulator must agree on it: the UI draws the touch controls on the canvas and
// the emulator rotates the NES frame into the same rectangle.
typedef struct {
	int panel_w;  // portrait canvas width
	int panel_h;  // portrait canvas height
	int scale;    // integer scale of the 256x240 picture
	int game_x0;  // left edge in landscape coordinates
	int game_y0;  // top edge in landscape coordinates
	int game_w;   // 256 * scale
	int game_h;   // 240 * scale
} nes_geometry_t;

typedef void (*nes_notify_fn)(void);

// Allocates the frame buffers and fixes the geometry. Safe to call again with
// the same size; frees the old buffers first. Returns false when the panel is
// too small or the allocation failed.
bool nes_set_geometry(int panel_w, int panel_h);
const nes_geometry_t *nes_geometry(void);

// Called from the emulator thread when a frame or an error is ready. The
// callback must be thread-safe; the UI uses morpthis_app_post().
void nes_set_notify(nes_notify_fn fn);

// Loads a ROM named relative to the app (for example "roms/Game.nes") on a new
// emulator thread. Stops any previous run first. False when the file cannot be
// resolved or the thread cannot be created; nes_last_error() says why.
bool nes_start(const char *relative_path);
void nes_stop(void);
void nes_shutdown(void);
bool nes_running(void);

// Pausing takes effect at the emulator's next scanline. The PCM output is left
// open but silent; the app calls morpthis_audio_set_playing(false).
void nes_pause(bool paused);
bool nes_is_paused(void);

// Requests a reset; the emulator thread performs it at the next frame.
void nes_reset(void);

void nes_set_pad(uint32_t pad);
void nes_set_turbo(bool on);
bool nes_turbo(void);
void nes_set_sound(bool on);
bool nes_sound_enabled(void);

// Copies the latest completed frame into `dst` (stride in pixels). Returns
// true when a new frame was available. Clears the frame's notification flag.
bool nes_frame_copy(uint16_t *dst, int stride);

const char *nes_last_error(void);
void nes_clear_error(void);

const char *nes_rom_relative(void);
const char *nes_save_path(void);

// ---------------------------------------------------------------------------
// Hooks used by nes_system.cpp. Not for the UI.
// ---------------------------------------------------------------------------

void nes_backend_frame(const uint16_t *work, int width, int height);
void nes_backend_error(const char *message);
uint32_t nes_backend_pad(void);
void nes_backend_set_audio_pacing(bool on);
bool nes_backend_audio_pacing(void);
void nes_backend_wait(void);
bool nes_backend_should_quit(void);
bool nes_backend_take_reset(void);
void nes_backend_thread_done(void);
bool nes_backend_sound_wanted(void);
bool nes_backend_paused(void);
bool nes_backend_turbo(void);

#ifdef __cplusplus
}
#endif

#endif /* NES_H */
