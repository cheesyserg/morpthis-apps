#include "radio.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "http.h"
#include "tls.h"

// The directory is a set of mirrors behind one name. A host that answered is
// tried first next time; the others are kept so one outage does not take the
// page down with it.
#define API_HOST_COUNT 3
static const char *const API_HOSTS[API_HOST_COUNT] = {
	"de2.api.radio-browser.info",
	"de1.api.radio-browser.info",
	"all.api.radio-browser.info",
};

#define API_MAX_BYTES (1024 * 1024)
// A directory answer is small. Eight seconds is generous for this device's
// TLS handshake and short enough that a dead mirror does not hold the page
// for a minute; four attempts bound the whole lookup at half that.
#define API_TIMEOUT_SECS 8
#define API_MAX_ATTEMPTS 4
#define API_PAGE_LIMIT 100
#define API_TERM_TEXT "160"

#define WORKER_STACK (256 * 1024)

// ---------------------------------------------------------------------------
// Reading the directory's JSON by hand
//
// radio-browser.info answers with an array of flat objects: strings, a few
// numbers and booleans, no nesting. That is a small enough language to read
// without a parser, and it avoids keeping a token for every field of every
// station on a device with 64 MB.
// ---------------------------------------------------------------------------

static const char *skip_string(const char *p) {
	p++; // the opening quote
	while (*p) {
		if (*p == '\\' && p[1]) {
			p += 2;
			continue;
		}
		if (*p == '"') {
			return p + 1;
		}
		p++;
	}
	return p;
}

// The next object in an array, or NULL. *end points just past its closing
// brace on return.
static const char *next_object(const char *p, const char **end) {
	while (*p && *p != '{') {
		if (*p == '"') {
			p = skip_string(p);
			continue;
		}
		p++;
	}
	if (!*p) {
		return NULL;
	}

	const char *start = p;
	int depth = 0;
	while (*p) {
		if (*p == '"') {
			p = skip_string(p);
			continue;
		}
		if (*p == '{') {
			depth++;
		} else if (*p == '}') {
			depth--;
			if (depth == 0) {
				*end = p + 1;
				return start;
			}
		}
		p++;
	}
	return NULL;
}

static int utf8_put(char *out, size_t room, unsigned cp) {
	if (cp < 0x80 && room >= 1) {
		out[0] = (char)cp;
		return 1;
	}
	if (cp < 0x800 && room >= 2) {
		out[0] = (char)(0xC0 | (cp >> 6));
		out[1] = (char)(0x80 | (cp & 0x3F));
		return 2;
	}
	if (cp < 0x10000 && room >= 3) {
		out[0] = (char)(0xE0 | (cp >> 12));
		out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
		out[2] = (char)(0x80 | (cp & 0x3F));
		return 3;
	}
	if (room >= 4) {
		out[0] = (char)(0xF0 | (cp >> 18));
		out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
		out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
		out[3] = (char)(0x80 | (cp & 0x3F));
		return 4;
	}
	return 0;
}

static unsigned hex4(const char *p) {
	unsigned v = 0;
	for (int i = 0; i < 4; i++) {
		char c = p[i];
		v <<= 4;
		if (c >= '0' && c <= '9') {
			v |= (unsigned)(c - '0');
		} else if (c >= 'a' && c <= 'f') {
			v |= (unsigned)(c - 'a' + 10);
		} else if (c >= 'A' && c <= 'F') {
			v |= (unsigned)(c - 'A' + 10);
		} else {
			return 0;
		}
	}
	return v;
}

static void unescape_into(const char *p, char *out, size_t out_size) {
	size_t o = 0;
	p++; // opening quote

	while (*p && *p != '"' && o + 1 < out_size) {
		if (*p != '\\') {
			out[o++] = *p++;
			continue;
		}

		p++;
		switch (*p) {
		case 'n':
			out[o++] = '\n';
			p++;
			break;
		case 't':
			out[o++] = '\t';
			p++;
			break;
		case 'r':
			p++;
			break; // a CR in a station name is noise
		case 'b':
		case 'f':
			p++;
			break;
		case 'u': {
			unsigned cp = hex4(p + 1);
			p += 5;
			if (cp >= 0xD800 && cp <= 0xDBFF && p[0] == '\\' && p[1] == 'u') {
				unsigned low = hex4(p + 2);
				if (low >= 0xDC00 && low <= 0xDFFF) {
					cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
					p += 6;
				}
			}
			o += (size_t)utf8_put(out + o, out_size - o - 1, cp);
			break;
		}
		case '\0':
			break;
		default:
			out[o++] = *p++;
			break;
		}
	}
	out[o] = '\0';
}

