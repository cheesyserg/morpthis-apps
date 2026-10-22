// Host-side integration test for the Subsonic app's client and playback path.
//
// It links the real subsonic_client.c, stream_player.c and vendored HTTP/JSON/
// MD5/dr_mp3/dr_flac objects. The SDK calls the player makes are stubbed here:
// the DAC counts frames instead of playing them, and a posted callback runs on
// the worker thread instead of the GUI thread. tests/integration.py supplies a
// loopback server with an MP3 and a real FLAC stream.

#include <morpthis/morpthis.h>

#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "stream_player.h"
#include "subsonic.h"
#include "tls.h"

// ---------------------------------------------------------------------------
// SDK stubs
// ---------------------------------------------------------------------------

static pthread_mutex_t audio_lock = PTHREAD_MUTEX_INITIALIZER;
static size_t audio_frames;
static int audio_rate;
static int audio_channels;
static int audio_bits;
static int audio_buffer_ms;

bool morpthis_audio_open_ex(int rate, int channels, int bits, int buffer_ms) {
	pthread_mutex_lock(&audio_lock);
	audio_rate = rate;
	audio_channels = channels;
	audio_bits = bits;
	audio_buffer_ms = buffer_ms;
	audio_frames = 0;
	pthread_mutex_unlock(&audio_lock);
	return true;
}

size_t morpthis_audio_write_pcm(const void *pcm, size_t frames) {
	(void)pcm;
	pthread_mutex_lock(&audio_lock);
	audio_frames += frames;
	pthread_mutex_unlock(&audio_lock);
	// Roughly 13x realtime: enough that the test can stop a stream part-way
	// without making the normal case slow.
	struct timespec pause = {.tv_sec = 0, .tv_nsec = 2000000L};
	nanosleep(&pause, NULL);
	return frames;
}

void morpthis_audio_set_playing(bool playing) { (void)playing; }

void morpthis_audio_close(void) {}

void morpthis_app_post(morpthis_post_fn fn, void *user) {
	// The app's posted callback only takes a lock and flips a flag, so running
	// it on the worker is safe for this test.
	fn(user);
}

void morpthis_log(morpthis_log_level_t level, const char *tag, const char *format, ...) {
	static const char *const names[] = {"D", "I", "W", "E"};
	va_list args;
	va_start(args, format);
	fprintf(stderr, "[%s] %s: ", names[level], tag ? tag : "test");
	vfprintf(stderr, format, args);
	fputc('\n', stderr);
	va_end(args);
}

// ---------------------------------------------------------------------------
// Test
// ---------------------------------------------------------------------------

static pthread_mutex_t event_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t event_cond = PTHREAD_COND_INITIALIZER;
static bool finished;
static bool failed;
static int started_count;
static char failure[192];

static void on_event(stream_event_kind_t kind, const char *message,
					 const stream_track_t *track, void *user) {
	(void)track;
	(void)user;
	if (kind == STREAM_EVENT_LISTENED || kind == STREAM_EVENT_PROGRESS) {
		return;
	}
	if (kind == STREAM_EVENT_STARTED) {
		pthread_mutex_lock(&event_lock);
		started_count++;
		pthread_cond_broadcast(&event_cond);
		pthread_mutex_unlock(&event_lock);
		return;
	}
	pthread_mutex_lock(&event_lock);
	if (kind == STREAM_EVENT_FINISHED) {
		finished = true;
	} else {
		failed = true;
		snprintf(failure, sizeof(failure), "%s", message ? message : "unknown");
	}
	pthread_cond_broadcast(&event_cond);
	pthread_mutex_unlock(&event_lock);
}

static int failures;
static void check(bool ok, const char *what) {
	if (!ok) {
		fprintf(stderr, "FAIL: %s\n", what);
		failures++;
	}
}

