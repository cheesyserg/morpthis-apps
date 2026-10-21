#ifndef SUBSONIC_SETTINGS_H
#define SUBSONIC_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>

#include "subsonic.h"

#define SUBSONIC_SETTINGS_FILE "settings.json"

typedef struct {
	subsonic_credentials_t credentials;
	subsonic_quality_t quality;
	bool tls_relaxed;
	char ca_bundle[SUBSONIC_CA_MAX];
	bool configured;   // server, username and password are all present
	bool file_missing; // no settings.json yet; the template was written
} subsonic_settings_t;

void settings_defaults(subsonic_settings_t *out);

// Reads settings.json through the SDK. A missing file is not an error: it is
// created from a template and `file_missing` is set. A malformed file is an
// error, with a sentence for the screen.
bool settings_load(subsonic_settings_t *out, char *error, size_t error_size);

// Writes the current settings back, including the password. Used by the TLS and
// quality toggles; the app never invents credentials, it only preserves what the
// user put in the file.
bool settings_save(const subsonic_settings_t *in);

// Writes the template. Called only when settings.json is absent from the
// writable tree, so it does not overwrite a user's file.
bool settings_write_template(void);

#endif
