// Loopback integration test for the radio app's directory client and transport.
//
// The real radio_client.c and radio_player.c are linked; the SDK calls they
// make are stubbed here. tests/integration.py serves a fake radio-browser
// directory, a dribbled MP3 stream and an HLS manifest with bare MP3 segments.

#include <morpthis/morpthis.h>

#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "radio.h"
#include "radio_player.h"

// ---------------------------------------------------------------------------
// SDK stubs
// ---------------------------------------------------------------------------

static pthread_mutex_t audio_lock = PTHREAD_MUTEX_INITIALIZER;
static size_t audio_frames;
static bool audio_open;
static int audio_rate;
static int audio_channels;
static int audio_bits;
static morpthis_power_t power_holds;
static morpthis_track_t published;
static bool published_valid;
static bool published_playing;

bool morpthis_audio_open_ex(int rate, int channels, int bits, int buffer_ms) {
	(void)buffer_ms;
	pthread_mutex_lock(&audio_lock);
	audio_open = true;
	audio_rate = rate;
	audio_channels = channels;
	audio_bits = bits;
	pthread_mutex_unlock(&audio_lock);
	return true;
}

bool morpthis_audio_open(int rate, int channels, int buffer_ms) {
	return morpthis_audio_open_ex(rate, channels, 16, buffer_ms);
}

size_t morpthis_audio_write_pcm(const void *pcm, size_t frames) {
	(void)pcm;
	pthread_mutex_lock(&audio_lock);
	if (!audio_open) {
		pthread_mutex_unlock(&audio_lock);
		return 0;
	}
	audio_frames += frames;
	pthread_mutex_unlock(&audio_lock);
	// Faster than realtime, but enough that the test can observe a state
	// change before the whole fixture has played.
	struct timespec pause = {.tv_sec = 0, .tv_nsec = 500000L};
	nanosleep(&pause, NULL);
	return frames;
}

size_t morpthis_audio_write(const int16_t *pcm, size_t frames) {
	return morpthis_audio_write_pcm(pcm, frames);
}

void morpthis_audio_set_playing(bool playing) {
	pthread_mutex_lock(&audio_lock);
	if (!playing) {
		audio_open = false;
	}
	published_playing = playing;
	pthread_mutex_unlock(&audio_lock);
}

void morpthis_audio_set_track(const morpthis_track_t *track, bool playing) {
	pthread_mutex_lock(&audio_lock);
	if (track) {
		published = *track;
		published_valid = true;
	} else {
		memset(&published, 0, sizeof(published));
		published_valid = false;
	}
	published_playing = playing;
	pthread_mutex_unlock(&audio_lock);
}

void morpthis_audio_close(void) {
	pthread_mutex_lock(&audio_lock);
	audio_open = false;
	pthread_mutex_unlock(&audio_lock);
}

void morpthis_power_set_holds(morpthis_power_t holds) {
	pthread_mutex_lock(&audio_lock);
	power_holds = holds;
	pthread_mutex_unlock(&audio_lock);
}

void morpthis_power_release_all(void) {
	morpthis_power_set_holds(MORPTHIS_POWER_NONE);
}