static const char *find_value(const char *obj, const char *obj_end, const char *key) {
	size_t key_len = strlen(key);
	const char *p = obj + 1;

	while (p < obj_end) {
		if (*p != '"') {
			p++;
			continue;
		}
		const char *name = p;
		const char *after = skip_string(p);

		const char *colon = after;
		while (colon < obj_end && (*colon == ' ' || *colon == '\t' || *colon == '\n')) {
			colon++;
		}
		if (colon < obj_end && *colon == ':') {
			if ((size_t)(after - name - 2) == key_len && strncmp(name + 1, key, key_len) == 0) {
				const char *v = colon + 1;
				while (v < obj_end && (*v == ' ' || *v == '\t' || *v == '\n')) {
					v++;
				}
				return v;
			}
			const char *v = colon + 1;
			while (v < obj_end && (*v == ' ' || *v == '\t' || *v == '\n')) {
				v++;
			}
			p = (*v == '"') ? skip_string(v) : v;
			continue;
		}
		p = after;
	}
	return NULL;
}

static void json_string(const char *obj, const char *obj_end, const char *key, char *out, size_t out_size) {
	out[0] = '\0';
	const char *v = find_value(obj, obj_end, key);
	if (!v) {
		return;
	}
	if (*v == '"') {
		unescape_into(v, out, out_size);
		return;
	}
	size_t n = 0;
	while (v[n] && n + 1 < out_size && v[n] != ',' && v[n] != '}' && v[n] != ' ') {
		n++;
	}
	memcpy(out, v, n);
	out[n] = '\0';
}

static int json_int(const char *obj, const char *obj_end, const char *key) {
	char buf[32];
	json_string(obj, obj_end, key, buf, sizeof(buf));
	return atoi(buf);
}

// ---------------------------------------------------------------------------
// Country names
//
// The directory uses ISO 3166 names: "The Russian Federation", "Korea,
// Republic Of". Sorted as they come, Russia sits under T and both Koreas under
// K with nothing to tell them apart. The label shown and sorted on is the
// everyday name; the search still sends the directory's own name.
// ---------------------------------------------------------------------------

static const struct {
	const char code[3];
	const char *name;
} COUNTRY_NAMES[] = {
	{"AE", "United Arab Emirates"}, {"BN", "Brunei"},		  {"BO", "Bolivia"},
	{"BQ", "Caribbean Netherlands"}, {"BS", "Bahamas"},		  {"CD", "DR Congo"},
	{"CF", "Central African Republic"}, {"CG", "Congo"},	  {"CV", "Cape Verde"},
	{"CZ", "Czechia"},				{"DO", "Dominican Republic"}, {"FK", "Falkland Islands"},
	{"FM", "Micronesia"},			{"FO", "Faroe Islands"},  {"GB", "United Kingdom"},
	{"GM", "Gambia"},				{"HK", "Hong Kong"},	 {"IR", "Iran"},
	{"KM", "Comoros"},				{"KP", "North Korea"},  {"KR", "South Korea"},
	{"KY", "Cayman Islands"},		{"LA", "Laos"},			 {"MD", "Moldova"},
	{"MF", "Saint Martin"},			{"MH", "Marshall Islands"}, {"MK", "North Macedonia"},
	{"MO", "Macao"},				{"MV", "Maldives"},	 {"NE", "Niger"},
	{"NL", "Netherlands"},			{"PH", "Philippines"},	  {"PS", "Palestine"},
	{"RU", "Russia"},				{"SD", "Sudan"},	 {"SY", "Syria"},
	{"SZ", "Eswatini"},				{"TC", "Turks and Caicos Islands"}, {"TL", "Timor-Leste"},
	{"TW", "Taiwan"},				{"TZ", "Tanzania"},	 {"US", "United States"},
	{"VA", "Vatican City"},			{"VE", "Venezuela"},	  {"VG", "British Virgin Islands"},
	{"VI", "US Virgin Islands"},	{"VN", "Vietnam"},
};

