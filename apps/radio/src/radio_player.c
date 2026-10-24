#include "radio_player.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <morpthis/morpthis.h>

#include "decoders/aacdec.h"
#include "decoders/dr_mp3.h"
#include "hls.h"
#include "http.h"

// The transport is deliberately in two threads: one owns the socket and fills
// a bounded ring, the other decodes out of it and blocks in the SDK's PCM
// write, which is what paces a live stream to real time. Without the first
// thread a network stall stops the decode loop and the card runs dry for its
// whole buffer; with it the ring is what absorbs the network.

#define RING_BYTES (256 * 1024)
#define PREFILL_BYTES (96 * 1024)
#define PREFILL_MS 3000
#define READ_TIMEOUT_SECS 12
#define INPUT_BYTES (32 * 1024)

// fdk-aac's frame is at most 2048 sample frames and can carry eight channels.
// The card is always opened for two; anything wider is folded down below.
#define PCM_FRAMES 2048
#define PCM_SAMPLES (PCM_FRAMES * 8)
#define STEREO_SAMPLES (PCM_FRAMES * 2)

#define TITLE_MARKS 4
#define PLAYLIST_MAX_BYTES (64 * 1024)
#define PLAYLIST_MAX_HOPS 3
#define PLAY_MAX_RETRIES 3
#define PLAY_RETRY_MS 2000
#define NO_AUDIO_TIMEOUT_MS 12000
#define ADTS_CHAIN_PROOF 3

// ---------------------------------------------------------------------------
// The ring
// ---------------------------------------------------------------------------

typedef struct {
	long long at; // bytes handed to the decoder when this becomes current
	char title[256];
} title_mark_t;

typedef struct {
	unsigned char data[RING_BYTES];
	int head;
	int level;
	long long written;
	long long taken;

	bool ended;
	bool stop;

	http_stream_t *stream;
	hls_t *hls;

	title_mark_t marks[TITLE_MARKS];
	int mark_count;

	pthread_mutex_t lock;
	pthread_cond_t filled;
	pthread_cond_t drained;
	pthread_t thread;
	bool thread_valid;
} ring_t;

static ring_t ring = {
	.lock = PTHREAD_MUTEX_INITIALIZER,
	.filled = PTHREAD_COND_INITIALIZER,
	.drained = PTHREAD_COND_INITIALIZER,
};

// ---------------------------------------------------------------------------
// Shared state
// ---------------------------------------------------------------------------

typedef enum {
	CMD_NONE = 0,
	CMD_PLAY,
	CMD_STOP,
} command_t;

static pthread_mutex_t ctl = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ctl_wake = PTHREAD_COND_INITIALIZER;
static pthread_t player_thread;
static bool player_started;
static bool quit;

static unsigned generation;
static command_t command;
static bool pause_requested;
static radio_station_t pending;
static radio_now_t now;

static pthread_mutex_t stream_lock = PTHREAD_MUTEX_INITIALIZER;
static http_stream_t *live_stream;
static hls_t *live_hls;
static int setup_fd = -1;

static bool output_open;
static int output_rate;
static bool holds_set;

static int16_t pcm[PCM_SAMPLES];
static int16_t stereo[STEREO_SAMPLES];
static unsigned char input[INPUT_BYTES];

static void bump_locked(void) { now.serial++; }

static bool abort_locked(unsigned mine) {
	return quit || mine != generation || pause_requested || command == CMD_STOP;
}

static bool abort_requested(unsigned mine) {
	pthread_mutex_lock(&ctl);
	bool stop = abort_locked(mine);
	pthread_mutex_unlock(&ctl);
	return stop;
}

static void set_state_locked(radio_player_state_t state, const char *error) {
	now.state = state;
	if (error) {
		// Callers pass now.error itself when it already holds the reason;
		// snprintf() with overlapping source and destination is undefined.
		char text[sizeof(now.error)];
		snprintf(text, sizeof(text), "%s", error);
		memcpy(now.error, text, sizeof(text));
	} else if (state != RADIO_PLAYER_ERROR) {
		now.error[0] = '\0';
	}
	bump_locked();
}

static void set_holds(bool on) {
	pthread_mutex_lock(&ctl);
	bool changed = holds_set != on;
	holds_set = on;
	pthread_mutex_unlock(&ctl);
	if (!changed) {
		return;
	}
	if (on) {
		// NETWORK implies AWAKE and keeps the radio up; this is the explicit
		// statement the power manager wants for a stream, since a raw PCM
		// write on its own does not imply it.
		morpthis_power_set_holds(MORPTHIS_POWER_AWAKE | MORPTHIS_POWER_NETWORK);
	} else {
		morpthis_power_set_holds(MORPTHIS_POWER_NONE);
	}
}

