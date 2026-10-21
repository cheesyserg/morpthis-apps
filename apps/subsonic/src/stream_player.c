#include "stream_player.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>

#include <morpthis/morpthis.h>

#include "vendor/dr_mp3.h"
#include "vendor/dr_flac.h"
#include "vendor/http.h"

#define PLAYER_STACK (256 * 1024)
#define READ_TIMEOUT 12
#define DECODE_FRAMES 1152 // one MPEG-1 Layer III frame
// 0 asks for the player's usual frame-based depth (8 x 4096 frames), the same
// sizing local playback uses. A time-based request such as 120 ms made this
// device's ALSA reject the HW parameters (buffer 120000 us / period 30000 us,
// EINVAL); the deep frame-based buffer is also the better fit for a network
// stream that can stall.
#define AUDIO_BUFFER_MS 0

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

	int position_ms;
	int duration_ms;
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

static size_t flac_read(void *user, void *buffer, size_t bytes) { return read_body(user, buffer, bytes); }

// dr_flac insists on a seek callback even when the stream is only read forward.
// Everything it does at open time is either a forward skip (metadata, ID3) or an
// attempt to load a seektable; refusing the latter is fine, dr_flac pretends the
// stream has none and starts at the first frame.
static drflac_bool32 flac_seek(void *user, int offset, drflac_seek_origin origin) {
	source_t *source = user;
	if (origin == DRFLAC_SEEK_END || origin == DRFLAC_SEEK_SET || offset < 0) {
		return DRFLAC_FALSE;
	}
	unsigned char scratch[2048];
	while (offset > 0) {
		size_t want = (size_t)offset < sizeof(scratch) ? (size_t)offset : sizeof(scratch);
		size_t got = read_body(source, scratch, want);
		if (got == 0) {
			return DRFLAC_FALSE;
		}
		offset -= (int)got;
	}
	return DRFLAC_TRUE;
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

	bool flac_stream = track->format == SUBSONIC_FORMAT_FLAC;
	if (track->format == SUBSONIC_FORMAT_OPUS) {
		post_event(generation, STREAM_EVENT_FAILED,
				   "This app does not decode Opus. Choose MP3 or FLAC on the server.", track);
		goto done;
	}
	if (track->format == SUBSONIC_FORMAT_RAW) {
		if (strcasecmp(track->suffix, "flac") == 0) {
			flac_stream = true;
		} else if (strcasecmp(track->suffix, "mp3") == 0) {
			flac_stream = false;
		} else {
			char why[160];
			snprintf(why, sizeof(why),
					 "This app cannot decode a raw %s; choose MP3 or FLAC transcoding.",
					 track->suffix[0] ? track->suffix : "file");
			post_event(generation, STREAM_EVENT_FAILED, why, track);
			goto done;
		}
	}

	drmp3 *mp3 = NULL;
	drflac *flac = NULL;
	source_t source = {.http = &http, .generation = generation, .io_error = false};
	int rate;
	int channels;
	int source_bits = 16;
	if (flac_stream) {
		flac = drflac_open(flac_read, flac_seek, NULL, &source, NULL);
		if (!flac) {
			if (!aborted(generation)) {
				post_event(generation, STREAM_EVENT_FAILED,
						   source.io_error ? "The stream ended before any audio arrived."
										  : (track->format == SUBSONIC_FORMAT_RAW
												 ? "Original quality: the server did not send a FLAC file this app can read. Choose MP3 or FLAC transcoding."
												 : "The server did not send a FLAC stream. Enable FLAC transcoding on it."),
						   track);
			}
			goto done;
		}
		rate = (int)flac->sampleRate;
		channels = (int)flac->channels;
		source_bits = (int)flac->bitsPerSample;
	} else {
		mp3 = calloc(1, sizeof(*mp3));
		if (!mp3) {
			post_event(generation, STREAM_EVENT_FAILED, "Not enough memory for the decoder.", track);
			goto done;
		}
		if (!drmp3_init(mp3, read_body, NULL, NULL, NULL, &source, NULL)) {
			drmp3_uninit(mp3);
			free(mp3);
			if (!aborted(generation)) {
				post_event(generation, STREAM_EVENT_FAILED,
						   source.io_error ? "The stream ended before any audio arrived."
										  : (track->format == SUBSONIC_FORMAT_RAW
												 ? "Original quality: the server did not send an MP3 file this app can read. Choose MP3 or FLAC transcoding."
												 : "The server did not send an MP3 stream. Enable MP3 transcoding on it."),
						   track);
			}
			goto done;
		}
		rate = (int)mp3->sampleRate;
		channels = (int)mp3->channels;
	}

	// Hi-res follows the source, exactly as the player's own decoder does: a
	// FLAC deeper than 16 bits is written as S32 left-justified, which the
	// built-in DAC consumes as S24 and plays bit-perfect. MP3 and 16-bit FLAC
	// stay at 16 bits, where there is nothing to preserve. If the device
	// refuses S32, fall back to 16 rather than leaving a hi-res track silent.
	int out_bits = source_bits > 16 ? 32 : 16;
	if (rate <= 0 || channels < 1 || channels > 2) {
		out_bits = 0;
	}
	if (out_bits && !morpthis_audio_open_ex(rate, channels, out_bits, AUDIO_BUFFER_MS)) {
		if (out_bits == 32) {
			morpthis_logw("subsonic", "the device refused S32; playing at 16 bits");
			out_bits = 16;
		}
		if (out_bits == 16 && !morpthis_audio_open_ex(rate, channels, 16, AUDIO_BUFFER_MS)) {
			out_bits = 0;
		}
	}
	if (!out_bits) {
		if (flac) {
			drflac_close(flac);
		} else {
			drmp3_uninit(mp3);
			free(mp3);
		}
		if (!aborted(generation)) {
			post_event(generation, STREAM_EVENT_FAILED, "Could not open the audio output.", track);
		}
		goto done;
	}

	// Now playing, best effort. The GUI thread hands this to the action worker:
	// a scrobble must never hold up the thread that feeds the DAC.
	morpthis_logi("subsonic", "%s %d-bit at %d Hz, output %d-bit", flac_stream ? "FLAC" : "MP3",
				  source_bits, rate, out_bits);
	pthread_mutex_lock(&player.lock);
	player.position_ms = 0;
	player.duration_ms = track->duration_ms;
	bool start_paused = player.paused;
	pthread_mutex_unlock(&player.lock);
	morpthis_audio_set_playing(!start_paused);
	post_event(generation, STREAM_EVENT_STARTED, "", track);

	// One union, because a track is either 16-bit or 32-bit for its whole life.
	union {
		int16_t s16[DECODE_FRAMES * 2];
		int32_t s32[DECODE_FRAMES * 2];
	} pcm;
	uint64_t frames_played = 0;
	uint64_t next_progress = (uint64_t)rate;
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

		uint64_t got;
		if (out_bits == 32) {
			got = drflac_read_pcm_frames_s32(flac, DECODE_FRAMES, pcm.s32);
		} else if (flac_stream) {
			got = drflac_read_pcm_frames_s16(flac, DECODE_FRAMES, pcm.s16);
		} else {
			got = drmp3_read_pcm_frames_s16(mp3, DECODE_FRAMES, pcm.s16);
		}
		if (got == 0) {
			if (source.io_error) {
				failed = true;
				why = "The stream was interrupted.";
			}
			break;
		}
		size_t wrote = morpthis_audio_write_pcm(&pcm, (size_t)got);
		if (wrote < (size_t)got) {
			if (!aborted(generation)) {
				failed = true;
				why = "The audio output stopped.";
			}
			break;
		}
		frames_played += got;
		if (frames_played >= next_progress) {
			int position = (int)(frames_played * 1000u / (uint64_t)rate);
			pthread_mutex_lock(&player.lock);
			player.position_ms = position;
			pthread_mutex_unlock(&player.lock);
			post_event(generation, STREAM_EVENT_PROGRESS, "", track);
			next_progress += (uint64_t)rate;
		}
	}

	morpthis_audio_close();
	if (flac) {
		drflac_close(flac);
	} else {
		drmp3_uninit(mp3);
		free(mp3);
	}

	// Submission is the half that counts; only for a real listen (thirty
	// seconds), never for a stop pressed two seconds in. Posted with the current
	// generation so a deliberate stop after the mark still reaches the worker.
	if (rate > 0 && frames_played * 1000u / (uint64_t)rate >= 30000u) {
		pthread_mutex_lock(&player.lock);
		unsigned delivery = player.generation;
		pthread_mutex_unlock(&player.lock);
		post_event(delivery, STREAM_EVENT_LISTENED, "", track);
	}

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
	// The page switches to the new title before the handshake finishes; a
	// position left over from the previous track would be shown against it.
	player.position_ms = 0;
	player.duration_ms = track->duration_ms;
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

	// The worker closes the output when it notices the abort; the status bar
	// should not claim sound during that window.
	morpthis_audio_set_playing(false);
}

void stream_player_pause(bool paused) {
	if (!player.started) {
		return;
	}
	pthread_mutex_lock(&player.lock);
	player.paused = paused;
	pthread_cond_broadcast(&player.cond);
	pthread_mutex_unlock(&player.lock);

	// The status bar follows the user's intent now, not one buffer later.
	morpthis_audio_set_playing(!paused);
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

int stream_player_position_ms(void) {
	pthread_mutex_lock(&player.lock);
	int position = player.position_ms;
	pthread_mutex_unlock(&player.lock);
	return position;
}

int stream_player_duration_ms(void) {
	pthread_mutex_lock(&player.lock);
	int duration = player.duration_ms;
	pthread_mutex_unlock(&player.lock);
	return duration;
}