int main(int argc, char **argv) {
	(void)argc;
	(void)argv;
	// The app's entry point does this; the test links the player without it.
	signal(SIGPIPE, SIG_IGN);
	const char *port = getenv("SUBSONIC_TEST_PORT");
	if (!port || !port[0]) {
		fprintf(stderr, "FAIL: SUBSONIC_TEST_PORT is not set; run tests/integration.py\n");
		return 2;
	}
	const char *scheme = getenv("SUBSONIC_TEST_SCHEME");
	if (!scheme || !scheme[0]) {
		scheme = "http";
	}
	const char *tls_mode = getenv("SUBSONIC_TEST_TLS");
	if (tls_mode && strcmp(tls_mode, "relaxed") == 0) {
		tls_set_verify_mode(TLS_VERIFY_RELAXED);
		check(tls_verify_mode() == TLS_VERIFY_RELAXED, "relaxed TLS was applied");
	} else {
		check(tls_verify_mode() == TLS_VERIFY_STRICT, "strict TLS is the default");
	}
	if (strcmp(scheme, "https") == 0 && !tls_available()) {
		fprintf(stderr, "FAIL: https was asked for but OpenSSL could not be loaded\n");
		return 2;
	}

	char error[192] = "";
	subsonic_credentials_t credentials;
	memset(&credentials, 0, sizeof(credentials));
	snprintf(credentials.server, sizeof(credentials.server), "%s://127.0.0.1:%s", scheme, port);
	snprintf(credentials.username, sizeof(credentials.username), "user");
	snprintf(credentials.password, sizeof(credentials.password), "pass");
	subsonic_quality_t quality = {SUBSONIC_FORMAT_MP3, 128};

	fprintf(stderr, "test_stream: %s, TLS %s\n", scheme,
			tls_mode && tls_mode[0] ? tls_mode : "strict");

	subsonic_client_init();
	check(subsonic_configure(&credentials, quality, error, sizeof(error)), error);

	// A pasted /rest suffix is accepted and removed.
	subsonic_credentials_t pasted = credentials;
	snprintf(pasted.server, sizeof(pasted.server), "%s://127.0.0.1:%s/rest", scheme, port);
	check(subsonic_configure(&pasted, quality, error, sizeof(error)), "server URL with /rest");
	char url[SUBSONIC_URL_MAX];
	check(subsonic_stream_url("tr1", quality, url, sizeof(url), error, sizeof(error)), error);
	check(strstr(url, "/rest/stream.view?") != NULL, "stream endpoint");
	check(strstr(url, "&format=mp3") != NULL, "MP3 format is requested");
	check(strstr(url, "&maxBitRate=128") != NULL, "bitrate is requested");
	check(subsonic_stream_url_at("tr1", quality, 125, url, sizeof(url), error, sizeof(error)), error);
	check(strstr(url, "&timeOffset=125") != NULL, "a seek offset is requested");

	subsonic_entry_t entries[SUBSONIC_PAGE_SIZE];
	bool more = true;
	int count = subsonic_browse(SUBSONIC_BROWSE_ALBUM, "al1", 0, entries, SUBSONIC_PAGE_SIZE,
								&more, error, sizeof(error));
	check(count == 2, "the album contains two tracks");
	check(!more, "the page is not truncated");
	if (count == 2) {
		check(entries[0].kind == SUBSONIC_TRACK, "the first entry is a track");
		check(strcmp(entries[0].title, "First Track") == 0, "the first title");
		check(strcmp(entries[0].artist, "Tester") == 0, "the first artist");
		check(entries[0].starred, "the starred flag is read");
		check(strcmp(entries[1].id, "tr2") == 0, "the second id");
	}

	count = subsonic_browse(SUBSONIC_BROWSE_PLAYLISTS, NULL, 0, entries, SUBSONIC_PAGE_SIZE,
							&more, error, sizeof(error));
	check(count == 1 && entries[0].kind == SUBSONIC_PLAYLIST, "playlists are parsed");
	check(count == 1 && strcmp(entries[0].title, "Road Trip") == 0, "the playlist name");

	count = subsonic_browse(SUBSONIC_BROWSE_STARRED, NULL, 0, entries, SUBSONIC_PAGE_SIZE,
							&more, error, sizeof(error));
	check(count == 2, "starred songs and albums are both returned");
	check(count == 2 && entries[0].kind == SUBSONIC_TRACK && entries[1].kind == SUBSONIC_ALBUM,
		  "starred keeps songs before albums");

	count = subsonic_browse(SUBSONIC_BROWSE_GENRES, NULL, 0, entries, SUBSONIC_PAGE_SIZE,
							&more, error, sizeof(error));
	check(count == 1 && entries[0].kind == SUBSONIC_GENRE, "genres are parsed");
	check(count == 1 && strcmp(entries[0].title, "Rock") == 0, "the genre value becomes the title");

	count = subsonic_browse(SUBSONIC_BROWSE_SEARCH, "first", 0, entries, SUBSONIC_PAGE_SIZE,
							&more, error, sizeof(error));
	check(count == 1 && entries[0].kind == SUBSONIC_TRACK, "search songs are parsed");

	subsonic_credentials_t invalid = credentials;
	snprintf(invalid.server, sizeof(invalid.server), "ftp://127.0.0.1");
	check(!subsonic_configure(&invalid, quality, error, sizeof(error)), "ftp is refused");
	// The failed call must not have replaced the valid account.
	check(subsonic_stream_url("tr1", quality, url, sizeof(url), error, sizeof(error)),
		  "the valid account survived a rejected one");

	check(stream_player_init(on_event, NULL), "player init");
	stream_track_t track;
	memset(&track, 0, sizeof(track));
	snprintf(track.id, sizeof(track.id), "tr1");
	snprintf(track.title, sizeof(track.title), "First Track");
	snprintf(track.artist, sizeof(track.artist), "Tester");
	snprintf(track.album, sizeof(track.album), "Test Album");
	check(stream_player_start(&track), "playback start");

	pthread_mutex_lock(&event_lock);
	struct timespec deadline;
	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += 20;
	while (!finished && !failed) {
		if (pthread_cond_timedwait(&event_cond, &event_lock, &deadline) != 0) {
			break;
		}
	}
	bool done = finished;
	bool did_fail = failed;
	pthread_mutex_unlock(&event_lock);

	check(done, did_fail ? failure : "playback finished before the deadline");

	pthread_mutex_lock(&audio_lock);
	size_t frames = audio_frames;
	int rate = audio_rate;
	int channels = audio_channels;
	int bits = audio_bits;
	int buffer_ms = audio_buffer_ms;
	pthread_mutex_unlock(&audio_lock);
	check(frames > 0, "PCM frames were written");
	check(rate == 44100, "the MP3 sample rate was used");
	check(channels == 2, "the MP3 channel count was used");
	check(bits == 16, "MP3 stays at 16 bits");
	// 0 means the player's own frame-based sizing. Asking for 120 ms made the
	// device's ALSA reject the buffer/period combination.
	check(buffer_ms == 0, "the player's usual audio depth was requested");

	// Seeking restarts the stream at the requested second. Check both halves:
	// the clock starts at the offset immediately, and the stream really did
	// restart with timeOffset (integration.py checks the request).
	pthread_mutex_lock(&event_lock);
	started_count = 0;
	finished = false;
	failed = false;
	pthread_mutex_unlock(&event_lock);
	track.duration_ms = 15000;
	check(stream_player_start(&track), "seek test playback start");

	pthread_mutex_lock(&event_lock);
	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += 10;
	while (started_count < 1 && !failed) {
		if (pthread_cond_timedwait(&event_cond, &event_lock, &deadline) != 0) {
			break;
		}
	}
	bool seek_started = started_count >= 1;
	pthread_mutex_unlock(&event_lock);
	check(seek_started, "the stream opened before seeking");
	check(stream_player_position_ms() < 1000, "a fresh stream starts at zero");

	check(stream_player_seek(3000), "seek accepted");
	int seek_position = stream_player_position_ms();
	check(seek_position >= 3000 && seek_position < 4000,
		  "the clock starts at the seek offset, not at zero or the old position");

	pthread_mutex_lock(&event_lock);
	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += 10;
	while (started_count < 2 && !failed) {
		if (pthread_cond_timedwait(&event_cond, &event_lock, &deadline) != 0) {
			break;
		}
	}
	bool seek_restarted = started_count >= 2;
	pthread_mutex_unlock(&event_lock);
	check(seek_restarted, "the stream restarted after the seek");

	struct timespec seek_pause = {.tv_sec = 0, .tv_nsec = 120000000L};
	nanosleep(&seek_pause, NULL);
	int after_seek = stream_player_position_ms();
	check(after_seek >= 3000, "the clock advances from the seek offset");
	check(after_seek < 15000, "the clock stays inside the track");
	stream_player_stop();

	// Stop a second stream in the middle. The worker must wake from its read and
	// leave no completion event for the abandoned generation.
	pthread_mutex_lock(&event_lock);
	finished = false;
	failed = false;
	pthread_mutex_unlock(&event_lock);
	check(stream_player_start(&track), "second playback start");
	struct timespec pause = {.tv_sec = 0, .tv_nsec = 60000000L};
	nanosleep(&pause, NULL);
	stream_player_stop();
	pause.tv_nsec = 150000000L;
	nanosleep(&pause, NULL);

	pthread_mutex_lock(&audio_lock);
	size_t stopped_frames = audio_frames;
	pthread_mutex_unlock(&audio_lock);
	check(stopped_frames > 0, "the second stream started");
	check(stopped_frames < 600 * 1152, "Stop cut the stream short");
	pthread_mutex_lock(&event_lock);
	check(!finished && !failed, "Stop did not report a completion or a failure");
	pthread_mutex_unlock(&event_lock);

	// Lossless: the client asks the server for a FLAC transcode and stream_player
	// decodes it with dr_flac. The fixture is 80 * 4096 frames.
	subsonic_quality_t flac_quality = {SUBSONIC_FORMAT_FLAC, 0};
	check(subsonic_configure(&credentials, flac_quality, error, sizeof(error)), error);
	pthread_mutex_lock(&event_lock);
	finished = false;
	failed = false;
	pthread_mutex_unlock(&event_lock);

	snprintf(track.id, sizeof(track.id), "flac1");
	snprintf(track.title, sizeof(track.title), "Lossless Track");
	snprintf(track.suffix, sizeof(track.suffix), "flac");
	track.format = SUBSONIC_FORMAT_FLAC;
	check(stream_player_start(&track), "FLAC playback start");

	pthread_mutex_lock(&event_lock);
	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += 20;
	while (!finished && !failed) {
		if (pthread_cond_timedwait(&event_cond, &event_lock, &deadline) != 0) {
			break;
		}
	}
	done = finished;
	did_fail = failed;
	pthread_mutex_unlock(&event_lock);
	check(done, did_fail ? failure : "FLAC playback finished before the deadline");

	pthread_mutex_lock(&audio_lock);
	frames = audio_frames;
	rate = audio_rate;
	channels = audio_channels;
	bits = audio_bits;
	pthread_mutex_unlock(&audio_lock);
	check(frames == 80 * 4096, "the exact FLAC PCM frame count");
	check(rate == 44100, "the FLAC sample rate was used");
	check(channels == 2, "the FLAC channel count was used");
	check(bits == 32, "a 24-bit FLAC is written left-justified at 32 bits");

	stream_player_shutdown();

	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("test_stream: %zu frames at %d Hz, %d channel(s)\n", frames, rate, channels);
	return 0;
}