// Tells the player's status bar and control centre what this app is playing.
// `playing` false keeps the station on the card but shows it paused.
static void publish_track(bool playing) {
	radio_station_t station;
	char title[sizeof(now.title)];
	pthread_mutex_lock(&ctl);
	station = now.station;
	snprintf(title, sizeof(title), "%s", now.title);
	pthread_mutex_unlock(&ctl);

	if (!station.url[0]) {
		morpthis_audio_set_track(NULL, false);
		return;
	}

	morpthis_track_t track;
	memset(&track, 0, sizeof(track));
	snprintf(track.title, sizeof(track.title), "%.*s", (int)sizeof(track.title) - 1, title);
	if (strcmp(title, station.name) != 0) {
		snprintf(track.artist, sizeof(track.artist), "%.*s", (int)sizeof(track.artist) - 1, station.name);
	} else {
		snprintf(track.artist, sizeof(track.artist), "Internet radio");
	}
	morpthis_audio_set_track(&track, playing);
}

// ---------------------------------------------------------------------------
// The ring's filling thread
// ---------------------------------------------------------------------------

static void ring_note_title(ring_t *b) {
	if (!b->stream || !b->stream->title_changed) {
		return;
	}
	b->stream->title_changed = false;

	int slot = b->mark_count < TITLE_MARKS ? b->mark_count++ : TITLE_MARKS - 1;
	b->marks[slot].at = b->written;
	snprintf(b->marks[slot].title, sizeof(b->marks[slot].title), "%s", b->stream->stream_title);
}

static void *ring_main(void *unused) {
	(void)unused;
	for (;;) {
		pthread_mutex_lock(&ring.lock);
		while (!ring.stop && ring.level >= RING_BYTES) {
			pthread_cond_wait(&ring.drained, &ring.lock);
		}
		bool stop = ring.stop;
		int tail = (ring.head + ring.level) % RING_BYTES;
		int room = RING_BYTES - ring.level;
		http_stream_t *stream = ring.stream;
		hls_t *hls = ring.hls;
		pthread_mutex_unlock(&ring.lock);

		if (stop) {
			break;
		}

		int to_end = RING_BYTES - tail;
		int want = room < to_end ? room : to_end;
		int got = stream ? http_stream_read(stream, ring.data + tail, want)
						 : hls_read(hls, ring.data + tail, want);

		pthread_mutex_lock(&ring.lock);
		if (got > 0) {
			// The mark goes at the start of the audio just read: the ICY block
			// that preceded it announces this audio, not the bytes already
			// consumed. Placing it after the increment made the title late by
			// one read.
			ring_note_title(&ring);
			ring.level += got;
			ring.written += got;
			pthread_cond_signal(&ring.filled);
			pthread_mutex_unlock(&ring.lock);
			continue;
		}
		ring.ended = true;
		pthread_cond_broadcast(&ring.filled);
		pthread_mutex_unlock(&ring.lock);
		break;
	}
	return NULL;
}

static bool ring_start(http_stream_t *stream, hls_t *hls) {
	pthread_mutex_lock(&ring.lock);
	ring.head = 0;
	ring.level = 0;
	ring.written = 0;
	ring.taken = 0;
	ring.ended = false;
	ring.stop = false;
	ring.stream = stream;
	ring.hls = hls;
	ring.mark_count = 0;
	pthread_mutex_unlock(&ring.lock);

	if (pthread_create(&ring.thread, NULL, ring_main, NULL) != 0) {
		ring.stream = NULL;
		ring.hls = NULL;
		return false;
	}
	ring.thread_valid = true;
	return true;
}

static void ring_stop(void) {
	if (!ring.thread_valid) {
		return;
	}

	pthread_mutex_lock(&ring.lock);
	ring.stop = true;
	http_stream_t *stream = ring.stream;
	hls_t *hls = ring.hls;
	pthread_cond_broadcast(&ring.filled);
	pthread_cond_broadcast(&ring.drained);
	pthread_mutex_unlock(&ring.lock);

	// The filling thread is normally sitting in recv(); the wake is what makes
	// joining it quick when the station has gone quiet.
	if (stream) {
		http_stream_wake(stream);
	} else if (hls) {
		hls_abort(hls);
	}
	pthread_join(ring.thread, NULL);

	pthread_mutex_lock(&ring.lock);
	ring.thread_valid = false;
	ring.stream = NULL;
	ring.hls = NULL;
	pthread_mutex_unlock(&ring.lock);
}

static int ring_read(void *dst, int max) {
	pthread_mutex_lock(&ring.lock);
	while (ring.level == 0 && !ring.ended && !ring.stop) {
		pthread_cond_wait(&ring.filled, &ring.lock);
	}

	int n = ring.level < max ? ring.level : max;
	if (n > 0) {
		int to_end = RING_BYTES - ring.head;
		int first = n < to_end ? n : to_end;
		memcpy(dst, ring.data + ring.head, (size_t)first);
		if (n > first) {
			memcpy((unsigned char *)dst + first, ring.data, (size_t)(n - first));
		}
		ring.head = (ring.head + n) % RING_BYTES;
		ring.level -= n;
		ring.taken += n;
		pthread_cond_signal(&ring.drained);
	}
	pthread_mutex_unlock(&ring.lock);
	return n;
}

// The same, without taking the bytes: the codec sniff needs to look before the
// decoder consumes.
static int ring_peek(void *dst, int max) {
	pthread_mutex_lock(&ring.lock);
	int n = ring.level < max ? ring.level : max;
	if (n > 0) {
		int to_end = RING_BYTES - ring.head;
		int first = n < to_end ? n : to_end;
		memcpy(dst, ring.data + ring.head, (size_t)first);
		if (n > first) {
			memcpy((unsigned char *)dst + first, ring.data, (size_t)(n - first));
		}
	}
	pthread_mutex_unlock(&ring.lock);
	return n;
}

