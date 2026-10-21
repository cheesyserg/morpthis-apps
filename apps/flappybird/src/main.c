#include <morpthis/morpthis.h>

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

MORPTHIS_DEFINE_API_VERSION();

// Flappy Bird as an SDK app.
//
// The game runs in the player's content area, in panel pixels. The artwork is
// the player's own Flappy Bird set, re-scaled before check-in to the size each
// widget draws it at: the SDK's image widgets fit a picture inside their box
// and never enlarge a smaller source.
//
// API 1 has no timer. A thread of the app's own keeps a 16 ms clock and posts
// each tick with morpthis_app_post(), so every widget call still runs on the
// GUI thread, where the SDK requires it.

// ---------------------------------------------------------------------------
// The world
// ---------------------------------------------------------------------------

#define STEP_MS 16
#define MAX_STEPS 4
#define STALL_MS 250

#define GROUND_H 88
#define GROUND_W 336
#define BG_W 288
#define BG_H 160

#define PIPE_COUNT 3
#define PIPE_W 52
#define PIPE_H 320
#define PIPE_GAP 80
#define PIPE_SPACING 157
#define PIPE_MARGIN 16
#define PIPE_MIN_CHANGE 24
#define FIRST_PIPE_AHEAD 120

#define BIRD_BOX 20
#define BIRD_LEFT 62
#define BIRD_W 34
#define BIRD_H 24
#define BIRD_COLOURS 8

#define GRAVITY 0.3f
#define FLAP_VELOCITY (-5.0f)
#define MAX_FALL 8.0f
#define LOST_ABOVE (-22)

// The top of the bird's hit box at rest on the Get Ready screen.
#define READY_TOP 78

#define MEDAL_BRONZE_AT 10
#define MEDAL_SILVER_AT 20
#define MEDAL_GOLD_AT 30

// Drawn sizes. Keep these in step with assets/.
#define LOGO_W 178
#define LOGO_H 48
#define READY_W 184
#define READY_H 50
#define HOWTO_W 57
#define HOWTO_H 49
#define OVER_W 192
#define OVER_H 42
#define BOARD_W 226
#define BOARD_H 114
#define MEDAL_W 44
#define MEDAL_H 44
#define NEW_W 32
#define NEW_H 14

// Score digits. The board's are the small set; the running score is the big
// one. The gaps are the original's, in pixels.
#define DIGITS_MAX 3
#define BIG_W 24
#define BIG_H 36
#define BIG_GAP (-2)
#define SMALL_W 14
#define SMALL_H 20
#define SMALL_GAP 2

#define FLASH_STEPS 12
#define FADE_STEPS 10

#define SOUND_RATE 44100
#define MIX_FRAMES 441
#define SOUND_MAX_BYTES (512 * 1024)

// Where the score board puts its contents, in its own 2x pixels.
#define BOARD_SCORE_Y 34
#define BOARD_BEST_Y 76
#define BOARD_DIGITS_RIGHT 206
#define BOARD_MEDAL_X 24
#define BOARD_MEDAL_Y 42
#define BOARD_NEW_X 132
#define BOARD_NEW_Y 58

typedef enum { ST_TITLE, ST_READY, ST_PLAY, ST_DYING, ST_OVER } state_t;
typedef enum { WINGS_REST, WINGS_IDLE, WINGS_FLAP } wings_t;
typedef enum { SFX_WING, SFX_POINT, SFX_HIT, SFX_DIE, SFX_SWOOSH, SFX_KINDS } sfx_t;

typedef struct {
	int x;
	int gap_top;
	bool scored;
} pipe_t;

typedef struct {
	int16_t *pcm;
	int channels;
	int frames;
	int pos;
} sound_t;

static morpthis_screen_t *screen;
static int width, height, ground_top;
static int board_x, board_y;

static state_t state;
static pipe_t pipes[PIPE_COUNT];
static int bird_c;
static float bird_v;
static int bob_step;
static wings_t wings;
static int wings_ms;
static int wings_i;
static int colour;
static bool night;
static int world_px;
static int bg_px;
static int score, best;
static bool new_best;
static int shown_frame = -1;

static morpthis_widget_t *bg_day[2], *bg_night[2];
static morpthis_widget_t *ground[2];
static morpthis_widget_t *pipe_up[PIPE_COUNT], *pipe_down[PIPE_COUNT];
static morpthis_widget_t *bird[BIRD_COLOURS][3];
static morpthis_widget_t *logo, *get_ready, *how_to, *game_over;
static morpthis_widget_t *board, *new_record;
static morpthis_widget_t *medals[3];
static morpthis_widget_t *score_digits[DIGITS_MAX];
static morpthis_widget_t *over_score_digits[DIGITS_MAX];
static morpthis_widget_t *over_best_digits[DIGITS_MAX];
static int score_shown, over_score_shown, over_best_shown;
static morpthis_widget_t *play_btn, *menu_btn;

