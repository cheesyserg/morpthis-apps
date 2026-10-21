#include "settings.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <morpthis/morpthis.h>

#include "vendor/json.h"

#define SETTINGS_MAX 8192

static const char *const template_text =
	"{\n"
	"  \"server\": \"\",\n"
	"  \"username\": \"\",\n"
	"  \"password\": \"\",\n"
	"  \"format\": \"mp3\",\n"
	"  \"max_bitrate\": 256,\n"
	"  \"tls_verify\": \"strict\",\n"
	"  \"ca_bundle\": \"\"\n"
	"}\n";

void settings_defaults(subsonic_settings_t *out) {
	if (!out) {
		return;
	}
	memset(out, 0, sizeof(*out));
	out->quality.format = SUBSONIC_FORMAT_MP3;
	out->quality.max_bitrate = 256;
}

static void reason(char *out, size_t size, const char *text) {
	if (out && size) {
		snprintf(out, size, "%s", text);
	}
}

bool settings_write_template(void) {
	// Called only when the readable tree has no settings.json at all. Writing
	// unconditionally is deliberate: morpthis_fs_exists() also sees a file in
	// the read-only package, and a shipped default must not stop the writable
	// copy from being created.
	return morpthis_fs_write_all(SUBSONIC_SETTINGS_FILE, template_text, strlen(template_text));
}

bool settings_load(subsonic_settings_t *out, char *error, size_t error_size) {
	if (!out) {
		reason(error, error_size, "Internal error.");
		return false;
	}
	settings_defaults(out);
	reason(error, error_size, "");

	char text[SETTINGS_MAX];
	size_t length = 0;
	long got = morpthis_fs_read_all(SUBSONIC_SETTINGS_FILE, text, sizeof(text), &length);
	if (got < 0) {
		if (errno == ENOENT) {
			out->file_missing = true;
			out->configured = false;
			if (!settings_write_template()) {
				morpthis_logw("subsonic", "could not create settings.json in the data folder");
			}
			return true;
		}
		reason(error, error_size, "Cannot read settings.json from this app's data folder.");
		return false;
	}
	(void)length;

	json_doc_t doc;
	if (!json_parse(text, &doc)) {
		reason(error, error_size, "settings.json is not valid JSON.");
		return false;
	}
	int root = json_root(&doc);

	json_str_exact(&doc, json_get(&doc, root, "server"), out->credentials.server,
				   sizeof(out->credentials.server));
	json_str_exact(&doc, json_get(&doc, root, "username"), out->credentials.username,
				   sizeof(out->credentials.username));
	json_str_exact(&doc, json_get(&doc, root, "password"), out->credentials.password,
				   sizeof(out->credentials.password));

	char format[16] = "mp3";
	json_obj_str(&doc, root, "format", format, sizeof(format));
	if (format[0] && strcasecmp(format, "mp3") != 0) {
		// The app carries dr_mp3 and nothing else. Saying so now is better than
		// opening a stream the decoder cannot read.
		reason(error, error_size, "This app decodes MP3 only; set \"format\": \"mp3\" in settings.json.");
		json_free(&doc);
		return false;
	}
	out->quality.format = SUBSONIC_FORMAT_MP3;

	long bitrate = json_obj_long(&doc, root, "max_bitrate", 256);
	if (bitrate < 64) bitrate = 64;
	if (bitrate > 320) bitrate = 320;
	out->quality.max_bitrate = (int)bitrate;

	char verify[16] = "strict";
	json_obj_str(&doc, root, "tls_verify", verify, sizeof(verify));
	out->tls_relaxed = strcasecmp(verify, "relaxed") == 0;

	json_str_exact(&doc, json_get(&doc, root, "ca_bundle"), out->ca_bundle,
				   sizeof(out->ca_bundle));
	json_free(&doc);

	out->configured = out->credentials.server[0] && out->credentials.username[0] &&
					  out->credentials.password[0];
	return true;
}

// ---------------------------------------------------------------------------
// Writing the file back
// ---------------------------------------------------------------------------
//
// The toggles on the settings screen change two fields and must leave the
// password where the user put it. Round-tripping through the parser and this
// writer produces a canonical file; it does mean the spacing and any unknown
// fields are not preserved, which the README says.

static bool append(char *out, size_t size, size_t *used, const char *text) {
	size_t length = strlen(text);
	if (*used + length >= size) {
		return false;
	}
	memcpy(out + *used, text, length);
	*used += length;
	out[*used] = '\0';
	return true;
}

static bool append_string(char *out, size_t size, size_t *used, const char *text) {
	if (!append(out, size, used, "\"")) {
		return false;
	}
	for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
		if (*p == '"' || *p == '\\') {
			char escaped[3] = {'\\', (char)*p, '\0'};
			if (!append(out, size, used, escaped)) {
				return false;
			}
		} else if (*p < 32) {
			char escaped[8];
			snprintf(escaped, sizeof(escaped), "\\u%04x", *p);
			if (!append(out, size, used, escaped)) {
				return false;
			}
		} else {
			char one[2] = {(char)*p, '\0'};
			if (!append(out, size, used, one)) {
				return false;
			}
		}
	}
	return append(out, size, used, "\"");
}

bool settings_save(const subsonic_settings_t *in) {
	if (!in) {
		return false;
	}
	char text[SETTINGS_MAX];
	size_t used = 0;
	text[0] = '\0';

	bool ok = append(text, sizeof(text), &used, "{\n  \"server\": ") &&
			  append_string(text, sizeof(text), &used, in->credentials.server) &&
			  append(text, sizeof(text), &used, ",\n  \"username\": ") &&
			  append_string(text, sizeof(text), &used, in->credentials.username) &&
			  append(text, sizeof(text), &used, ",\n  \"password\": ") &&
			  append_string(text, sizeof(text), &used, in->credentials.password) &&
			  append(text, sizeof(text), &used, ",\n  \"format\": \"mp3\",\n  \"max_bitrate\": ");
	char number[16];
	snprintf(number, sizeof(number), "%d", in->quality.max_bitrate);
	ok = ok && append(text, sizeof(text), &used, number) &&
		 append(text, sizeof(text), &used, ",\n  \"tls_verify\": \"") &&
		 append(text, sizeof(text), &used, in->tls_relaxed ? "relaxed" : "strict") &&
		 append(text, sizeof(text), &used, "\",\n  \"ca_bundle\": ") &&
		 append_string(text, sizeof(text), &used, in->ca_bundle) &&
		 append(text, sizeof(text), &used, "\n}\n");
	if (!ok) {
		return false;
	}
	return morpthis_fs_write_all(SUBSONIC_SETTINGS_FILE, text, used);
}