static bool ring_take_title(char *out, size_t size) {
	bool got = false;
	pthread_mutex_lock(&ring.lock);
	if (ring.mark_count > 0 && ring.taken >= ring.marks[0].at) {
		snprintf(out, size, "%s", ring.marks[0].title);
		ring.mark_count--;
		memmove(ring.marks, ring.marks + 1, (size_t)ring.mark_count * sizeof(ring.marks[0]));
		got = true;
	}
	pthread_mutex_unlock(&ring.lock);
	return got;
}

static int ring_level(void) {
	pthread_mutex_lock(&ring.lock);
	int level = ring.level;
	pthread_mutex_unlock(&ring.lock);
	return level;
}

static void ring_prefill(unsigned mine) {
	int waited = 0;
	for (;;) {
		pthread_mutex_lock(&ring.lock);
		int level = ring.level;
		bool ended = ring.ended;
		pthread_mutex_unlock(&ring.lock);

		if (ended || level >= PREFILL_BYTES || waited >= PREFILL_MS || abort_requested(mine)) {
			break;
		}
		usleep(50 * 1000);
		waited += 50;
	}
}

// Waits until the sniff has enough to look at, or the source ends.
static bool ring_wait_level(int wanted, int wait_ms, unsigned mine) {
	int waited = 0;
	for (;;) {
		pthread_mutex_lock(&ring.lock);
		int level = ring.level;
		bool ended = ring.ended;
		pthread_mutex_unlock(&ring.lock);
		if (level >= wanted || ended || waited >= wait_ms || abort_requested(mine)) {
			return level > 0;
		}
		usleep(50 * 1000);
		waited += 50;
	}
}

static size_t ring_read_cb(void *user, void *dst, size_t bytes) {
	(void)user;
	if (bytes > (size_t)INPUT_BYTES) {
		bytes = INPUT_BYTES;
	}
	int got = ring_read(dst, (int)bytes);
	return got > 0 ? (size_t)got : 0;
}

// ---------------------------------------------------------------------------
// What the bytes are
// ---------------------------------------------------------------------------

typedef enum {
	CODEC_UNKNOWN = 0,
	CODEC_MP3,
	CODEC_AAC,
} codec_t;

static codec_t codec_from_content_type(const char *ct) {
	if (!ct || !ct[0]) {
		return CODEC_UNKNOWN;
	}
	if (strcasestr(ct, "aac") || strcasestr(ct, "mp4a")) {
		return CODEC_AAC;
	}
	if (strcasestr(ct, "mpeg") || strcasestr(ct, "mp3")) {
		return CODEC_MP3;
	}
	return CODEC_UNKNOWN;
}

static int skip_id3(const unsigned char *data, int len) {
	int off = 0;
	while (off + 10 <= len && data[off] == 'I' && data[off + 1] == 'D' && data[off + 2] == '3') {
		int size = ((data[off + 6] & 0x7F) << 21) | ((data[off + 7] & 0x7F) << 14) |
				   ((data[off + 8] & 0x7F) << 7) | (data[off + 9] & 0x7F);
		int step = 10 + size;
		if (step <= 0 || off + step > len) {
			break;
		}
		off += step;
	}
	return off;
}

static int adts_frame_len(const unsigned char *data, int len, int off) {
	if (off < 0 || off + 7 > len) {
		return 0;
	}
	// Twelve sync bits, then the layer bits at zero -- what tells ADTS from
	// MPEG audio, where the layer is never zero.
	if (data[off] != 0xFF || (data[off + 1] & 0xF6) != 0xF0) {
		return 0;
	}
	if (((data[off + 2] >> 2) & 0x0F) > 12) {
		return 0;
	}
	int frame_len = ((data[off + 3] & 0x03) << 11) | (data[off + 4] << 3) | ((data[off + 5] >> 5) & 0x07);
	return frame_len >= 7 ? frame_len : 0;
}

static int adts_chain(const unsigned char *data, int len, int off, int want) {
	int seen = 0;
	while (seen < want) {
		int frame_len = adts_frame_len(data, len, off);
		if (frame_len == 0) {
			return seen;
		}
		seen++;
		off += frame_len;
	}
	return seen;
}

// A positive answer needs several ADTS frames whose declared lengths land on
// each other. One 0xFF in payload data proves nothing; minimp3 resynchronises
// after a wrong guess, fdk-aac does not.
static bool bytes_look_like_adts(const unsigned char *data, int len) {
	for (int off = skip_id3(data, len); off + 7 <= len; off++) {
		if (data[off] != 0xFF) {
			continue;
		}
		if (adts_chain(data, len, off, ADTS_CHAIN_PROOF) >= ADTS_CHAIN_PROOF) {
			return true;
		}
	}
	return false;
}

// ---------------------------------------------------------------------------
// Playlists
// ---------------------------------------------------------------------------