static pthread_t tick_thread;
static volatile bool tick_running;
static bool tick_started;
static uint32_t last_ms, step_accum;

static sound_t sounds[SFX_KINDS];
static pthread_mutex_t sound_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t mixer_thread;
static volatile bool mixer_stop;
static bool mixer_started;

static morpthis_widget_t *flash, *fade;
static int flash_t = -1;
static int fade_t = -1;
static void (*fade_action)(void);

static float rot_q, rot_r;

// ---------------------------------------------------------------------------
// Widgets
// ---------------------------------------------------------------------------

static morpthis_widget_t *make_image(const char *path, int w, int h) {
	morpthis_widget_t *widget = morpthis_image_create(screen, path);
	if (!widget) {
		morpthis_loge("flappy", "no widget for %s", path);
		return NULL;
	}
	morpthis_widget_set_size(widget, w, h);
	morpthis_widget_set_visible(widget, false);
	return widget;
}

static morpthis_widget_t *make_button(const char *text, int x, int y, int w, int h,
									  morpthis_click_cb cb) {
	morpthis_widget_t *button = morpthis_button_create(screen, text, cb, NULL);
	if (!button) {
		return NULL;
	}
	morpthis_widget_set_pos(button, x, y);
	morpthis_widget_set_size(button, w, h);
	morpthis_widget_set_visible(button, false);
	return button;
}

// ---------------------------------------------------------------------------
// Sound
//
// The five effects are the original Ogg files decoded to WAV. A thread mixes
// them into the player's raw output; the write blocks at the rate the DAC
// plays, which is what paces the loop. Nothing here touches the UI.
// ---------------------------------------------------------------------------

static const char *const SOUND_FILES[SFX_KINDS] = {
	[SFX_WING] = "assets/sounds/sfx_wing.wav",
	[SFX_POINT] = "assets/sounds/sfx_point.wav",
	[SFX_HIT] = "assets/sounds/sfx_hit.wav",
	[SFX_DIE] = "assets/sounds/sfx_die.wav",
	[SFX_SWOOSH] = "assets/sounds/sfx_swooshing.wav",
};

static uint16_t le16(const uint8_t *p) {
	return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t le32(const uint8_t *p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
			((uint32_t)p[3] << 24);
}

static bool wav_load(sound_t *sound, const char *path) {
	morpthis_file_t *file = morpthis_fs_open(path, MORPTHIS_FS_READ);
	if (!file) {
		return false;
	}

	if (morpthis_fs_seek(file, 0, SEEK_END) != 0) {
		morpthis_fs_close(file);
		return false;
	}
	long size = morpthis_fs_tell(file);
	if (size < 44 || size > SOUND_MAX_BYTES) {
		morpthis_fs_close(file);
		return false;
	}

	uint8_t *data = malloc((size_t)size);
	if (!data) {
		morpthis_fs_close(file);
		return false;
	}
	bool ok = morpthis_fs_seek(file, 0, SEEK_SET) == 0 &&
			  morpthis_fs_read(file, data, (size_t)size) == (size_t)size;
	morpthis_fs_close(file);
	if (!ok) {
		free(data);
		return false;
	}

	if (memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "WAVE", 4) != 0) {
		free(data);
		return false;
	}

	int channels = 0, bits = 0;
	long rate = 0;
	const uint8_t *pcm = NULL;
	size_t bytes = 0;
	size_t off = 12;
	while (off + 8 <= (size_t)size) {
		const uint8_t *id = data + off;
		uint32_t length = le32(data + off + 4);
		const uint8_t *body = data + off + 8;
		if (off + 8 + length > (size_t)size) {
			break;
		}
		if (memcmp(id, "fmt ", 4) == 0 && length >= 16) {
			channels = le16(body + 2);
			rate = (long)le32(body + 4);
			bits = le16(body + 14);
		} else if (memcmp(id, "data", 4) == 0) {
			pcm = body;
			bytes = length;
			break;
		}
		off += 8 + length + (length & 1);
	}

	if (!pcm || bits != 16 || (channels != 1 && channels != 2) || rate != SOUND_RATE) {
		free(data);
		return false;
	}

	size_t frames = bytes / (2 * (size_t)channels);
	int16_t *copy = malloc(frames * (size_t)channels * sizeof(int16_t));
	if (!copy) {
		free(data);
		return false;
	}
	memcpy(copy, pcm, frames * (size_t)channels * sizeof(int16_t));
	free(data);

	// The files carry a quiet tail; stop at the last audible frame so a sound
	// never runs past itself in the mix.
	while (frames > 0) {
		bool quiet = true;
		for (int c = 0; c < channels; c++) {
			if (abs(copy[(frames - 1) * channels + c]) > 64) {
				quiet = false;
			}
		}
		if (!quiet) {
			break;
		}
		frames--;
	}

	sound->pcm = copy;
	sound->channels = channels;
	sound->frames = (int)frames;
	sound->pos = (int)frames;
	return true;
}

