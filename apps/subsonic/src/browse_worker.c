#include "browse_worker.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <morpthis/morpthis.h>

#include "vendor/http.h"

#define BROWSE_STACK (256 * 1024)

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static pthread_t thread;
static bool started;
static bool quit;

static bool pending;
static int pending_generation;
static subsonic_browse_t pending_kind;
static char pending_id[SUBSONIC_ID_MAX];
static int pending_offset;

static browse_done_cb done_cb;
static void *done_user;

// Runs on the GUI thread. The result and the entry array are freed here, after
// the app has had its chance to copy them.
static void deliver(void *user) {
	browse_result_t *result = user;
	if (result->generation == pending_generation && done_cb) {
		done_cb(result, done_user);
	}
	free(result->entries);
	free(result);
}

static void *worker_main(void *unused) {
	(void)unused;
	for (;;) {
		pthread_mutex_lock(&lock);
		while (!quit && !pending) {
			pthread_cond_wait(&cond, &lock);
		}
		if (quit) {
			pthread_mutex_unlock(&lock);
			break;
		}
		int generation = pending_generation;
		subsonic_browse_t kind = pending_kind;
		char id[SUBSONIC_ID_MAX];
		snprintf(id, sizeof(id), "%s", pending_id);
		int offset = pending_offset;
		pending = false;
		pthread_mutex_unlock(&lock);

		subsonic_entry_t *entries = calloc(SUBSONIC_PAGE_SIZE, sizeof(*entries));
		browse_result_t *result = calloc(1, sizeof(*result));
		if (!entries || !result) {
			free(entries);
			free(result);
			morpthis_loge("subsonic", "no memory for a browse result");
			continue;
		}

		bool more = false;
		int count = subsonic_browse(kind, id, offset, entries, SUBSONIC_PAGE_SIZE, &more,
									result->error, sizeof(result->error));
		result->generation = generation;
		result->count = count;
		result->more = more;
		result->entries = entries;

		if (count < 0) {
			morpthis_logw("subsonic", "browse failed: %s", result->error);
		} else {
			morpthis_logd("subsonic", "browse returned %d entries%s", count, more ? " (more)" : "");
		}
		morpthis_app_post(deliver, result);
	}
	return NULL;
}

bool browse_worker_init(browse_done_cb cb, void *user) {
	done_cb = cb;
	done_user = user;

	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, BROWSE_STACK);
	int rc = pthread_create(&thread, &attr, worker_main, NULL);
	pthread_attr_destroy(&attr);
	started = rc == 0;
	if (!started) {
		morpthis_loge("subsonic", "could not start the browse worker");
	}
	return started;
}

void browse_worker_request(int generation, subsonic_browse_t kind, const char *id, int offset) {
	if (!started) {
		return;
	}
	pthread_mutex_lock(&lock);
	pending = true;
	pending_generation = generation;
	pending_kind = kind;
	snprintf(pending_id, sizeof(pending_id), "%s", id ? id : "");
	pending_offset = offset;
	pthread_cond_signal(&cond);
	pthread_mutex_unlock(&lock);
}

void browse_worker_shutdown(void) {
	if (!started) {
		return;
	}
	pthread_mutex_lock(&lock);
	quit = true;
	pthread_cond_broadcast(&cond);
	pthread_mutex_unlock(&lock);

	// A request may be blocked in connect() or recv(). The socket is shut down
	// from here so the join does not wait out a network timeout; the worker
	// still owns and closes it.
	http_abort_all();
	pthread_join(thread, NULL);
	started = false;
}