static bool url_ends_with(const char *url, const char *ext) {
	// The extension is what is left of the query string: plenty of stations
	// answer with .m3u8?token=... and the token is not part of the name.
	const char *query = strchr(url, '?');
	size_t len = query ? (size_t)(query - url) : strlen(url);
	size_t n = strlen(ext);
	return len >= n && strncasecmp(url + len - n, ext, n) == 0;
}

static bool url_is_hls(const char *url) {
	return url_ends_with(url, ".m3u8");
}

static bool url_is_playlist(const char *url) {
	return url_ends_with(url, ".m3u") || url_ends_with(url, ".pls") ||
		   url_ends_with(url, ".xspf") || url_ends_with(url, ".asx");
}

static bool content_type_is_playlist(const char *ct) {
	if (!ct || !ct[0]) {
		return false;
	}
	return strcasestr(ct, "mpegurl") || strcasestr(ct, "scpls") || strcasestr(ct, "pls") ||
		   strcasestr(ct, "xspf") || strcasestr(ct, "ms-asf");
}

// The first address in .pls (File1=http://...) or .m3u (one per line), or the
// first thing that looks like an address in anything else.
static bool playlist_first_url(const char *text, char *out, size_t out_size) {
	const char *line = text;
	while (line && *line) {
		const char *end = strpbrk(line, "\r\n");
		size_t len = end ? (size_t)(end - line) : strlen(line);
		const char *begin = line;
		while (len > 0 && (*begin == ' ' || *begin == '\t')) {
			begin++;
			len--;
		}
		while (len > 0 && (begin[len - 1] == ' ' || begin[len - 1] == '\t')) {
			len--;
		}

		if (len > 0 && *begin != '#' && *begin != ';') {
			const char *http = NULL;
			for (size_t i = 0; i + 7 <= len; i++) {
				if (strncasecmp(begin + i, "http://", 7) == 0 ||
					(i + 8 <= len && strncasecmp(begin + i, "https://", 8) == 0)) {
					http = begin + i;
					len -= i;
					break;
				}
			}
			if (http) {
				size_t take = 0;
				while (take < len && http[take] != '"' && http[take] != '\'' && http[take] != '<' &&
					   http[take] != '>' && http[take] != ' ') {
					take++;
				}
				if (take > 0 && take < out_size) {
					memcpy(out, http, take);
					out[take] = '\0';
					return true;
				}
			}
		}
		line = end ? end + strspn(end, "\r\n") : NULL;
	}
	return false;
}

static bool resolve_playlist(char *url, size_t url_size, bool *is_hls, char *why, size_t why_size) {
	for (int hop = 0; hop < PLAYLIST_MAX_HOPS; hop++) {
		if (url_is_hls(url)) {
			*is_hls = true;
			return true;
		}
		if (!url_is_playlist(url)) {
			return true;
		}

		char *text = NULL;
		if (!http_get(url, &text, NULL, PLAYLIST_MAX_BYTES, 10)) {
			return true; // try it as a stream; some servers hand out a stream here
		}
		char next[RADIO_URL_MAX];
		bool found = playlist_first_url(text, next, sizeof(next));
		free(text);
		if (!found) {
			snprintf(why, why_size, "The station's playlist is empty.");
			return false;
		}
		snprintf(url, url_size, "%s", next);
	}
	snprintf(why, why_size, "The station's playlist points in a circle.");
	return false;
}

// ---------------------------------------------------------------------------
// Opening the connection
// ---------------------------------------------------------------------------

static bool observe_socket(void *user, int fd) {
	unsigned mine = (unsigned)(uintptr_t)user;
	pthread_mutex_lock(&ctl);
	setup_fd = fd;
	bool keep = fd < 0 || !abort_locked(mine);
	pthread_mutex_unlock(&ctl);
	return keep;
}

static void wake_stream(void) {
	// The live pointers are cleared under this lock before the source is
	// closed, so holding it across the wake makes the operation safe.
	pthread_mutex_lock(&stream_lock);
	if (live_stream) {
		http_stream_wake(live_stream);
	}
	if (live_hls) {
		hls_abort(live_hls);
	}
	pthread_mutex_unlock(&stream_lock);

	pthread_mutex_lock(&ctl);
	int fd = setup_fd;
	pthread_mutex_unlock(&ctl);
	if (fd >= 0) {
		shutdown(fd, SHUT_RDWR);
	}
}

static bool open_http(http_stream_t *stream, const char *url, unsigned mine, char *why, size_t why_size) {
	memset(stream, 0, sizeof(*stream));
	stream->fd = -1;

	http_stream_options_t options = {
		.socket_cb = observe_socket,
		.socket_user = (void *)(uintptr_t)mine,
	};
	bool opened = http_stream_open_ex(stream, url, READ_TIMEOUT_SECS, &options);

	pthread_mutex_lock(&ctl);
	setup_fd = -1;
	pthread_mutex_unlock(&ctl);

	if (!opened) {
		const char *reason = http_last_error();
		snprintf(why, why_size, "%s", reason ? reason : "Could not open the stream.");
		return false;
	}
	return true;
}

// ---------------------------------------------------------------------------
// The output
// ---------------------------------------------------------------------------