static void sound_play(sfx_t sfx) {
	if (sfx < 0 || sfx >= SFX_KINDS) {
		return;
	}
	pthread_mutex_lock(&sound_lock);
	if (sounds[sfx].pcm) {
		sounds[sfx].pos = 0;
	}
	pthread_mutex_unlock(&sound_lock);
}

static void mix_into(int32_t *mix) {
	memset(mix, 0, sizeof(int32_t) * MIX_FRAMES * 2);
	pthread_mutex_lock(&sound_lock);
	for (int i = 0; i < SFX_KINDS; i++) {
		sound_t *s = &sounds[i];
		int n = s->frames - s->pos;
		if (!s->pcm || n <= 0) {
			continue;
		}
		if (n > MIX_FRAMES) {
			n = MIX_FRAMES;
		}
		const int16_t *src = s->pcm + (size_t)s->pos * s->channels;
		for (int f = 0; f < n; f++) {
			int32_t left = src[f * s->channels];
			int32_t right = s->channels == 2 ? src[f * 2 + 1] : left;
			mix[f * 2] += left;
			mix[f * 2 + 1] += right;
		}
		s->pos += n;
	}
	pthread_mutex_unlock(&sound_lock);
}

static void *mixer_main(void *unused) {
	(void)unused;
	int loaded = 0;
	for (int i = 0; i < SFX_KINDS && !mixer_stop; i++) {
		sound_t sound = {0};
		if (wav_load(&sound, SOUND_FILES[i])) {
			pthread_mutex_lock(&sound_lock);
			sounds[i] = sound;
			pthread_mutex_unlock(&sound_lock);
			loaded++;
		} else {
			morpthis_logw("flappy", "cannot load %s", SOUND_FILES[i]);
		}
	}
	morpthis_logi("flappy", "%d of %d sounds loaded", loaded, SFX_KINDS);
	if (loaded == 0 || mixer_stop) {
		return NULL;
	}

	// The output is taken only once the sounds are in memory, so opening it and
	// stopping the player's track is not a silent pause in the middle of a load.
	if (!morpthis_audio_open(SOUND_RATE, 2, 46)) {
		morpthis_loge("flappy", "no PCM; playing silently");
		return NULL;
	}

	int32_t mix[MIX_FRAMES * 2];
	int16_t out[MIX_FRAMES * 2];
	while (!mixer_stop) {
		mix_into(mix);
		for (int i = 0; i < MIX_FRAMES * 2; i++) {
			int32_t value = mix[i];
			out[i] = (int16_t)(value > 32767 ? 32767 : value < -32768 ? -32768 : value);
		}
		if (morpthis_audio_write(out, MIX_FRAMES) == 0) {
			break;
		}
	}

	morpthis_audio_close();
	return NULL;
}

static bool sound_start(void) {
	mixer_stop = false;
	if (pthread_create(&mixer_thread, NULL, mixer_main, NULL) != 0) {
		mixer_stop = true;
		return false;
	}
	mixer_started = true;
	return true;
}

static void sound_stop(void) {
	if (!mixer_started) {
		return;
	}
	mixer_stop = true;
	pthread_join(mixer_thread, NULL);
	mixer_started = false;

	pthread_mutex_lock(&sound_lock);
	for (int i = 0; i < SFX_KINDS; i++) {
		free(sounds[i].pcm);
		memset(&sounds[i], 0, sizeof(sounds[i]));
	}
	pthread_mutex_unlock(&sound_lock);
}

// ---------------------------------------------------------------------------
// Overlays
// ---------------------------------------------------------------------------

static void flash_on(void) {
	flash_t = 0;
	// The overlays are older than the score digits, which are created as the
	// value changes; raising it here keeps the flash over everything.
	morpthis_widget_raise(flash);
	morpthis_widget_set_visible(flash, true);
	morpthis_widget_set_opacity(flash, 86);
}

static void fade_to(void (*action)(void)) {
	fade_t = 0;
	fade_action = action;
	morpthis_widget_raise(fade);
	morpthis_widget_set_visible(fade, true);
	morpthis_widget_set_opacity(fade, 0);
	sound_play(SFX_SWOOSH);
}

static int number_width(int value, bool big) {
	if (value < 0) {
		value = 0;
	}
	if (value > 999) {
		value = 999;
	}
	int count = 1;
	for (int v = value; v >= 10; v /= 10) {
		count++;
	}
	int digit_w = big ? BIG_W : SMALL_W;
	int gap = big ? BIG_GAP : SMALL_GAP;
	return count * digit_w + (count - 1) * gap;
}

