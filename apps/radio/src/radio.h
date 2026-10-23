#ifndef RADIO_H
#define RADIO_H

#include <stdbool.h>
#include <stddef.h>

// Internet radio, as a Morpthis app.
//
// The directory is radio-browser.info: a community list of public stations.
// Three things are kept apart, as they were in the player:
//
//   * the directory client -- lists of genres, countries and languages, and
//     the stations under each. One worker thread and one request at a time;
//     the page polls the job state.
//   * the store -- favourites, recents and the stations the user added by
//     hand, in the app's data folder. Plain lines rather than SQLite: a few
//     hundred stations do not need a database and the SDK gives every app a
//     writable folder.
//   * the transport -- a thread that pulls the stream into a bounded ring,
//     and one that decodes out of it and writes PCM through the SDK's raw
//     output. MP3 and ADTS AAC; HLS manifests are unwrapped by hls.c.

#define RADIO_NAME_MAX 128
#define RADIO_URL_MAX 512
#define RADIO_TERM_MAX 96
#define RADIO_UUID_MAX 40

// One page of stations, and the term lists the directory sends. The UI adds
// one entry per row; a hundred is what a person scrolls through, and the
// directory's own limit matches.
#define RADIO_STATION_MAX 100
#define RADIO_TERM_LIST_MAX 160

typedef struct {
	char uuid[RADIO_UUID_MAX];
	char name[RADIO_NAME_MAX];
	char url[RADIO_URL_MAX]; // url_resolved when the directory had one
	char country[8];		 // ISO 3166-1 alpha-2, or empty
	char codec[16];
	int bitrate; // kbps, 0 when unknown
} radio_station_t;

typedef struct {
	char name[RADIO_TERM_MAX];	// what the API wants back
	char label[RADIO_TERM_MAX]; // the same thing with a capital on the front
	int station_count;
} radio_term_t;

typedef enum {
	RADIO_BROWSE_TOP = 0,
	RADIO_BROWSE_GENRES,
	RADIO_BROWSE_COUNTRIES,
	RADIO_BROWSE_LANGUAGES,
} radio_browse_t;

typedef enum {
	RADIO_JOB_IDLE = 0,
	RADIO_JOB_BUSY,
	RADIO_JOB_OK,
	RADIO_JOB_FAILED,
} radio_job_t;

// The directory. The worker owns the result until the next request; the page
// copies what it keeps when the serial changes. The calls are safe from the
// GUI thread and never block on the network.
bool radio_client_init(void);
void radio_client_shutdown(void);

// Each request returns the id the answer will carry. A page that was replaced
// while a request was in flight compares ids and drops the old answer instead
// of showing it under the new title.
unsigned radio_client_top(void);
unsigned radio_client_terms(radio_browse_t kind);
unsigned radio_client_stations(radio_browse_t kind, const char *term);
unsigned radio_client_search(const char *text);
unsigned radio_client_result_request(void);

radio_job_t radio_client_state(void);
unsigned radio_client_serial(void);
const char *radio_client_error(void);

// Copies the finished result. Returns the count when the job was successful,
// 0 otherwise. `out` may be NULL to ask for the count only.
int radio_client_copy_terms(radio_term_t *out, int max);
int radio_client_copy_stations(radio_station_t *out, int max);

// Which shape the last finished answer has. A request that was already in
// flight when the page changed is still delivered; the page uses this to drop
// an answer it no longer asked for instead of reading it as the wrong type.
bool radio_client_result_is_terms(void);

#endif /* RADIO_H */