static bool ensure_output(int rate, int channels, const char *codec) {
	if (rate <= 0 || channels <= 0) {
		return false;
	}
	if (output_open && output_rate == rate) {
		return true;
	}
	if (output_open) {
		morpthis_audio_close();
		output_open = false;
	}

	// Always two channels: a card opened for one interprets stereo bytes as
	// half-pairs and plays them at double speed. Mono is widened below.
	if (!morpthis_audio_open_ex(rate, 2, 16, 0)) {
		return false;
	}
	output_open = true;
	output_rate = rate;
	morpthis_audio_set_playing(true);

	pthread_mutex_lock(&ctl);
	now.rate = rate;
	now.channels = channels;
	snprintf(now.codec, sizeof(now.codec), "%s", codec ? codec : "");
	bump_locked();
	pthread_mutex_unlock(&ctl);
	return true;
}

static bool push_pcm(const int16_t *samples, int frames, int channels, int rate, const char *codec) {
	if (frames <= 0) {
		return true;
	}
	if (!ensure_output(rate, channels, codec)) {
		pthread_mutex_lock(&ctl);
		snprintf(now.error, sizeof(now.error), "Could not open the audio output.");
		pthread_mutex_unlock(&ctl);
		return false;
	}

	const int16_t *out = samples;
	if (channels == 1) {
		for (int i = frames - 1; i >= 0; i--) {
			stereo[2 * i] = samples[i];
			stereo[2 * i + 1] = samples[i];
		}
		out = stereo;
	} else if (channels > 2) {
		int pairs = channels / 2;
		for (int i = 0; i < frames; i++) {
			int left = 0;
			int right = 0;
			for (int c = 0; c < pairs; c++) {
				left += samples[i * channels + 2 * c];
				right += samples[i * channels + 2 * c + 1];
			}
			stereo[2 * i] = (int16_t)(left / pairs);
			stereo[2 * i + 1] = (int16_t)(right / pairs);
		}
		out = stereo;
	}

	size_t wrote = morpthis_audio_write_pcm(out, (size_t)frames);
	if (wrote < (size_t)frames) {
		pthread_mutex_lock(&ctl);
		snprintf(now.error, sizeof(now.error), "The audio output stopped.");
		pthread_mutex_unlock(&ctl);
		return false;
	}
	return true;
}

// ---------------------------------------------------------------------------
// One connection
// ---------------------------------------------------------------------------

typedef enum {
	ONCE_ENDED = 0,
	ONCE_ERROR,	  // worth a reconnect
	ONCE_FATAL,	  // the format or this device, not the network
	ONCE_ABORTED, // stopped, replaced or paused
	ONCE_REDIRECT, // the server sent a playlist instead of audio
} once_result_t;

