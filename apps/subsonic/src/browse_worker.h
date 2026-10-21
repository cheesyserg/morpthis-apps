#ifndef SUBSONIC_BROWSE_H
#define SUBSONIC_BROWSE_H

#include <stdbool.h>

#include "subsonic.h"

// One browse request at a time on a worker thread. The worker owns the result
// until the callback returns; the callback must copy what it keeps.

typedef struct {
	int generation;
	int count;
	bool more;
	char error[192];
	subsonic_entry_t *entries;
} browse_result_t;

typedef void (*browse_done_cb)(const browse_result_t *result, void *user);

bool browse_worker_init(browse_done_cb cb, void *user);
void browse_worker_request(int generation, subsonic_browse_t kind, const char *id, int offset);
void browse_worker_shutdown(void);

#endif