// Replaces the digit widgets. The value is right-aligned on right_x; the
// widgets are created on top of whatever is already there.
static void draw_number(morpthis_widget_t **digits, int *shown, int value, int right_x, int y,
						bool big) {
	for (int i = 0; i < DIGITS_MAX; i++) {
		if (digits[i]) {
			morpthis_widget_destroy(digits[i]);
			digits[i] = NULL;
		}
	}
	*shown = 0;

	if (value < 0) {
		value = 0;
	}
	if (value > 999) {
		value = 999;
	}
	int count = 1;
	for (int v = value; v >= 10; v /= 10) {
		count++;
	}

	int digit_w = big ? BIG_W : SMALL_W;
	int digit_h = big ? BIG_H : SMALL_H;
	int gap = big ? BIG_GAP : SMALL_GAP;
	int x = right_x - (count * digit_w + (count - 1) * gap);

	int place = 1;
	for (int i = count - 1; i >= 0; i--) {
		int digit = (value / place) % 10;
		place *= 10;

		char path[48];
		snprintf(path, sizeof(path), big ? "assets/score-game-%d.png" : "assets/score-%d.png",
				 digit);
		morpthis_widget_t *widget = make_image(path, digit_w, digit_h);
		if (!widget) {
			continue;
		}
		morpthis_widget_set_pos(widget, x + i * (digit_w + gap), y);
		morpthis_widget_set_visible(widget, true);
		digits[i] = widget;
	}
	*shown = count;
}

static void draw_score(void) {
	draw_number(score_digits, &score_shown, score, (width + number_width(score, true)) / 2, 10,
				true);
}

static void draw_board_numbers(void) {
	int right = board_x + BOARD_DIGITS_RIGHT;
	draw_number(over_score_digits, &over_score_shown, score, right, board_y + BOARD_SCORE_Y,
				false);
	draw_number(over_best_digits, &over_best_shown, best, right, board_y + BOARD_BEST_Y, false);
}

// ---------------------------------------------------------------------------
// The bird
// ---------------------------------------------------------------------------

static int wing_frame(void) {
	static const int SEQUENCE[4] = {0, 1, 2, 1};
	return wings == WINGS_REST ? 1 : SEQUENCE[wings_i % 4];
}

static void wings_set(wings_t w) {
	wings = w;
	wings_ms = 0;
	wings_i = 0;
}

static void wings_tick(void) {
	if (wings == WINGS_REST) {
		return;
	}
	wings_ms += 15;
	if (wings_ms >= (wings == WINGS_FLAP ? 33 : 100)) {
		wings_ms = 0;
		wings_i++;
		if (wings == WINGS_FLAP && wings_i >= 12) {
			wings = WINGS_REST;
		}
	}
}

static void bird_show(int frame) {
	for (int c = 0; c < BIRD_COLOURS; c++) {
		for (int f = 0; f < 3; f++) {
			morpthis_widget_set_visible(bird[c][f], c == colour && f == frame);
		}
	}
	shown_frame = frame;
}

static void bird_refresh(void) {
	bird_show(wing_frame());
}

static void bird_place(void) {
	int x = BIRD_LEFT - (BIRD_W - BIRD_BOX) / 2;
	int y = bird_c - (BIRD_H - BIRD_BOX) / 2;
	for (int f = 0; f < 3; f++) {
		morpthis_widget_set_pos(bird[colour][f], x, y);
		morpthis_widget_set_rotation(bird[colour][f], (int)rot_q);
	}
}

static int bird_floor(void) {
	return ground_top - BIRD_BOX;
}

static bool bird_on_ground(void) {
	return bird_c >= bird_floor();
}

static void bird_reset(void) {
	bird_c = READY_TOP;
	bird_v = 0;
	bob_step = 0;
	rot_q = 0;
	rot_r = 0;
	wings_set(WINGS_IDLE);
}

// A triangle standing in for the original sine: four pixels each way, one
// cycle a little over a second.
static int bob(void) {
	int phase = bob_step % 64;
	int triangle = phase < 32 ? phase : 64 - phase;
	return (triangle - 16) / 4;
}

static void flap(void) {
	if (bird_c >= 0) {
		bird_v = FLAP_VELOCITY;
		rot_r = -10.0f;
		wings_set(WINGS_FLAP);
		sound_play(SFX_WING);
	}
}

static void bird_fall(void) {
	bird_v += GRAVITY;
	if (bird_v > MAX_FALL) {
		bird_v = MAX_FALL;
	}
	bird_c = (int)((float)bird_c + bird_v);
	if (bird_c > bird_floor()) {
		bird_c = bird_floor();
		bird_v = 0;
	}

	// A flap sets the turn rate nose-up; it grows a step at a time, so the
	// bird levels out and then points at the ground as it falls.
	rot_q += rot_r;
	rot_r += 0.4f;
	if (rot_q < -20.0f) {
		rot_q = -20.0f;
	}
	if (rot_q > 90.0f) {
		rot_q = 90.0f;
	}
}

