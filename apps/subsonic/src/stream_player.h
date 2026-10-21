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
	subsonic_format_t format;
} stream_track_t;

typedef enum {
	STREAM_EVENT_FINISHED,
	STREAM_EVENT_FAILED,
	STREAM_EVENT_STARTED,  // now playing
	STREAM_EVENT_LISTENED, // past the thirty-second mark
} stream_event_kind_t;

typedef void (*stream_event_cb)(stream_event_kind_t kind, const char *message,
								const stream_track_t *track, void *user);

bool stream_player_init(stream_event_cb cb, void *user);

// Replaces whatever is playing. Returns false when the worker is not running.
bool stream_player_start(const stream_track_t *track);

// Stops the current stream; the worker stays alive for the next track.
void stream_player_stop(void);

// Holds the stream where it is. The socket stays open, so a pause that outlasts
// the server's own timeout may end the track when it resumes.
void stream_player_pause(bool paused);

void stream_player_shutdown(void);

#endif