static void country_label(const char *name, const char *code, char *out, size_t size) {
	for (size_t i = 0; i < sizeof(COUNTRY_NAMES) / sizeof(COUNTRY_NAMES[0]); i++) {
		if (strcasecmp(code, COUNTRY_NAMES[i].code) == 0) {
			snprintf(out, size, "%s", COUNTRY_NAMES[i].name);
			return;
		}
	}

	if (strncasecmp(name, "the ", 4) == 0) {
		name += 4;
	}
	snprintf(out, size, "%s", name);
	char *cut = strpbrk(out, ",(");
	if (cut && cut > out) {
		*cut = '\0';
		for (char *e = cut - 1; e >= out && *e == ' '; e--) {
			*e = '\0';
		}
	}
}

// ---------------------------------------------------------------------------
// Fetching
// ---------------------------------------------------------------------------

static int api_host_index;
static bool https_ready;
static bool https_checked;

static char *api_get(const char *path) {
	// A development and test override: a full base URL, or host:port. It is
	// read on every request so a test can point the app at a loopback server
	// without rebuilding, and a user with a private mirror can do the same.
	const char *override = getenv("MORPTHIS_RADIO_API_HOST");
	if (override && *override) {
		char url[1400];
		bool has_scheme = strstr(override, "://") != NULL;
		snprintf(url, sizeof(url), "%s%s%s", has_scheme ? "" : "http://", override, path);
		char *body = NULL;
		if (http_get(url, &body, NULL, API_MAX_BYTES, API_TIMEOUT_SECS)) {
			return body;
		}
	}

	if (!https_checked) {
		https_ready = tls_available();
		https_checked = true;
	}

	// A host that answered last time is tried first, on the scheme that
	// worked. Then its plain form, then the next mirror: the directory has
	// moved hosts before, and a mirror with a broken certificate is still a
	// working directory over http://.
	char url[1400];
	int schemes = https_ready ? 2 : 1;
	int attempts = 0;
	for (int i = 0; i < API_HOST_COUNT; i++) {
		int index = (api_host_index + i) % API_HOST_COUNT;
		for (int attempt = 0; attempt < schemes; attempt++) {
			if (attempts++ >= API_MAX_ATTEMPTS) {
				return NULL;
			}
			bool https = https_ready && attempt == 0;
			snprintf(url, sizeof(url), "%s://%s%s", https ? "https" : "http", API_HOSTS[index], path);
			char *body = NULL;
			if (http_get(url, &body, NULL, API_MAX_BYTES, API_TIMEOUT_SECS)) {
				api_host_index = index;
				return body;
			}
		}
	}
	return NULL;
}

// ---------------------------------------------------------------------------
// Parsing a response
// ---------------------------------------------------------------------------

static int parse_terms(const char *json, radio_browse_t kind, radio_term_t *out, int max) {
	int count = 0;
	const char *p = json;
	const char *end;

	while (count < max) {
		const char *obj = next_object(p, &end);
		if (!obj) {
			break;
		}
		p = end;

		radio_term_t *t = &out[count];
		memset(t, 0, sizeof(*t));

		json_string(obj, end, "name", t->name, sizeof(t->name));
		if (!t->name[0]) {
			continue;
		}
		t->station_count = json_int(obj, end, "stationcount");

		if (kind == RADIO_BROWSE_COUNTRIES) {
			char code[8];
			json_string(obj, end, "iso_3166_1", code, sizeof(code));
			country_label(t->name, code, t->label, sizeof(t->label));
		} else {
			snprintf(t->label, sizeof(t->label), "%s", t->name);
		}
		if (t->label[0] >= 'a' && t->label[0] <= 'z') {
			t->label[0] = (char)(t->label[0] - 'a' + 'A');
		}

		count++;
	}
	return count;
}