// ---------------------------------------------------------------------------
// Pipes
// ---------------------------------------------------------------------------

static int random_gap_top(int previous) {
	int lo = PIPE_MARGIN;
	int hi = ground_top - PIPE_MARGIN - PIPE_GAP;
	if (hi <= lo) {
		return lo;
	}
	int top = lo;
	for (int tries = 0; tries < 8; tries++) {
		top = lo + rand() % (hi - lo + 1);
		if (previous < 0 || abs(top - previous) >= PIPE_MIN_CHANGE) {
			break;
		}
	}
	return top;
}

static void pipes_reset(void) {
	for (int i = 0; i < PIPE_COUNT; i++) {
		pipes[i].x = width + FIRST_PIPE_AHEAD + i * PIPE_SPACING;
		pipes[i].gap_top = random_gap_top(i > 0 ? pipes[i - 1].gap_top : -1);
		pipes[i].scored = false;
	}
}

static void pipes_place(void) {
	for (int i = 0; i < PIPE_COUNT; i++) {
		morpthis_widget_set_pos(pipe_up[i], pipes[i].x, pipes[i].gap_top - PIPE_H);
		morpthis_widget_set_pos(pipe_down[i], pipes[i].x, pipes[i].gap_top + PIPE_GAP);
	}
}

static void pipes_advance(void) {
	for (int i = 0; i < PIPE_COUNT; i++) {
		pipes[i].x -= 2;
	}
	for (int i = 0; i < PIPE_COUNT; i++) {
		pipe_t *p = &pipes[i];
		if (p->x + PIPE_W < 0) {
			int last = 0;
			for (int k = 1; k < PIPE_COUNT; k++) {
				if (pipes[k].x > pipes[last].x) {
					last = k;
				}
			}
			p->x = pipes[last].x + PIPE_SPACING;
			p->gap_top = random_gap_top(pipes[last].gap_top);
			p->scored = false;
		}
		if (!p->scored && p->x <= BIRD_LEFT) {
			p->scored = true;
			score++;
			sound_play(SFX_POINT);
			draw_score();
		}
	}
	pipes_place();
}

// Edges touching count, as in the original.
static bool boxes_touch(int x, int y, int w, int h, int px, int py, int pw, int ph) {
	return x + w >= px && x <= px + pw && y + h >= py && y <= py + ph;
}