uint32_t morpthis_app_time_ms(void) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint32_t)((uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u);
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
// Harness
// ---------------------------------------------------------------------------

static int failures;

static void check(bool ok, const char *what) {
	if (!ok) {
		fprintf(stderr, "FAIL: %s\n", what);
		failures++;
	}
}

static bool wait_client(radio_job_t want, int timeout_ms) {
	for (int waited = 0; waited < timeout_ms; waited += 20) {
		if (radio_client_state() == want) {
			return true;
		}
		usleep(20 * 1000);
	}
	return false;
}

static bool wait_state(radio_player_state_t want, int timeout_ms) {
	radio_now_t now;
	for (int waited = 0; waited < timeout_ms; waited += 20) {
		radio_player_get(&now);
		if (now.state == want) {
			return true;
		}
		usleep(20 * 1000);
	}
	return false;
}

static size_t current_frames(void) {
	pthread_mutex_lock(&audio_lock);
	size_t frames = audio_frames;
	pthread_mutex_unlock(&audio_lock);
	return frames;
}

static int main_client(const char *base) {
	radio_term_t terms[RADIO_TERM_LIST_MAX];
	radio_station_t stations[RADIO_STATION_MAX];

	radio_client_terms(RADIO_BROWSE_GENRES);
	check(wait_client(RADIO_JOB_OK, 5000), "genres answered");
	int count = radio_client_copy_terms(terms, RADIO_TERM_LIST_MAX);
	check(count == 2, "two genres were parsed");
	if (count == 2) {
		check(strcmp(terms[0].label, "Rock") == 0, "the first genre is Rock");
		check(terms[0].station_count == 120, "the station count is read");
		check(strcmp(terms[1].name, "jazz") == 0, "the API name is kept");
	}

	radio_client_terms(RADIO_BROWSE_COUNTRIES);
	check(wait_client(RADIO_JOB_OK, 5000), "countries answered");
	count = radio_client_copy_terms(terms, RADIO_TERM_LIST_MAX);
	check(count == 1, "one country was parsed");
	if (count == 1) {
		check(strcmp(terms[0].label, "Italy") == 0, "the country label is the everyday name");
	}

	radio_client_search("test");
	check(wait_client(RADIO_JOB_OK, 5000), "search answered");
	count = radio_client_copy_stations(stations, RADIO_STATION_MAX);
	check(count == 2, "two stations were parsed");
	if (count == 2) {
		check(strcmp(stations[0].name, "Test One") == 0, "the first station name");
		check(strcmp(stations[0].url, "http://example.invalid/one") == 0,
			  "url_resolved is preferred");
		check(strcmp(stations[1].url, "http://example.invalid/two") == 0,
			  "the plain url is the fallback");
		check(stations[0].bitrate == 128, "the bitrate is read");
	}

	radio_client_stations(RADIO_BROWSE_COUNTRIES, "Italy");
	check(wait_client(RADIO_JOB_OK, 5000), "country stations answered");
	count = radio_client_copy_stations(stations, RADIO_STATION_MAX);
	check(count == 1, "the country returns its station");
	check(radio_client_result_is_terms() == false, "the last answer has the stations shape");

	(void)base;
	return 0;
}

static void main_player(const char *base) {
	radio_station_t station;
	memset(&station, 0, sizeof(station));
	snprintf(station.name, sizeof(station.name), "Test Station");
	snprintf(station.url, sizeof(station.url), "%s/stream.mp3", base);
	station.bitrate = 128;

	check(radio_player_init(), "player init");
	size_t before = current_frames();
	check(radio_player_play(&station), "play accepted");
	check(wait_state(RADIO_PLAYER_PLAYING, 10000), "the MP3 stream started");

	pthread_mutex_lock(&audio_lock);
	int rate = audio_rate;
	int channels = audio_channels;
	morpthis_power_t holds = power_holds;
	pthread_mutex_unlock(&audio_lock);
	check(rate == 44100, "the MP3 sample rate was used");
	check(channels == 2, "the output was opened for stereo");
	check((holds & (MORPTHIS_POWER_AWAKE | MORPTHIS_POWER_NETWORK)) ==
			  (MORPTHIS_POWER_AWAKE | MORPTHIS_POWER_NETWORK),
		  "playing takes the AWAKE | NETWORK hold");
	check(current_frames() > before, "PCM frames were written");

	radio_now_t now;
	radio_player_get(&now);
	check(strcmp(now.title, "Test Station") == 0, "without ICY the station is the title");
	pthread_mutex_lock(&audio_lock);
	check(published_valid && strcmp(published.title, "Test Station") == 0,
		  "the track was published for the control centre");
	check(published_playing, "the published track says it is playing");
	pthread_mutex_unlock(&audio_lock);

	// Pause closes the stream and the output; the state must flip at once and
	// the frames must stop growing.
	radio_player_toggle_pause();
	check(wait_state(RADIO_PLAYER_PAUSED, 2000), "pause is immediate");
	size_t paused_at = current_frames();
	usleep(300 * 1000);
	check(current_frames() - paused_at < 4096, "pause stopped the PCM writes");
	pthread_mutex_lock(&audio_lock);
	check(power_holds == MORPTHIS_POWER_NONE, "pause released the power holds");
	pthread_mutex_unlock(&audio_lock);

	// Resume reconnects the station at the live point.
	radio_player_toggle_pause();
	check(wait_state(RADIO_PLAYER_PLAYING, 10000), "resume reconnected");
	size_t resumed_at = current_frames();
	usleep(300 * 1000);
	check(current_frames() > resumed_at, "resume produced more audio");

	radio_player_stop();
	check(wait_state(RADIO_PLAYER_IDLE, 3000), "stop returned to idle");
	pthread_mutex_lock(&audio_lock);
	check(!published_valid, "stop cleared the published track");
	check(power_holds == MORPTHIS_POWER_NONE, "stop released the power holds");
	pthread_mutex_unlock(&audio_lock);

	// ICY: the station announces a song inside the stream. The title is held
	// with the byte position it arrived at, so it must not appear before the
	// decoder reaches it -- but it must appear.
	radio_station_t icy;
	memset(&icy, 0, sizeof(icy));
	snprintf(icy.name, sizeof(icy.name), "ICY Station");
	snprintf(icy.url, sizeof(icy.url), "%s/icy.mp3", base);
	check(radio_player_play(&icy), "ICY play accepted");
	bool icy_started = wait_state(RADIO_PLAYER_PLAYING, 10000);
	if (!icy_started) {
		radio_player_get(&now);
		fprintf(stderr, "icy: state %d, error %s\n", (int)now.state, now.error);
	}
	check(icy_started, "the ICY stream started");
	bool title_seen = false;
	for (int waited = 0; waited < 8000; waited += 50) {
		radio_player_get(&now);
		if (strcmp(now.title, "Test Song") == 0) {
			title_seen = true;
			break;
		}
		usleep(50 * 1000);
	}
	check(title_seen, "the ICY StreamTitle was carried to the now-playing title");
	pthread_mutex_lock(&audio_lock);
	check(published_valid && strcmp(published.title, "Test Song") == 0,
		  "the ICY title was published to the control centre");
	check(strcmp(published.artist, "ICY Station") == 0,
		  "the station name is published as the artist");
	pthread_mutex_unlock(&audio_lock);
	radio_player_stop();
	check(wait_state(RADIO_PLAYER_IDLE, 3000), "ICY stop returned to idle");

	// HLS: the manifest points at two bare MP3 segments, and the player must
	// unwrap them and decode them as one stream.
	radio_station_t hls;
	memset(&hls, 0, sizeof(hls));
	snprintf(hls.name, sizeof(hls.name), "Test HLS");
	snprintf(hls.url, sizeof(hls.url), "%s/live.m3u8", base);
	size_t hls_before = current_frames();
	check(radio_player_play(&hls), "HLS play accepted");
	check(wait_state(RADIO_PLAYER_PLAYING, 15000), "the HLS stream started");
	usleep(400 * 1000);
	check(current_frames() > hls_before, "HLS produced PCM");
	radio_now_t hls_now;
	radio_player_get(&hls_now);
	check(strcmp(hls_now.codec, "MP3") == 0, "HLS reported the MP3 codec");
	radio_player_stop();
	check(wait_state(RADIO_PLAYER_IDLE, 3000), "HLS stop returned to idle");

	radio_player_shutdown();
}

int main(int argc, char **argv) {
	(void)argc;
	(void)argv;
	signal(SIGPIPE, SIG_IGN);

	const char *port = getenv("RADIO_TEST_PORT");
	if (!port || !port[0]) {
		fprintf(stderr, "FAIL: RADIO_TEST_PORT is not set; run tests/integration.py\n");
		return 2;
	}
	char base[64];
	snprintf(base, sizeof(base), "http://127.0.0.1:%s", port);
	if (setenv("MORPTHIS_RADIO_API_HOST", base, 1) != 0) {
		fprintf(stderr, "FAIL: could not set the API override\n");
		return 2;
	}

	check(radio_client_init(), "client init");
	main_client(base);
	main_player(base);
	radio_client_shutdown();

	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	fprintf(stderr, "test_radio: ok\n");
	return 0;
}