static int parse_stations(const char *json, radio_station_t *out, int max) {
	int count = 0;
	const char *p = json;
	const char *end;

	while (count < max) {
		const char *obj = next_object(p, &end);
		if (!obj) {
			break;
		}
		p = end;

		radio_station_t *s = &out[count];
		memset(s, 0, sizeof(*s));

		json_string(obj, end, "stationuuid", s->uuid, sizeof(s->uuid));
		json_string(obj, end, "name", s->name, sizeof(s->name));
		json_string(obj, end, "url_resolved", s->url, sizeof(s->url));
		if (!s->url[0]) {
			json_string(obj, end, "url", s->url, sizeof(s->url));
		}
		json_string(obj, end, "countrycode", s->country, sizeof(s->country));
		json_string(obj, end, "codec", s->codec, sizeof(s->codec));
		s->bitrate = json_int(obj, end, "bitrate");

		if (!s->name[0] || !s->url[0]) {
			continue;
		}
		for (int i = (int)strlen(s->name) - 1; i >= 0 && (s->name[i] == ' ' || s->name[i] == '\t'); i--) {
			s->name[i] = '\0';
		}

		count++;
	}
	return count;
}

static int term_by_label(const void *a, const void *b) {
	const radio_term_t *x = a;
	const radio_term_t *y = b;
	int order = strcasecmp(x->label, y->label);
	return order ? order : strcmp(x->name, y->name);
}

static void sort_terms(radio_browse_t kind, radio_term_t *terms, int count) {
	// Genres keep the directory's popularity order: an alphabetical tag list
	// opens on "00s", which is not how anyone looks for a genre.
	if (kind != RADIO_BROWSE_LANGUAGES && kind != RADIO_BROWSE_COUNTRIES) {
		return;
	}
	qsort(terms, (size_t)count, sizeof(*terms), term_by_label);
}

// ---------------------------------------------------------------------------
// The worker
// ---------------------------------------------------------------------------

typedef enum {
	JOB_NONE = 0,
	JOB_TOP,
	JOB_TERMS,
	JOB_STATIONS,
	JOB_SEARCH,
} job_kind_t;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static pthread_t thread;
static bool started;
static bool quit;

static job_kind_t pending_kind;
static radio_browse_t pending_browse;
static char pending_value[RADIO_TERM_MAX];
static bool has_pending;
static unsigned pending_request;
static unsigned next_request = 1;

static radio_job_t job_state = RADIO_JOB_IDLE;
static unsigned job_serial;
static char job_error[192];

static radio_term_t result_terms[RADIO_TERM_LIST_MAX];
static int result_term_count;
static radio_station_t result_stations[RADIO_STATION_MAX];
static int result_station_count;
static bool result_terms_shape;
static unsigned result_request;

static void fail_job(unsigned request_id, const char *why) {
	pthread_mutex_lock(&lock);
	snprintf(job_error, sizeof(job_error), "%s", why ? why : "The radio directory did not answer.");
	result_term_count = 0;
	result_station_count = 0;
	result_request = request_id;
	job_state = RADIO_JOB_FAILED;
	job_serial++;
	pthread_mutex_unlock(&lock);
}

static void run_terms_job(unsigned request_id, radio_browse_t kind) {
	const char *path;
	switch (kind) {
	case RADIO_BROWSE_COUNTRIES:
		path = "/json/countries?order=stationcount&reverse=true&limit=" API_TERM_TEXT;
		break;
	case RADIO_BROWSE_LANGUAGES:
		path = "/json/languages?order=stationcount&reverse=true&limit=" API_TERM_TEXT;
		break;
	case RADIO_BROWSE_GENRES:
	default:
		path = "/json/tags?order=stationcount&reverse=true&limit=" API_TERM_TEXT;
		break;
	}

	char *body = api_get(path);
	if (!body) {
		fail_job(request_id, "The radio directory did not answer.");
		return;
	}

	radio_term_t *terms = calloc(RADIO_TERM_LIST_MAX, sizeof(*terms));
	int count = terms ? parse_terms(body, kind, terms, RADIO_TERM_LIST_MAX) : 0;
	free(body);
	if (!terms) {
		fail_job(request_id, "Not enough memory for the station list.");
		return;
	}
	sort_terms(kind, terms, count);

	pthread_mutex_lock(&lock);
	memcpy(result_terms, terms, sizeof(*terms) * (size_t)count);
	result_term_count = count;
	result_station_count = 0;
	result_terms_shape = true;
	result_request = request_id;
	if (count == 0) {
		snprintf(job_error, sizeof(job_error), "The directory returned no entries.");
		job_state = RADIO_JOB_FAILED;
	} else {
		job_state = RADIO_JOB_OK;
	}
	job_serial++;
	pthread_mutex_unlock(&lock);
	free(terms);
}

