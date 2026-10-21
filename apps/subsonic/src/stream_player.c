#include "stream_player.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include <morpthis/morpthis.h>

#include "vendor/dr_mp3.h"
#include "vendor/http.h"

#define PLAYER_STACK (256 * 1024)
#define READ_TIMEOUT 12
#define DECODE_FRAMES 1152 // one MPEG-1 Layer III frame
#define AUDIO_BUFFER_MS 120

typedef struct {
	pthread_mutex_t lock;
	pthread_cond_t cond;
	pthread_t thread;
	bool started;
	bool quit;

	// A new request stops the old stream and starts this one.
	bool has_request;
	bool abort;
	bool paused;
	unsigned generation;
	stream_track_t request;

	// The stream being opened or read, published so stop/shutdown can shut its
	// socket down. `setup_fd` covers the window before http_stream_open_ex()
	// hands the fd back.
	http_stream_t *http;
	int http_state; // 0 idle, 1 opening, 2 open
	int setup_fd;

	stream_event_cb cb;
	void *user;
} player_t;

static player_t player;

typedef struct {
	unsigned generation;
	stream_event_kind_t kind;
	char message[192];
	stream_track_t track;
} event_t;

static void deliver_event(void *user) {
	event_t *event = user;
	pthread_mutex_lock(&player.lock);
	bool current = event->generation == player.generation;
	stream_event_cb cb = player.cb;
	void *cb_user = player.user;
	pthread_mutex_unlock(&player.lock);
	if (current && cb) {
		cb(event->kind, event->message, &event->track, cb_user);
	}
	free(event);
}

static void post_event(unsigned generation, stream_event_kind_t kind, const char *message,
					   const stream_track_t *track) {
	event_t *event = calloc(1, sizeof(*event));
	if (!event) {
		return;
	}
	event->generation = generation;
	event->kind = kind;
	snprintf(event->message, sizeof(event->message), "%s", message ? message : "");
	if (track) {
		event->track = *track;
	}
	morpthis_app_post(deliver_event, event);
}

// ---------------------------------------------------------------------------
// The socket the worker is using
// ---------------------------------------------------------------------------

static bool observe_socket(void *user, int fd) {
	player_t *p = user;
	pthread_mutex_lock(&p->lock);
	p->setup_fd = fd;
	bool keep = fd < 0 || (!p->abort && !p->quit);
	pthread_mutex_unlock(&p->lock);
	return keep;
}

static bool aborted(unsigned generation) {
	pthread_mutex_lock(&player.lock);
	bool stop = player.abort || player.quit || generation != player.generation;
	pthread_mutex_unlock(&player.lock);
	return stop;
}

// ---------------------------------------------------------------------------
// One track
// ---------------------------------------------------------------------------

typedef struct {
	http_stream_t *http;
	unsigned generation;
	bool io_error;
} source_t;

static size_t read_body(void *user, void *buffer, size_t bytes) {
	source_t *source = user;
	if (aborted(source->generation)) {
		return 0;
	}
	int got = http_stream_read(source->http, buffer, (int)bytes);
	if (got < 0) {
		source->io_error = true;
		return 0;
	}
	return (size_t)got;
}

static void play_one(const stream_track_t *track, unsigned generation) {
	char url[SUBSONIC_URL_MAX];
	char error[192] = "";
	subsonic_quality_t quality = subsonic_quality();
	if (!subsonic_stream_url(track->id, quality, url, sizeof(url), error, sizeof(error))) {
		post_event(generation, STREAM_EVENT_FAILED, error, track);
		return;
	}

	http_stream_t http;
	memset(&http, 0, sizeof(http));
	http.fd = -1;

	pthread_mutex_lock(&player.lock);
	if (player.abort || player.quit || generation != player.generation) {
		pthread_mutex_unlock(&player.lock);
		return;
	}
	// Not published until the handshake is over: stop() can reach the socket
	// through setup_fd while it is opening, and only through `http` once the
	// struct is no longer being initialised by http_stream_open_ex().
	player.http = NULL;
	player.http_state = 1;
	player.setup_fd = -1;
	pthread_mutex_unlock(&player.lock);

	http_stream_options_t options = {
		.socket_cb = observe_socket,
		.socket_user = &player,
		.no_icy = true,
		.no_https_downgrade = true,
	};
	bool opened = http_stream_open_ex(&http, url, READ_TIMEOUT, &options);
	memset(url, 0, sizeof(url));

	pthread_mutex_lock(&player.lock);
	player.setup_fd = -1;
	bool canceled = player.abort || player.quit || generation != player.generation;
	if (canceled || !opened) {
		player.http = NULL;
		player.http_state = 0;
	} else {
		player.http = &http;
		player.http_state = 2;
	}
	pthread_mutex_unlock(&player.lock);

	if (canceled) {
		http_stream_close(&http);
		return;
	}
	if (!opened) {
		const char *why = http_last_error();
		post_event(generation, STREAM_EVENT_FAILED, why ? why : "Could not open the stream.", track);
		http_stream_close(&http);
		return;
	}

	drmp3 *mp3 = calloc(1, sizeof(*mp3));
	if (!mp3) {
		http_stream_close(&http);
		post_event(generation, STREAM_EVENT_FAILED, "Not enough memory for the decoder.", track);
		return;
	}
	source_t source = {.http = &http, .generation = generation, .io_error = false};
	if (!drmp3_init(mp3, read_body, NULL, NULL, NULL, &source, NULL)) {
		drmp3_uninit(mp3);
		free(mp3);
		if (!aborted(generation)) {
			post_event(generation, STREAM_EVENT_FAILED,
					   source.io_error ? "The stream ended before any audio arrived."
									   : "The server did not send an MP3 stream. Enable MP3 transcoding on it.",
					   track);
		}
		goto done;
	}

	int rate = (int)mp3->sampleRate;
	int channels = (int)mp3->channels;
	if (rate <= 0 || channels < 1 || channels > 2 || !morpthis_audio_open(rate, channels, AUDIO_BUFFER_MS)) {
		drmp3_uninit(mp3);
		free(mp3);
		if (!aborted(generation)) {
			post_event(generation, STREAM_EVENT_FAILED, "Could not open the audio output.", track);
		}
		goto done;
	}

	int16_t pcm[DECODE_FRAMES * 2];
	bool failed = false;
	const char *why = NULL;
	for (;;) {
		pthread_mutex_lock(&player.lock);
		while (player.paused && !player.abort && !player.quit && generation == player.generation) {
			pthread_cond_wait(&player.cond, &player.lock);
		}
		bool stop = player.abort || player.quit || generation != player.generation;
		pthread_mutex_unlock(&player.lock);
		if (stop) {
			break;
		}

		drmp3_uint64 got = drmp3_read_pcm_frames_s16(mp3, DECODE_FRAMES, pcm);
		if (got == 0) {
			if (source.io_error) {
				failed = true;
				why = "The stream was interrupted.";
			}
			break;
		}
		size_t wrote = morpthis_audio_write(pcm, (size_t)got);
		if (wrote < (size_t)got) {
			if (!aborted(generation)) {
				failed = true;
				why = "The audio output stopped.";
			}
			break;
		}
	}

	morpthis_audio_close();
	drmp3_uninit(mp3);
	free(mp3);

	if (!aborted(generation)) {
		post_event(generation, failed ? STREAM_EVENT_FAILED : STREAM_EVENT_FINISHED,
				   failed ? why : "", track);
	}

done:
	// Unpublish before closing: a waker must never read http.fd while close is
	// setting it to -1. After this the worker owns the struct alone.
	pthread_mutex_lock(&player.lock);
	if (player.http == &http) {
		player.http = NULL;
		player.http_state = 0;
	}
	pthread_mutex_unlock(&player.lock);
	http_stream_close(&http);
}

