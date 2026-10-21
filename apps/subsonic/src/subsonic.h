#ifndef SUBSONIC_H
#define SUBSONIC_H

// Subsonic / Gonic / Navidrome, from inside a Morpthis app.
//
// One account, configured by the user in settings.json, and plain strings in the
// errors the screens show. Every call blocks and belongs on a worker thread.

#include <stdbool.h>
#include <stddef.h>

#define SUBSONIC_SERVER_MAX 512
#define SUBSONIC_USER_MAX 128
#define SUBSONIC_PASSWORD_MAX 256
#define SUBSONIC_ID_MAX 128
#define SUBSONIC_URL_MAX 2048
#define SUBSONIC_PAGE_SIZE 200
#define SUBSONIC_CA_MAX 512

typedef struct {
	char server[SUBSONIC_SERVER_MAX]; // base URL, with any reverse-proxy prefix
	char username[SUBSONIC_USER_MAX];
	char password[SUBSONIC_PASSWORD_MAX];
} subsonic_credentials_t;

typedef enum {
	SUBSONIC_FORMAT_MP3 = 0,
	SUBSONIC_FORMAT_OPUS,
	SUBSONIC_FORMAT_RAW,
	SUBSONIC_FORMAT_FLAC, // server-side lossless transcode
} subsonic_format_t;

typedef struct {
	subsonic_format_t format;
	int max_bitrate; // 64..320 kbps; ignored for raw and FLAC
} subsonic_quality_t;

typedef enum {
	SUBSONIC_MUSIC_FOLDER,
	SUBSONIC_DIRECTORY,
	SUBSONIC_ARTIST,
	SUBSONIC_ALBUM,
	SUBSONIC_TRACK,
	SUBSONIC_PLAYLIST,
	SUBSONIC_GENRE,
	SUBSONIC_MENU, // app-side root-menu row, never sent by the server
} subsonic_entry_kind_t;

typedef struct {
	subsonic_entry_kind_t kind;
	char id[SUBSONIC_ID_MAX]; // opaque strings, not necessarily numeric
	char title[200];
	char artist[128];
	char album[200];
	char suffix[16];
	int duration;
	int track_number;
	bool starred;
} subsonic_entry_t;

typedef enum {
	SUBSONIC_BROWSE_FOLDERS,
	SUBSONIC_BROWSE_INDEXES,
	SUBSONIC_BROWSE_DIRECTORY,
	SUBSONIC_BROWSE_ARTISTS,
	SUBSONIC_BROWSE_ARTIST,
	SUBSONIC_BROWSE_ALBUM,
	SUBSONIC_BROWSE_ALBUMS,
	SUBSONIC_BROWSE_PLAYLISTS,
	SUBSONIC_BROWSE_PLAYLIST,
	SUBSONIC_BROWSE_STARRED,
	SUBSONIC_BROWSE_RANDOM,
	SUBSONIC_BROWSE_NEWEST,
	SUBSONIC_BROWSE_GENRES,
	SUBSONIC_BROWSE_GENRE,
	SUBSONIC_BROWSE_SEARCH,
} subsonic_browse_t;

void subsonic_client_init(void);

// Validates and stores one account. The server must be an http:// or https://
// base URL; a pasted /rest suffix is removed. On failure `error` says why.
bool subsonic_configure(const subsonic_credentials_t *credentials,
						subsonic_quality_t quality, char *error, size_t error_size);
bool subsonic_configured(void);
void subsonic_disconnect(void);
void subsonic_credentials(subsonic_credentials_t *out);
subsonic_quality_t subsonic_quality(void);
void subsonic_set_quality(subsonic_quality_t quality);

bool subsonic_ping(char *error, size_t error_size);

// Pages of at most SUBSONIC_PAGE_SIZE entries. getArtists/getIndexes have no
// server-side pagination in the protocol, so offset is applied while walking
// the JSON tokens. `more` is explicit, never a silent truncation.
int subsonic_browse(subsonic_browse_t browse, const char *id, int offset,
					subsonic_entry_t *entries, int limit, bool *more,
					char *error, size_t error_size);

// URL construction only, no network. Fresh /dev/urandom salt, token auth,
// v=1.16.1, c=morpthis, f=json; no plaintext password in a URL or a log.
bool subsonic_stream_url(const char *id, subsonic_quality_t quality,
						 char *out, size_t size, char *error, size_t error_size);

// Stars or unstars a track, album or artist. Blocking; call it on a worker.
bool subsonic_star(subsonic_entry_kind_t kind, const char *id, bool on,
				   char *error, size_t error_size);

// Reports a play to the server (the Last.fm bridge many servers offer).
bool subsonic_scrobble(const char *id, bool submission, char *error, size_t error_size);

// Shuts down every socket the app has open, waking a worker blocked in a
// request. Used at teardown, before joining that worker.
void subsonic_abort(void);

#endif