static once_result_t play_once(char *url, size_t url_size, bool *is_hls, unsigned mine) {
	http_stream_t stream;
	hls_t *hls = NULL;
	aacdec_t *aac = NULL;
	drmp3 *mp3 = NULL;
	bool using_hls = *is_hls;
	char why[192] = "";
	once_result_t result = ONCE_ERROR;

	if (using_hls) {
		hls = hls_open(url, why, sizeof(why));
		if (!hls) {
			pthread_mutex_lock(&ctl);
			snprintf(now.error, sizeof(now.error), "%s",
					 why[0] ? why : "The station's manifest could not be opened.");
			pthread_mutex_unlock(&ctl);
			return ONCE_ERROR;
		}
		pthread_mutex_lock(&stream_lock);
		live_hls = hls;
		pthread_mutex_unlock(&stream_lock);
	} else {
		if (!open_http(&stream, url, mine, why, sizeof(why))) {
			pthread_mutex_lock(&ctl);
			snprintf(now.error, sizeof(now.error), "%s", why);
			pthread_mutex_unlock(&ctl);
			return abort_requested(mine) ? ONCE_ABORTED : ONCE_ERROR;
		}

		if (content_type_is_playlist(stream.content_type)) {
			// The address looked like a stream, but the server answered with a
			// list. Read the little of it there is and start again at what it
			// points at.
			char text[PLAYLIST_MAX_BYTES];
			int got = 0;
			while (got < (int)sizeof(text) - 1) {
				int n = http_stream_read(&stream, text + got, (int)sizeof(text) - 1 - got);
				if (n <= 0) {
					break;
				}
				got += n;
			}
			text[got > 0 ? got : 0] = '\0';
			http_stream_close(&stream);

			if (hls_text_looks_like(text, (size_t)got)) {
				*is_hls = true;
				return ONCE_REDIRECT;
			}
			char next[RADIO_URL_MAX];
			if (!playlist_first_url(text, next, sizeof(next))) {
				pthread_mutex_lock(&ctl);
				snprintf(now.error, sizeof(now.error), "The station sent a playlist with no address in it.");
				pthread_mutex_unlock(&ctl);
				return ONCE_FATAL;
			}
			snprintf(url, url_size, "%s", next);
			return ONCE_REDIRECT;
		}

		pthread_mutex_lock(&stream_lock);
		live_stream = &stream;
		pthread_mutex_unlock(&stream_lock);
	}

	if (!ring_start(using_hls ? NULL : &stream, using_hls ? hls : NULL)) {
		pthread_mutex_lock(&ctl);
		snprintf(now.error, sizeof(now.error), "Not enough memory for the stream buffer.");
		pthread_mutex_unlock(&ctl);
		result = ONCE_FATAL;
		goto done;
	}

	if (!using_hls) {
		morpthis_logd("radio", "stream %s, ICY interval %d, ring %d",
					  stream.content_type[0] ? stream.content_type : "?", stream.icy_metaint,
					  ring_level());
	}

	ring_prefill(mine);
	morpthis_logd("radio", "prefill %d bytes", ring_level());

	codec_t codec = CODEC_UNKNOWN;
	if (using_hls) {
		switch (hls_codec(hls)) {
		case HLS_CODEC_MP3:
			codec = CODEC_MP3;
			break;
		case HLS_CODEC_AAC:
			codec = CODEC_AAC;
			break;
		default:
			break;
		}
		if (codec == CODEC_UNKNOWN && ring_wait_level(4096, 2000, mine)) {
			unsigned char probe[4096];
			int n = ring_peek(probe, sizeof(probe));
			codec = bytes_look_like_adts(probe, n) ? CODEC_AAC : CODEC_MP3;
		}
		if (codec == CODEC_UNKNOWN) {
			pthread_mutex_lock(&ctl);
			snprintf(now.error, sizeof(now.error), "The stream did not provide audio this app can decode.");
			pthread_mutex_unlock(&ctl);
			result = ONCE_ERROR;
			goto done;
		}
	} else {
		codec = codec_from_content_type(stream.content_type);
		if (codec == CODEC_UNKNOWN && ring_wait_level(4096, 2000, mine)) {
			unsigned char probe[4096];
			int n = ring_peek(probe, sizeof(probe));
			codec = bytes_look_like_adts(probe, n) ? CODEC_AAC : CODEC_MP3;
		}
		if (codec == CODEC_UNKNOWN) {
			codec = CODEC_MP3; // dr_mp3 resynchronises; let it try
		}
	}

	if (abort_requested(mine)) {
		result = ONCE_ABORTED;
		goto done;
	}

	int rate = 0;
	int channels = 0;

	if (codec == CODEC_AAC) {
		aac = aacdec_open_adts();
		if (!aac) {
			pthread_mutex_lock(&ctl);
			snprintf(now.error, sizeof(now.error),
					 "This station uses AAC, but the device's AAC decoder is not available.");
			pthread_mutex_unlock(&ctl);
			result = ONCE_FATAL;
			goto done;
		}
	} else {
		mp3 = calloc(1, sizeof(*mp3));
		if (!mp3) {
			pthread_mutex_lock(&ctl);
			snprintf(now.error, sizeof(now.error), "Not enough memory for the MP3 decoder.");
			pthread_mutex_unlock(&ctl);
			result = ONCE_FATAL;
			goto done;
		}
		if (!drmp3_init(mp3, ring_read_cb, NULL, NULL, NULL, &ring, NULL)) {
			drmp3_uninit(mp3);
			free(mp3);
			mp3 = NULL;
			morpthis_logw("radio", "the MP3 decoder rejected the stream (ring %d bytes)",
						  ring_level());
			pthread_mutex_lock(&ctl);
			snprintf(now.error, sizeof(now.error),
					 "The station did not send MP3 audio this app can decode.");
			pthread_mutex_unlock(&ctl);
			result = ONCE_ERROR;
			goto done;
		}
		rate = (int)mp3->sampleRate;
		channels = (int)mp3->channels;
	}

	bool started = false;
	bool failed = false;
	int have = 0;
	uint32_t opened_at = 0;
	bool audio_seen = false;

	// Pre-roll is paid; the clock for "this station never sent audio" starts
	// after it, so a slow but working station is not cut off during buffering.
	opened_at = morpthis_app_time_ms();

	for (;;) {
		if (abort_requested(mine)) {
			result = ONCE_ABORTED;
			break;
		}

		int frames = 0;
		if (mp3) {
			drmp3_uint64 got = drmp3_read_pcm_frames_s16(mp3, PCM_FRAMES, pcm);
			frames = (int)got;
			if (frames <= 0) {
				result = ONCE_ENDED;
				break;
			}
			rate = (int)mp3->sampleRate;
			channels = (int)mp3->channels;
		} else {
			if (have < INPUT_BYTES) {
				int got = ring_read(input + have, INPUT_BYTES - have);
				if (got <= 0) {
					result = ONCE_ENDED;
					break;
				}
				have += got;
			}

			int taken = aacdec_fill(aac, input, have);
			if (taken < 0) {
				result = ONCE_FATAL;
				pthread_mutex_lock(&ctl);
				snprintf(now.error, sizeof(now.error), "The AAC stream was refused by the decoder.");
				pthread_mutex_unlock(&ctl);
				break;
			}
			if (taken > 0) {
				memmove(input, input + taken, (size_t)(have - taken));
				have -= taken;
			}
			frames = aacdec_pull(aac, pcm, PCM_SAMPLES);
			rate = aacdec_sample_rate(aac);
			channels = aacdec_channels(aac);
			if (taken == 0 && frames <= 0) {
				// Neither consumed nor produced: bytes the decoder cannot use.
				// Dropping them beats stalling forever.
				have = 0;
			}
		}

		if (frames > 0 && rate > 0 && channels > 0) {
			if (!started) {
				started = true;
				pthread_mutex_lock(&ctl);
				set_state_locked(RADIO_PLAYER_PLAYING, NULL);
				pthread_mutex_unlock(&ctl);
				publish_track(true);
			}
			audio_seen = true;
			const char *name = codec == CODEC_AAC ? "AAC" : "MP3";
			if (!push_pcm(pcm, frames, channels, rate, name)) {
				result = ONCE_FATAL;
				failed = true;
				break;
			}

			char title[sizeof(now.title)];
			if (ring_take_title(title, sizeof(title)) && title[0]) {
				pthread_mutex_lock(&ctl);
				snprintf(now.title, sizeof(now.title), "%s", title);
				bump_locked();
				pthread_mutex_unlock(&ctl);
				publish_track(true);
			}
		}

		if (!audio_seen && (uint32_t)(morpthis_app_time_ms() - opened_at) > NO_AUDIO_TIMEOUT_MS) {
			pthread_mutex_lock(&ctl);
			snprintf(now.error, sizeof(now.error), "The station did not send playable audio.");
			pthread_mutex_unlock(&ctl);
			result = ONCE_ERROR;
			break;
		}
	}

	if (failed) {
		result = ONCE_FATAL;
	}

done:
	// Stop the filling thread before closing the source it is reading. The
	// ring's own wake is what brings it out of a blocking read.
	ring_stop();

	pthread_mutex_lock(&stream_lock);
	live_stream = NULL;
	live_hls = NULL;
	pthread_mutex_unlock(&stream_lock);

	if (!using_hls) {
		http_stream_close(&stream);
	}

	if (mp3) {
		drmp3_uninit(mp3);
		free(mp3);
	}
	aacdec_close(aac);
	if (hls) {
		hls_close(hls);
	}

	if (output_open) {
		morpthis_audio_set_playing(false);
		morpthis_audio_close();
		output_open = false;
		output_rate = 0;
	}

	return result;
}