static void *worker_main(void *unused) {
	(void)unused;
	for (;;) {
		pthread_mutex_lock(&player.lock);
		while (!player.quit && !player.has_request) {
			pthread_cond_wait(&player.cond, &player.lock);
		}
		if (player.quit) {
			pthread_mutex_unlock(&player.lock);
			break;
		}
		stream_track_t track = player.request;
		unsigned generation = player.generation;
		player.has_request = false;
		player.abort = false;
		pthread_mutex_unlock(&player.lock);

		morpthis_logi("subsonic", "playing \"%s\"", track.title);
		play_one(&track, generation);
	}
	return NULL;
}

// ---------------------------------------------------------------------------
// Public
// ---------------------------------------------------------------------------

bool stream_player_init(stream_event_cb cb, void *user) {
	memset(&player, 0, sizeof(player));
	player.cb = cb;
	player.user = user;
	player.setup_fd = -1;
	pthread_mutex_init(&player.lock, NULL);
	pthread_cond_init(&player.cond, NULL);

	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, PLAYER_STACK);
	int rc = pthread_create(&player.thread, &attr, worker_main, NULL);
	pthread_attr_destroy(&attr);
	player.started = rc == 0;
	if (!player.started) {
		morpthis_loge("subsonic", "could not start the playback worker");
	}
	return player.started;
}

bool stream_player_start(const stream_track_t *track) {
	if (!track || !player.started) {
		return false;
	}
	pthread_mutex_lock(&player.lock);
	player.request = *track;
	player.has_request = true;
	player.abort = true; // stop whatever is running
	player.paused = false;
	player.generation++;
	if (player.http) {
		http_stream_wake(player.http);
	}
	if (player.setup_fd >= 0) {
		shutdown(player.setup_fd, SHUT_RDWR);
	}
	pthread_cond_broadcast(&player.cond);
	pthread_mutex_unlock(&player.lock);
	return true;
}

void stream_player_stop(void) {
	if (!player.started) {
		return;
	}
	pthread_mutex_lock(&player.lock);
	player.has_request = false;
	player.abort = true;
	player.paused = false;
	player.generation++;
	if (player.http) {
		http_stream_wake(player.http);
	}
	if (player.setup_fd >= 0) {
		shutdown(player.setup_fd, SHUT_RDWR);
	}
	pthread_cond_broadcast(&player.cond);
	pthread_mutex_unlock(&player.lock);
}

void stream_player_pause(bool paused) {
	if (!player.started) {
		return;
	}
	pthread_mutex_lock(&player.lock);
	player.paused = paused;
	pthread_cond_broadcast(&player.cond);
	pthread_mutex_unlock(&player.lock);
}

void stream_player_shutdown(void) {
	if (!player.started) {
		return;
	}
	pthread_mutex_lock(&player.lock);
	player.quit = true;
	player.abort = true;
	player.has_request = false;
	if (player.http) {
		http_stream_wake(player.http);
	}
	if (player.setup_fd >= 0) {
		shutdown(player.setup_fd, SHUT_RDWR);
	}
	pthread_cond_broadcast(&player.cond);
	pthread_mutex_unlock(&player.lock);

	// Covers a worker still inside DNS or a TLS handshake, where there is no
	// stream to wake yet. The worker closes its own socket afterwards.
	http_abort_all();
	pthread_join(player.thread, NULL);
	player.started = false;
}
