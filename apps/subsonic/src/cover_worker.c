#include "cover_worker.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include <morpthis/morpthis.h>

#include "vendor/http.h"

#define COVER_STACK (128 * 1024)
#define COVER_TIMEOUT 12
#define COVER_MAX_BYTES (2 * 1024 * 1024)
#define COVER_FILE "covers/current.jpg"

typedef struct {
	pthread_mutex_t lock;
	pthread_cond_t cond;
	pthread_t thread;
	bool started;
	bool quit;

	bool pending;
	unsigned pending_generation;
	char pending_id[SUBSONIC_ID_MAX];

	http_stream_t *http;
	int setup_fd;

	cover_done_cb cb;
	void *user;
} worker_t;

static worker_t worker;

static void deliver(void *user) {
	cover_result_t *result = user;
	pthread_mutex_lock(&worker.lock);
	bool current = result->generation == worker.pending_generation;
	cover_done_cb cb = worker.cb;
	void *cb_user = worker.user;
	pthread_mutex_unlock(&worker.lock);
	if (current && cb) {
		cb(result, cb_user);
	}
	free(result);
}

static void post_result(unsigned generation, const char *id, const char *path, const char *error) {
	cover_result_t *result = calloc(1, sizeof(*result));
	if (!result) {
		return;
	}
	result->generation = generation;
	snprintf(result->id, sizeof(result->id), "%s", id ? id : "");
	snprintf(result->path, sizeof(result->path), "%s", path ? path : "");
	snprintf(result->error, sizeof(result->error), "%s", error ? error : "");
	morpthis_app_post(deliver, result);
}

static bool aborted(unsigned generation) {
	pthread_mutex_lock(&worker.lock);
	bool stop = worker.quit || generation != worker.pending_generation || worker.pending;
	pthread_mutex_unlock(&worker.lock);
	return stop;
}

static bool observe_socket(void *user, int fd) {
	worker_t *w = user;
	pthread_mutex_lock(&w->lock);
	w->setup_fd = fd;
	bool keep = fd < 0 || (!w->quit && !w->pending);
	pthread_mutex_unlock(&w->lock);
	return keep;
}

// True when the downloaded bytes are a picture the player can decode. A server
// behind a captive portal answers 200 with an HTML login page, and that should
// leave the cover blank rather than replacing the last good one with garbage.
static bool looks_like_image(const unsigned char *data, size_t size) {
	if (size < 4) {
		return false;
	}
	if (data[0] == 0xFF && data[1] == 0xD8) {
		return true; // JPEG
	}
	if (data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G') {
		return true;
	}
	if (data[0] == 'G' && data[1] == 'I' && data[2] == 'F') {
		return true;
	}
	if (data[0] == 'R' && data[1] == 'I' && data[2] == 'F' && data[3] == 'F') {
		return true; // WebP (RIFF)
	}
	return false;
}

