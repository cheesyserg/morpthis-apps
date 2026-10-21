#ifndef SUBSONIC_COVER_H
#define SUBSONIC_COVER_H

#include <stdbool.h>

#include "subsonic.h"

// One cover download at a time on a worker thread. The album art is written to
// covers/current.jpg in the app's data folder, which is the path the now-playing
// image and the mini-player row read. A newer request cancels an older one;
// the callback gets the cover id so the GUI can ignore a late answer.

typedef struct {
	unsigned generation;
	char id[SUBSONIC_ID_MAX];
	char path[32];
	char error[160];
} cover_result_t;

typedef void (*cover_done_cb)(const cover_result_t *result, void *user);

bool cover_worker_init(cover_done_cb cb, void *user);
void cover_worker_request(unsigned generation, const char *cover_id);
void cover_worker_shutdown(void);

#endif