// ---------------------------------------------------------------------------
// The station
// ---------------------------------------------------------------------------

static void play_station(const radio_station_t *station, unsigned mine) {
	char url[RADIO_URL_MAX];
	snprintf(url, sizeof(url), "%s", station->url);
	bool is_hls = url_is_hls(url);
	char why[192] = "";

	// Held from the first attempt, not from the first decoded frame: a station
	// whose connect or TLS handshake takes ten seconds would otherwise let the
	// device go to standby in the middle of opening it. Retries keep it.
	set_holds(true);

	if (!resolve_playlist(url, sizeof(url), &is_hls, why, sizeof(why))) {
		pthread_mutex_lock(&ctl);
		if (mine == generation) {
			set_state_locked(RADIO_PLAYER_ERROR, why);
		}
		pthread_mutex_unlock(&ctl);
		goto out;
	}

	int attempts = 0;
	int redirects = 0;
	const char *last_error = "The station stopped.";

	for (;;) {
		if (abort_requested(mine)) {
			goto out;
		}
		once_result_t result = play_once(url, sizeof(url), &is_hls, mine);
		if (result == ONCE_ABORTED) {
			goto out;
		}
		if (result == ONCE_REDIRECT) {
			if (++redirects > PLAYLIST_MAX_HOPS) {
				last_error = "The station redirects in a circle.";
				break;
			}
			continue;
		}
		if (result == ONCE_FATAL) {
			break;
		}
		if (result == ONCE_ENDED || result == ONCE_ERROR) {
			if (!abort_requested(mine) && attempts < PLAY_MAX_RETRIES) {
				attempts++;
				pthread_mutex_lock(&ctl);
				if (mine == generation) {
					set_state_locked(RADIO_PLAYER_CONNECTING, NULL);
				}
				pthread_mutex_unlock(&ctl);
				for (int waited = 0; waited < PLAY_RETRY_MS; waited += 100) {
					if (abort_requested(mine)) {
						goto out;
					}
					usleep(100 * 1000);
				}
				continue;
			}
			last_error = "The station stopped.";
			break;
		}
		break;
	}

	pthread_mutex_lock(&ctl);
	if (mine == generation && !quit) {
		const char *error = now.error[0] ? now.error : last_error;
		set_state_locked(RADIO_PLAYER_ERROR, error);
	}
	pthread_mutex_unlock(&ctl);

out:
	set_holds(false);
}

static void *player_main(void *unused) {
	(void)unused;
	for (;;) {
		pthread_mutex_lock(&ctl);
		while (!quit && command == CMD_NONE) {
			pthread_cond_wait(&ctl_wake, &ctl);
		}
		if (quit) {
			pthread_mutex_unlock(&ctl);
			break;
		}

		if (command == CMD_PLAY) {
			radio_station_t station = pending;
			command = CMD_NONE;
			unsigned mine = generation;
			pthread_mutex_unlock(&ctl);
			play_station(&station, mine);
			continue;
		}

		// Stop.
		command = CMD_NONE;
		set_state_locked(RADIO_PLAYER_IDLE, NULL);
		pthread_mutex_unlock(&ctl);

		morpthis_audio_set_playing(false);
		morpthis_audio_set_track(NULL, false);
		morpthis_audio_close();
		output_open = false;
		output_rate = 0;
		set_holds(false);
	}

	morpthis_audio_set_playing(false);
	morpthis_audio_set_track(NULL, false);
	morpthis_audio_close();
	set_holds(false);
	return NULL;
}

