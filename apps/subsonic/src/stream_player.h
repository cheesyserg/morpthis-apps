#ifndef SUBSONIC_STREAM_H
#define SUBSONIC_STREAM_H

#include <stdbool.h>

#include "subsonic.h"

// The playback side: a worker thread owns the HTTP body, the MP3/FLAC decoder
// and the raw DAC output. Opening, reading, writing and closing the output all
// happen on that one thread, as the SDK requires.

typedef struct {
	char id[SUBSONIC_ID_MAX];
	char title[200];
	char artist[128];
	char album[200];
	char suffix[16];
	char cover_art[SUBSONIC_ID_MAX];
	int duration_ms;
	int start_ms; // where this request begins; 0 for the normal start
	subsonic_format_t format;
} stream_track_t;

typedef enum {
	STREAM_EVENT_FINISHED,
	STREAM_EVENT_FAILED,
	STREAM_EVENT_STARTED,  // now playing
	STREAM_EVENT_LISTENED, // past the thirty-second mark
	STREAM_EVENT_PROGRESS, // one second of audio has been sent
} stream_event_kind_t;

typedef void (*stream_event_cb)(stream_event_kind_t kind, const char *message,
								const stream_track_t *track, void *user);

bool stream_player_init(stream_event_cb cb, void *user);

// Replaces whatever is playing. Returns false when the worker is not running.
bool stream_player_start(const stream_track_t *track);

// Stops the current stream; the worker stays alive for the next track.
void stream_player_stop(void);

// Restarts the current track at `position_ms`, asking the server to begin there
// (Subsonic timeOffset, whole seconds). A paused track stays paused. False when
// nothing has been loaded yet. Servers that ignore the parameter send the
// track from the beginning again.
bool stream_player_seek(int position_ms);

// Holds the stream where it is. The socket stays open, so a pause that outlasts
// the server's own timeout may end the track when it resumes.
void stream_player_pause(bool paused);

void stream_player_shutdown(void);

// How far into the current track the worker has fed, and the duration it was
// told about. Both in milliseconds; zero when nothing is loaded.
int stream_player_position_ms(void);
int stream_player_duration_ms(void);

// What the current stream really opened at. Zero/false until the decoder has
// read the header and the output is open (STREAM_EVENT_STARTED).
int stream_player_rate(void);
int stream_player_bits(void);
bool stream_player_flac(void);

#endif