static void publish_stations(unsigned request_id, char *body, const char *why) {
	if (!body) {
		fail_job(request_id, why);
		return;
	}

	radio_station_t *stations = calloc(RADIO_STATION_MAX, sizeof(*stations));
	int count = stations ? parse_stations(body, stations, RADIO_STATION_MAX) : 0;
	free(body);
	if (!stations) {
		fail_job(request_id, "Not enough memory for the station list.");
		return;
	}

	pthread_mutex_lock(&lock);
	memcpy(result_stations, stations, sizeof(*stations) * (size_t)count);
	result_station_count = count;
	result_term_count = 0;
	result_terms_shape = false;
	result_request = request_id;
	if (count == 0) {
		snprintf(job_error, sizeof(job_error), "No stations were found.");
		job_state = RADIO_JOB_FAILED;
	} else {
		job_state = RADIO_JOB_OK;
	}
	job_serial++;
	pthread_mutex_unlock(&lock);
	free(stations);
}

static void run_top_job(unsigned request_id) {
	char path[256];
	snprintf(path, sizeof(path), "/json/stations/topclick/%d", API_PAGE_LIMIT);
	publish_stations(request_id, api_get(path), "The radio directory did not answer.");
}

static void run_search_job(unsigned request_id, const char *text) {
	char encoded[512];
	http_url_encode(text, encoded, sizeof(encoded));

	char path[1400];
	snprintf(path, sizeof(path),
			 "/json/stations/search?name=%s&order=clickcount&reverse=true&limit=%d",
			 encoded, API_PAGE_LIMIT);
	char *body = api_get(path);
	if (!body) {
		snprintf(path, sizeof(path),
				 "/json/stations/byname/%s?order=clickcount&reverse=true&limit=%d",
				 encoded, API_PAGE_LIMIT);
		body = api_get(path);
	}
	publish_stations(request_id, body, "No stations matched that search.");
}

static void run_stations_job(unsigned request_id, radio_browse_t browse, const char *value) {
	char encoded[512];
	http_url_encode(value, encoded, sizeof(encoded));

	const char *field = "language";
	const char *simple = "bylanguageexact";
	if (browse == RADIO_BROWSE_COUNTRIES) {
		field = "country";
		simple = "bycountryexact";
	} else if (browse == RADIO_BROWSE_GENRES) {
		field = "tag";
		simple = "bytagexact";
	}

	// <field>Exact is a boolean; the value goes in <field>. Without the flag
	// "tag=rock" also drags in "punk rock" and "rockabilly".
	char path[1400];
	snprintf(path, sizeof(path),
			 "/json/stations/search?%s=%s&%sExact=true&order=clickcount&reverse=true&limit=%d",
			 field, encoded, field, API_PAGE_LIMIT);
	char *body = api_get(path);
	if (!body) {
		snprintf(path, sizeof(path),
				 "/json/stations/%s/%s?order=clickcount&reverse=true&limit=%d",
				 simple, encoded, API_PAGE_LIMIT);
		body = api_get(path);
	}
	publish_stations(request_id, body, "The radio directory did not answer.");
}

static void *worker_main(void *unused) {
	(void)unused;
	for (;;) {
		pthread_mutex_lock(&lock);
		while (!quit && !has_pending) {
			pthread_cond_wait(&wake, &lock);
		}
		if (quit) {
			pthread_mutex_unlock(&lock);
			break;
		}
		job_kind_t kind = pending_kind;
		radio_browse_t browse = pending_browse;
		unsigned request_id = pending_request;
		char value[RADIO_TERM_MAX];
		snprintf(value, sizeof(value), "%s", pending_value);
		has_pending = false;
		// A superseded job may have left OK or FAILED behind while this one was
		// queued. BUSY is the truth until this one answers.
		job_state = RADIO_JOB_BUSY;
		pthread_mutex_unlock(&lock);

		if (kind == JOB_TOP) {
			run_top_job(request_id);
		} else if (kind == JOB_TERMS) {
			run_terms_job(request_id, browse);
		} else if (kind == JOB_SEARCH) {
			run_search_job(request_id, value);
		} else {
			run_stations_job(request_id, browse, value);
		}
	}
	return NULL;
}