static void fetch(unsigned generation, const char *id) {
	char url[SUBSONIC_URL_MAX];
	char error[160] = "";
	if (!subsonic_cover_url(id, 300, url, sizeof(url), error, sizeof(error))) {
		post_result(generation, id, "", error);
		return;
	}

	http_stream_t http;
	memset(&http, 0, sizeof(http));
	http.fd = -1;

	pthread_mutex_lock(&worker.lock);
	if (worker.quit || generation != worker.pending_generation) {
		pthread_mutex_unlock(&worker.lock);
		memset(url, 0, sizeof(url));
		return;
	}
	worker.http = NULL;
	worker.setup_fd = -1;
	pthread_mutex_unlock(&worker.lock);

	http_stream_options_t options = {
		.socket_cb = observe_socket,
		.socket_user = &worker,
		.no_icy = true,
		.no_https_downgrade = true,
	};
	bool opened = http_stream_open_ex(&http, url, COVER_TIMEOUT, &options);
	memset(url, 0, sizeof(url));

	pthread_mutex_lock(&worker.lock);
	worker.setup_fd = -1;
	bool canceled = worker.quit || generation != worker.pending_generation || worker.pending;
	if (canceled || !opened) {
		worker.http = NULL;
	} else {
		worker.http = &http;
	}
	pthread_mutex_unlock(&worker.lock);

	if (canceled) {
		http_stream_close(&http);
		return;
	}
	if (!opened) {
		const char *why = http_last_error();
		post_result(generation, id, "", why ? why : "Could not reach the server for the cover.");
		http_stream_close(&http);
		return;
	}

	morpthis_fs_mkdir("covers"); // EEXIST is the normal answer
	morpthis_file_t *file = morpthis_fs_open(COVER_FILE, MORPTHIS_FS_WRITE);
	if (!file) {
		post_result(generation, id, "", "Could not create the cover file.");
		goto done;
	}

	unsigned char buffer[8192];
	unsigned char head[4];
	size_t head_size = 0;
	size_t total = 0;
	bool failed = false;
	const char *why = NULL;
	for (;;) {
		if (aborted(generation)) {
			failed = true;
			why = "";
			break;
		}
		int got = http_stream_read(&http, buffer, sizeof(buffer));
		if (got < 0) {
			failed = true;
			why = "The cover download was interrupted.";
			break;
		}
		if (got == 0) {
			break;
		}
		if (total + (size_t)got > COVER_MAX_BYTES) {
			failed = true;
			why = "The cover image is too large.";
			break;
		}
		if (head_size < sizeof(head)) {
			size_t take = sizeof(head) - head_size;
			if (take > (size_t)got) {
				take = (size_t)got;
			}
			memcpy(head + head_size, buffer, take);
			head_size += take;
		}
		if (morpthis_fs_write(file, buffer, (size_t)got) != (size_t)got) {
			failed = true;
			why = "Could not write the cover to the card.";
			break;
		}
		total += (size_t)got;
	}

	morpthis_fs_close(file);
	if (!failed && total == 0) {
		failed = true;
		why = "The server sent an empty cover.";
	}
	if (!failed && !looks_like_image(head, head_size)) {
		failed = true;
		why = "The server did not send an image for the cover.";
	}

	if (failed) {
		morpthis_fs_remove(COVER_FILE);
		post_result(generation, id, "", why && why[0] ? why : "The cover was canceled.");
	} else {
		post_result(generation, id, COVER_FILE, "");
	}

done:
	pthread_mutex_lock(&worker.lock);
	if (worker.http == &http) {
		worker.http = NULL;
	}
	pthread_mutex_unlock(&worker.lock);
	http_stream_close(&http);
}

static void *worker_main(void *unused) {
	(void)unused;
	for (;;) {
		pthread_mutex_lock(&worker.lock);
		while (!worker.quit && !worker.pending) {
			pthread_cond_wait(&worker.cond, &worker.lock);
		}
		if (worker.quit) {
			pthread_mutex_unlock(&worker.lock);
			break;
		}
		unsigned generation = worker.pending_generation;
		char id[SUBSONIC_ID_MAX];
		snprintf(id, sizeof(id), "%s", worker.pending_id);
		worker.pending = false;
		pthread_mutex_unlock(&worker.lock);

		fetch(generation, id);
	}
	return NULL;
}

bool cover_worker_init(cover_done_cb cb, void *user) {
	memset(&worker, 0, sizeof(worker));
	worker.cb = cb;
	worker.user = user;
	worker.setup_fd = -1;
	pthread_mutex_init(&worker.lock, NULL);
	pthread_cond_init(&worker.cond, NULL);

	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, COVER_STACK);
	int rc = pthread_create(&worker.thread, &attr, worker_main, NULL);
	pthread_attr_destroy(&attr);
	worker.started = rc == 0;
	return worker.started;
}

void cover_worker_request(unsigned generation, const char *cover_id) {
	if (!worker.started) {
		return;
	}
	pthread_mutex_lock(&worker.lock);
	// The generation moves even for a track with no cover: that is how an
	// in-flight download for the previous track is cancelled instead of
	// finishing into covers/current.jpg after the page stopped showing it.
	worker.pending_generation = generation;
	if (cover_id && cover_id[0]) {
		snprintf(worker.pending_id, sizeof(worker.pending_id), "%s", cover_id);
		worker.pending = true;
	} else {
		worker.pending_id[0] = '\0';
		worker.pending = false;
	}
	if (worker.http) {
		http_stream_wake(worker.http);
	}
	if (worker.setup_fd >= 0) {
		shutdown(worker.setup_fd, SHUT_RDWR);
	}
	pthread_cond_broadcast(&worker.cond);
	pthread_mutex_unlock(&worker.lock);
}

void cover_worker_shutdown(void) {
	if (!worker.started) {
		return;
	}
	pthread_mutex_lock(&worker.lock);
	worker.quit = true;
	worker.pending = false;
	if (worker.http) {
		http_stream_wake(worker.http);
	}
	if (worker.setup_fd >= 0) {
		shutdown(worker.setup_fd, SHUT_RDWR);
	}
	pthread_cond_broadcast(&worker.cond);
	pthread_mutex_unlock(&worker.lock);

	http_abort_all();
	pthread_join(worker.thread, NULL);
	worker.started = false;
}
