#ifndef DOOM_MUSIC_H
#define DOOM_MUSIC_H

#include <stdint.h>

// Renders the next `frames` stereo frames of OPL music into `stereo`.
// Silence when no song is playing or the module is shut down.
void dg_opl_render(int16_t *stereo, int frames);

#endif /* DOOM_MUSIC_H */