// ---------------------------------------------------------------------------
// The interface
// ---------------------------------------------------------------------------

bool radio_client_init(void) {
	if (started) {
		return true;
	}
	https_checked = false;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, WORKER_STACK);
	int rc = pthread_create(&thread, &attr, worker_main, NULL);
	pthread_attr_destroy(&attr);
	started = rc == 0;
	return started;
}

static unsigned request(job_kind_t kind, radio_browse_t browse, const char *value) {
	if (!started) {
		return 0;
	}
	pthread_mutex_lock(&lock);
	unsigned request_id = next_request++;
	pending_kind = kind;
	pending_browse = browse;
	pending_request = request_id;
	snprintf(pending_value, sizeof(pending_value), "%s", value ? value : "");
	has_pending = true;
	job_state = RADIO_JOB_BUSY;
	job_error[0] = '\0';
	pthread_cond_signal(&wake);
	pthread_mutex_unlock(&lock);
	return request_id;
}

unsigned radio_client_top(void) { return request(JOB_TOP, RADIO_BROWSE_TOP, NULL); }

unsigned radio_client_terms(radio_browse_t kind) { return request(JOB_TERMS, kind, NULL); }

unsigned radio_client_stations(radio_browse_t kind, const char *term) {
	return request(JOB_STATIONS, kind, term);
}

unsigned radio_client_search(const char *text) {
	return request(JOB_SEARCH, RADIO_BROWSE_LANGUAGES, text);
}

radio_job_t radio_client_state(void) {
	pthread_mutex_lock(&lock);
	radio_job_t state = job_state;
	pthread_mutex_unlock(&lock);
	return state;
}

unsigned radio_client_serial(void) {
	pthread_mutex_lock(&lock);
	unsigned serial = job_serial;
	pthread_mutex_unlock(&lock);
	return serial;
}

const char *radio_client_error(void) {
	// Borrowed until the next request. The GUI reads it right after a
	// completion and copies what it shows, so a stable pointer is enough.
	pthread_mutex_lock(&lock);
	const char *error = job_error;
	pthread_mutex_unlock(&lock);
	return error;
}

bool radio_client_result_is_terms(void) {
	pthread_mutex_lock(&lock);
	bool terms = result_terms_shape;
	pthread_mutex_unlock(&lock);
	return terms;
}

unsigned radio_client_result_request(void) {
	pthread_mutex_lock(&lock);
	unsigned request_id = result_request;
	pthread_mutex_unlock(&lock);
	return request_id;
}

int radio_client_copy_terms(radio_term_t *out, int max) {
	pthread_mutex_lock(&lock);
	int count = job_state == RADIO_JOB_OK ? result_term_count : 0;
	if (count > max) {
		count = max;
	}
	if (out && count > 0) {
		memcpy(out, result_terms, sizeof(*out) * (size_t)count);
	}
	pthread_mutex_unlock(&lock);
	return count;
}

int radio_client_copy_stations(radio_station_t *out, int max) {
	pthread_mutex_lock(&lock);
	int count = job_state == RADIO_JOB_OK ? result_station_count : 0;
	if (count > max) {
		count = max;
	}
	if (out && count > 0) {
		memcpy(out, result_stations, sizeof(*out) * (size_t)count);
	}
	pthread_mutex_unlock(&lock);
	return count;
}

void radio_client_shutdown_begin(void) {
	if (!started) {
		return;
	}
	pthread_mutex_lock(&lock);
	quit = true;
	pthread_cond_broadcast(&wake);
	pthread_mutex_unlock(&lock);
}

void radio_client_shutdown(void) {
	if (!started) {
		return;
	}
	radio_client_shutdown_begin();

	// A request may be blocked in connect() or recv(). Wake it rather than
	// waiting out the network timeout, then join while the code is still
	// mapped.
	http_abort_all();
	pthread_join(thread, NULL);
	started = false;
}
