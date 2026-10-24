#ifndef RADIO_PLAYER_H
#define RADIO_PLAYER_H

#include <stdbool.h>

#include "radio.h"

typedef enum {
	RADIO_PLAYER_IDLE = 0,
	RADIO_PLAYER_CONNECTING,
	RADIO_PLAYER_PLAYING,
	RADIO_PLAYER_PAUSED,
	RADIO_PLAYER_ERROR,
} radio_player_state_t;

typedef struct {
	radio_player_state_t state;
	radio_station_t station;
	char title[256]; // the station's ICY title, or the station's name
	char error[192];
	char codec[16];	 // what actually opened
	int bitrate;	 // kbps, 0 when unknown
	int rate;		 // Hz, 0 when not playing
	int channels;	 // the stream's channel count
	unsigned serial; // changes whenever anything above changes
} radio_now_t;

// The transport. One station at a time, on one thread; the calls are safe
// from the GUI thread and do not wait for the network. The page polls
// radio_player_get() for the state and the title.
bool radio_player_init(void);
// Signals the worker to stop, wakes its socket and aborts every HTTP socket.
// Idempotent. Call this and radio_client_shutdown_begin() before either join
// so the two workers wind down in parallel, then call the shutdown calls.
void radio_player_shutdown_begin(void);
void radio_player_shutdown(void);

// Starts `station`. A station already playing is replaced; the old socket is
// woken and its thread returns to the controller before the new one opens.
bool radio_player_play(const radio_station_t *station);

// Stops, or starts the loaded station again after a stop/error.
void radio_player_toggle_pause(void);
void radio_player_stop(void);

void radio_player_get(radio_now_t *out);

// True while a station is loaded, including paused and connecting.
bool radio_player_active(void);

#endif /* RADIO_PLAYER_H */