static bool bird_hits_pipe(void) {
	for (int i = 0; i < PIPE_COUNT; i++) {
		const pipe_t *p = &pipes[i];
		if (boxes_touch(BIRD_LEFT, bird_c, BIRD_BOX, BIRD_BOX, p->x, p->gap_top - PIPE_H, PIPE_W,
						PIPE_H) ||
			boxes_touch(BIRD_LEFT, bird_c, BIRD_BOX, BIRD_BOX, p->x, p->gap_top + PIPE_GAP,
						PIPE_W, PIPE_H)) {
			return true;
		}
	}
	return false;
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

// The background is 288 px wide and scrolls one pixel a step; the ground is
// 336 and scrolls two. Two copies of each cover any offset.
static void scroll_place(void) {
	int bx = -(bg_px % BG_W);
	int gx = -(world_px % GROUND_W);
	morpthis_widget_set_pos(bg_day[0], bx, 0);
	morpthis_widget_set_pos(bg_day[1], bx + BG_W, 0);
	morpthis_widget_set_pos(bg_night[0], bx, 0);
	morpthis_widget_set_pos(bg_night[1], bx + BG_W, 0);
	morpthis_widget_set_pos(ground[0], gx, ground_top);
	morpthis_widget_set_pos(ground[1], gx + GROUND_W, ground_top);
}

static int medal_for(int value) {
	if (value >= MEDAL_GOLD_AT) {
		return 2;
	}
	if (value >= MEDAL_SILVER_AT) {
		return 1;
	}
	if (value >= MEDAL_BRONZE_AT) {
		return 0;
	}
	return -1;
}

static void place_buttons(void) {
	if (state == ST_TITLE) {
		morpthis_widget_set_pos(play_btn, (width - 120) / 2, 104);
		morpthis_widget_set_size(play_btn, 120, 40);
	} else if (state == ST_OVER) {
		morpthis_widget_set_pos(play_btn, 8, 168);
		morpthis_widget_set_size(play_btn, 110, 36);
		morpthis_widget_set_pos(menu_btn, width - 118, 168);
		morpthis_widget_set_size(menu_btn, 110, 36);
	}
}

static void apply_visibility(void) {
	const bool title = state == ST_TITLE;
	const bool ready = state == ST_READY;
	const bool playing = state == ST_PLAY || state == ST_DYING;
	const bool over = state == ST_OVER;

	for (int i = 0; i < 2; i++) {
		morpthis_widget_set_visible(bg_day[i], !night);
		morpthis_widget_set_visible(bg_night[i], night);
		morpthis_widget_set_visible(ground[i], true);
	}

	for (int i = 0; i < PIPE_COUNT; i++) {
		morpthis_widget_set_visible(pipe_up[i], playing || over);
		morpthis_widget_set_visible(pipe_down[i], playing || over);
	}

	morpthis_widget_set_visible(logo, title);
	morpthis_widget_set_visible(get_ready, ready);
	morpthis_widget_set_visible(how_to, ready);

	place_buttons();
	morpthis_widget_set_visible(play_btn, title || over);
	morpthis_widget_set_visible(menu_btn, over);

	morpthis_widget_set_visible(game_over, over);
	morpthis_widget_set_visible(board, over);
	morpthis_widget_set_visible(new_record, over && new_best);
	int medal = over ? medal_for(score) : -1;
	for (int i = 0; i < 3; i++) {
		morpthis_widget_set_visible(medals[i], i == medal);
	}

	for (int i = 0; i < DIGITS_MAX; i++) {
		morpthis_widget_set_visible(score_digits[i], playing && i < score_shown);
		morpthis_widget_set_visible(over_score_digits[i], over && i < over_score_shown);
		morpthis_widget_set_visible(over_best_digits[i], over && i < over_best_shown);
	}

	if (ready || playing || over) {
		bird_refresh();
	} else {
		for (int c = 0; c < BIRD_COLOURS; c++) {
			for (int f = 0; f < 3; f++) {
				morpthis_widget_set_visible(bird[c][f], false);
			}
		}
		shown_frame = -1;
	}
}

static void pick_look(void) {
	static const int WEIGHTS[BIRD_COLOURS] = {10, 10, 10, 10, 10, 10, 3, 1};
	int total = 0;
	for (int i = 0; i < BIRD_COLOURS; i++) {
		total += WEIGHTS[i];
	}
	int pick = rand() % total;
	colour = 0;
	while (colour < BIRD_COLOURS - 1 && pick >= WEIGHTS[colour]) {
		pick -= WEIGHTS[colour];
		colour++;
	}
	night = (rand() & 1) != 0;
}

static int best_load(void) {
	char text[32];
	size_t got = 0;
	if (morpthis_fs_read_all("best.txt", text, sizeof(text), &got) <= 0) {
		return 0;
	}
	int value = atoi(text);
	if (value < 0) {
		value = 0;
	}
	if (value > 9999) {
		value = 9999;
	}
	return value;
}

static void best_save(int value) {
	char text[16];
	int length = snprintf(text, sizeof(text), "%d\n", value);
	if (length > 0) {
		morpthis_fs_write_all("best.txt", text, (size_t)length);
	}
}

static void enter_over(void) {
	state = ST_OVER;
	draw_board_numbers();
	apply_visibility();

	// The digits were created after the overlays; put the overlay back on top
	// before its remaining steps are drawn.
	if (flash_t >= 0) {
		morpthis_widget_raise(flash);
	}
	if (fade_t >= 0) {
		morpthis_widget_raise(fade);
	}

	if (new_best) {
		morpthis_toast("New best!");
	}
}

static void hit(void) {
	flash_on();
	sound_play(SFX_HIT);

	if (score > best) {
		best = score;
		new_best = true;
		best_save(best);
	}
	if (bird_on_ground()) {
		enter_over();
	} else {
		state = ST_DYING;
		sound_play(SFX_DIE);
	}
}

static void go_title(void) {
	state = ST_TITLE;
	pick_look();
	bird_reset();
	apply_visibility();
}

static void start_ready(bool new_look) {
	if (new_look) {
		pick_look();
	}
	state = ST_READY;
	score = 0;
	new_best = false;
	bird_reset();
	pipes_reset();
	pipes_place();
	draw_score();
	apply_visibility();

	// draw_score() created widgets after the fade overlay was raised.
	if (fade_t >= 0) {
		morpthis_widget_raise(fade);
	}
}

static void start_play(void) {
	state = ST_PLAY;
	flap();
	apply_visibility();
}

// Fade actions take no arguments; these are the callbacks the overlay runs
// halfway through.
static void fade_start_title(void) {
	start_ready(false);
}

static void fade_start_new(void) {
	start_ready(true);
}

static void fade_go_title(void) {
	go_title();
}

static void step(void) {
	const bool moving = state == ST_TITLE || state == ST_READY || state == ST_PLAY;
	if (moving) {
		world_px = (world_px + 2) % GROUND_W;
		bg_px = (bg_px + 1) % BG_W;
	}
	if (state == ST_TITLE || state == ST_READY) {
		bob_step = (bob_step + 1) % 64;
	}
	if (state != ST_OVER) {
		wings_tick();
	}

	switch (state) {
	case ST_PLAY:
		pipes_advance();
		bird_fall();
		if (bird_on_ground() || bird_hits_pipe() || bird_c < LOST_ABOVE) {
			hit();
		}
		break;
	case ST_DYING:
		bird_fall();
		if (bird_on_ground()) {
			enter_over();
		}
		break;
	default:
		break;
	}

	if (state == ST_READY) {
		bird_c = READY_TOP + bob();
	}

	if (flash_t >= 0) {
		flash_t++;
		if (flash_t > FLASH_STEPS) {
			flash_t = -1;
			morpthis_widget_set_visible(flash, false);
		}
	}

	if (fade_t >= 0) {
		fade_t++;
		if (fade_t == FADE_STEPS && fade_action) {
			fade_action();
			fade_action = NULL;
		}

		int opacity;
		if (fade_t <= FADE_STEPS) {
			opacity = fade_t * 100 / FADE_STEPS;
		} else {
			opacity = (2 * FADE_STEPS - fade_t) * 100 / FADE_STEPS;
		}
		if (opacity < 0) {
			opacity = 0;
		}
		if (opacity > 100) {
			opacity = 100;
		}
		morpthis_widget_set_opacity(fade, opacity);

		if (fade_t >= 2 * FADE_STEPS) {
			fade_t = -1;
			morpthis_widget_set_visible(fade, false);
		}
	}
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

static void on_tap(void) {
	if (fade_t >= 0) {
		return;
	}

	switch (state) {
	case ST_TITLE:
		fade_to(fade_start_title);
		break;
	case ST_READY:
		start_play();
		break;
	case ST_PLAY:
		flap();
		break;
	case ST_OVER:
		fade_to(fade_start_new);
		break;
	default:
		break;
	}
}

static void play_clicked(morpthis_widget_t *widget, void *user) {
	(void)widget;
	(void)user;
	if (fade_t >= 0) {
		return;
	}
	if (state == ST_TITLE) {
		fade_to(fade_start_title);
	} else if (state == ST_OVER) {
		fade_to(fade_start_new);
	}
}

static void menu_clicked(morpthis_widget_t *widget, void *user) {
	(void)widget;
	(void)user;
	if (fade_t >= 0) {
		return;
	}
	fade_to(fade_go_title);
}

// ---------------------------------------------------------------------------
// The clock
// ---------------------------------------------------------------------------

static void tick_stop(void) {
	if (!tick_started) {
		return;
	}
	tick_running = false;
	pthread_join(tick_thread, NULL);
	tick_started = false;
}

static void tick_cb(void *unused) {
	(void)unused;

	uint32_t now = morpthis_app_time_ms();
	uint32_t elapsed = now - last_ms;
	last_ms = now;

	if (elapsed > STALL_MS) {
		// The panel was dark or the GUI was busy elsewhere. A game in
		// progress waits rather than making the jump up in one frame.
		step_accum = 0;
		return;
	}

	step_accum += elapsed;
	int steps = 0;
	while (step_accum >= STEP_MS && steps < MAX_STEPS) {
		step();
		step_accum -= STEP_MS;
		steps++;
	}
	if (step_accum >= STEP_MS) {
		step_accum = 0;
	}
	if (steps == 0) {
		return;
	}

	if (state == ST_TITLE || state == ST_READY || state == ST_PLAY) {
		scroll_place();
	}
	if (state != ST_TITLE) {
		bird_place();
	}
	if (state != ST_OVER && wing_frame() != shown_frame) {
		bird_refresh();
	}
}

static void *tick_main(void *unused) {
	(void)unused;
	const struct timespec delay = {.tv_sec = 0, .tv_nsec = 16 * 1000 * 1000L};
	while (tick_running) {
		(void)nanosleep(&delay, NULL);
		if (!tick_running) {
			break;
		}
		morpthis_app_post(tick_cb, NULL);
	}
	return NULL;
}

static bool tick_start(void) {
	last_ms = morpthis_app_time_ms();
	step_accum = 0;
	tick_running = true;
	if (pthread_create(&tick_thread, NULL, tick_main, NULL) != 0) {
		tick_running = false;
		return false;
	}
	tick_started = true;
	return true;
}

static void button_cb(morpthis_button_t button, morpthis_input_action_t action, void *user) {
	(void)user;
	if (action != MORPTHIS_INPUT_PRESS && action != MORPTHIS_INPUT_REPEAT) {
		return;
	}
	if (button == MORPTHIS_BUTTON_BACK) {
		// The player quits the app right after delivering BACK. Both threads must
		// be gone before the shared object is unloaded. Quitting here too keeps
		// the host runtime, which only delivers the button, in step with it.
		sound_stop();
		tick_stop();
		morpthis_app_quit();
	} else if (button == MORPTHIS_BUTTON_PLAY_PAUSE) {
		on_tap();
	}
}

static void touch_cb(morpthis_touch_action_t action, morpthis_point_t point, void *user) {
	(void)point;
	(void)user;
	if (action == MORPTHIS_TOUCH_DOWN) {
		on_tap();
	}
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

static bool build(void) {
	width = morpthis_ui_width();
	height = morpthis_ui_height();
	if (width < 120 || height < 160) {
		return false;
	}
	ground_top = height - GROUND_H;

	screen = morpthis_screen_create("Flappy Bird");
	if (!screen) {
		return false;
	}

	for (int i = 0; i < 2; i++) {
		bg_day[i] = make_image("assets/bg-day.png", BG_W, BG_H);
		bg_night[i] = make_image("assets/bg-night.png", BG_W, BG_H);
	}
	for (int i = 0; i < 2; i++) {
		ground[i] = make_image("assets/ground.png", GROUND_W, GROUND_H);
		morpthis_widget_set_pos(ground[i], 0, ground_top);
	}

	for (int i = 0; i < PIPE_COUNT; i++) {
		pipe_up[i] = make_image("assets/pipe-up.png", PIPE_W, PIPE_H);
		pipe_down[i] = make_image("assets/pipe-down.png", PIPE_W, PIPE_H);
	}

	static const char *const COLOURS[BIRD_COLOURS] = {"yellow", "red",  "blue", "green",
													  "peach",  "purple", "cow",  "rare"};
	char path[64];
	for (int c = 0; c < BIRD_COLOURS; c++) {
		for (int f = 0; f < 3; f++) {
			snprintf(path, sizeof(path), "assets/bird-%s-0%d.png", COLOURS[c], f + 1);
			bird[c][f] = make_image(path, BIRD_W, BIRD_H);
		}
	}

	logo = make_image("assets/logo.png", LOGO_W, LOGO_H);
	morpthis_widget_set_pos(logo, (width - LOGO_W) / 2, 16);

	get_ready = make_image("assets/get-ready.png", READY_W, READY_H);
	morpthis_widget_set_pos(get_ready, (width - READY_W) / 2, 14);

	how_to = make_image("assets/how-to.png", HOWTO_W, HOWTO_H);
	morpthis_widget_set_pos(how_to, (width - HOWTO_W) / 2, ground_top - HOWTO_H - 8);

	game_over = make_image("assets/game-over.png", OVER_W, OVER_H);
	morpthis_widget_set_pos(game_over, (width - OVER_W) / 2, 2);

	board_x = (width - BOARD_W) / 2;
	board_y = 46;
	board = make_image("assets/score-board.png", BOARD_W, BOARD_H);
	morpthis_widget_set_pos(board, board_x, board_y);

	new_record = make_image("assets/new-record-label.png", NEW_W, NEW_H);
	morpthis_widget_set_pos(new_record, board_x + BOARD_NEW_X, board_y + BOARD_NEW_Y);

	medals[0] = make_image("assets/medal-bronze.png", MEDAL_W, MEDAL_H);
	medals[1] = make_image("assets/medal-silver.png", MEDAL_W, MEDAL_H);
	medals[2] = make_image("assets/medal-gold.png", MEDAL_W, MEDAL_H);
	for (int i = 0; i < 3; i++) {
		morpthis_widget_set_pos(medals[i], board_x + BOARD_MEDAL_X, board_y + BOARD_MEDAL_Y);
	}

	play_btn = make_button("Play", (width - 120) / 2, 104, 120, 40, play_clicked);
	menu_btn = make_button("Menu", width - 118, 168, 110, 36, menu_clicked);

	// Last, so they start above the rest; flash_on() and fade_to() raise them
	// again after their value has changed the score digits' order.
	flash = make_image("assets/white.png", width, height);
	morpthis_widget_set_pos(flash, 0, 0);
	fade = make_image("assets/black.png", width, height);
	morpthis_widget_set_pos(fade, 0, 0);

	best = best_load();
	scroll_place();
	return true;
}

int morpthis_app_main(const morpthis_app_t *app) {
	(void)app;
	morpthis_logi("flappy", "starting");

	srand((unsigned)time(NULL) ^ (unsigned)morpthis_app_time_ms());

	if (!build()) {
		morpthis_loge("flappy", "could not build the screen");
		morpthis_toast("Flappy Bird could not start");
		return 1;
	}

	morpthis_input_set_button_cb(button_cb, NULL);
	morpthis_input_set_touch_cb(touch_cb, NULL);

	go_title();
	morpthis_screen_load(screen);

	if (!tick_start()) {
		morpthis_loge("flappy", "could not start the game clock");
		morpthis_toast("Flappy Bird could not start");
		return 1;
	}

	if (!sound_start()) {
		morpthis_loge("flappy", "could not start the mixer; playing silently");
	}

	return 0;
}