// ---------------------------------------------------------------------------
// The interface
// ---------------------------------------------------------------------------

bool radio_player_init(void) {
	if (player_started) {
		return true;
	}
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 256 * 1024);
	int rc = pthread_create(&player_thread, &attr, player_main, NULL);
	pthread_attr_destroy(&attr);
	player_started = rc == 0;
	if (player_started) {
		pthread_mutex_lock(&ctl);
		set_state_locked(RADIO_PLAYER_IDLE, NULL);
		pthread_mutex_unlock(&ctl);
	}
	return player_started;
}

bool radio_player_play(const radio_station_t *station) {
	if (!player_started || !station || !station->url[0]) {
		return false;
	}

	pthread_mutex_lock(&ctl);
	pending = *station;
	if (pending.bitrate < 0) {
		pending.bitrate = 0;
	}
	now.station = *station;
	snprintf(now.title, sizeof(now.title), "%s", station->name);
	now.codec[0] = '\0';
	now.error[0] = '\0';
	now.rate = 0;
	now.channels = 0;
	now.bitrate = station->bitrate;
	now.state = RADIO_PLAYER_CONNECTING;
	bump_locked();

	pause_requested = false;
	command = CMD_PLAY;
	generation++;
	pthread_cond_signal(&ctl_wake);
	pthread_mutex_unlock(&ctl);

	wake_stream();
	return true;
}

void radio_player_toggle_pause(void) {
	pthread_mutex_lock(&ctl);

	if (now.state == RADIO_PLAYER_PLAYING || now.state == RADIO_PLAYER_CONNECTING) {
		// Pause means the socket and the output are let go: a live stream has
		// no buffer to resume from, and holding the connection open would make
		// the resume minutes behind the broadcast.
		pause_requested = true;
		generation++;
		command = CMD_NONE;
		set_state_locked(RADIO_PLAYER_PAUSED, NULL);
		pthread_mutex_unlock(&ctl);
		wake_stream();
		morpthis_audio_set_playing(false);
		publish_track(false);
		set_holds(false);
		return;
	}

	if ((now.state == RADIO_PLAYER_PAUSED || now.state == RADIO_PLAYER_ERROR ||
		 now.state == RADIO_PLAYER_IDLE) &&
		now.station.url[0]) {
		pending = now.station;
		// The previous ICY title belongs to the connection that was just
		// closed; a fresh one starts from the station's own name.
		snprintf(now.title, sizeof(now.title), "%s", now.station.name);
		now.codec[0] = '\0';
		now.rate = 0;
		now.channels = 0;
		pause_requested = false;
		command = CMD_PLAY;
		generation++;
		set_state_locked(RADIO_PLAYER_CONNECTING, NULL);
		pthread_cond_signal(&ctl_wake);
	}
	pthread_mutex_unlock(&ctl);
}

void radio_player_stop(void) {
	pthread_mutex_lock(&ctl);
	generation++;
	command = CMD_STOP;
	pause_requested = false;
	set_state_locked(RADIO_PLAYER_IDLE, NULL);
	pthread_cond_signal(&ctl_wake);
	pthread_mutex_unlock(&ctl);

	wake_stream();
	morpthis_audio_set_playing(false);
	morpthis_audio_set_track(NULL, false);
	set_holds(false);
}

void radio_player_get(radio_now_t *out) {
	if (!out) {
		return;
	}
	pthread_mutex_lock(&ctl);
	*out = now;
	pthread_mutex_unlock(&ctl);
}

bool radio_player_active(void) {
	pthread_mutex_lock(&ctl);
	bool active = now.state == RADIO_PLAYER_CONNECTING || now.state == RADIO_PLAYER_PLAYING ||
				  now.state == RADIO_PLAYER_PAUSED;
	pthread_mutex_unlock(&ctl);
	return active;
}

void radio_player_shutdown_begin(void) {
	if (player_started) {
		pthread_mutex_lock(&ctl);
		quit = true;
		generation++;
		command = CMD_STOP;
		pause_requested = false;
		pthread_cond_signal(&ctl_wake);
		pthread_mutex_unlock(&ctl);

		wake_stream();
	}
	// A playlist lookup runs a whole http_get() that wake_stream() cannot see.
	// Aborting every socket is what keeps shutdown inside the player's bound.
	// It also covers the directory worker: both modules share this HTTP client,
	// so the app can put them both on the way out before joining either.
	http_abort_all();
}

void radio_player_shutdown(void) {
	if (!player_started) {
		http_abort_all();
		return;
	}

	radio_player_shutdown_begin();
	pthread_join(player_thread, NULL);
	player_started = false;

	morpthis_audio_set_playing(false);
	morpthis_audio_set_track(NULL, false);
	morpthis_audio_close();
	output_open = false;
	output_rate = 0;
	morpthis_power_release_all();
}
