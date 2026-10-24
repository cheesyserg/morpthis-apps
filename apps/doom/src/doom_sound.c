// Doom's sound effects, mixed in the app and played through the Morpthis DAC.
//
// Doom's WAD stores each effect as unsigned 8-bit mono at its own rate, with a
// short header and silent padding. This module resamples an effect once to the
// output rate, keeps it in a small LRU cache, and mixes up to sixteen active
// channels with Doom's volume and stereo separation. The mixer writes its own
// thread because morpthis_audio_write() blocks at the speed of the DAC.
//
// Music is rendered by src/opl/ and mixed into the same output block: the OPL
// register writes come from Chocolate Doom's OPL player, which this module
// drives from its mixer thread.

#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <morpthis/audio.h>
#include <morpthis/log.h>

#include "deh_str.h"
#include "doom_music.h"
#include "doomtype.h"
#include "i_sound.h"
#include "m_misc.h"
#include "w_wad.h"
#include "z_zone.h"

#define NUM_CHANNELS 16
#define OUTPUT_RATE 44100
#define OUTPUT_CHANNELS 2
#define OUTPUT_BUFFER_MS 46
#define MIX_FRAMES 512
#define CACHE_BYTES (4 * 1024 * 1024)

// i_sound.c and m_config.c still bind these under FEATURE_SOUND. This port
// does its own linear resampling and never uses libsamplerate.
int use_libsamplerate = 0;
float libsamplerate_scale = 0.65f;

typedef struct cached_sound {
	sfxinfo_t *sfxinfo;
	int16_t *samples;
	int frames;
	size_t bytes;
	int users;
	struct cached_sound *prev;
	struct cached_sound *next;
} cached_sound_t;

typedef struct {
	cached_sound_t *sound;
	int position;
	int left_gain;  // 0..256, 256 is full scale
	int right_gain;
	bool active;
} sound_channel_t;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_start_cond = PTHREAD_COND_INITIALIZER;
static pthread_t g_mixer_thread;
static bool g_mixer_started;
static bool g_sound_ready;
static bool g_audio_ready;
static bool g_audio_ok;
static volatile bool g_stop;
static bool g_use_sfx_prefix;

static sound_channel_t g_channels[NUM_CHANNELS];
static cached_sound_t *g_cache_head;
static cached_sound_t *g_cache_tail;
static size_t g_cache_bytes;

// ---------------------------------------------------------------------------
// sound cache
// ---------------------------------------------------------------------------

static void cache_unlink(cached_sound_t *sound) {
	if (sound->prev) {
		sound->prev->next = sound->next;
	} else {
		g_cache_head = sound->next;
	}
	if (sound->next) {
		sound->next->prev = sound->prev;
	} else {
		g_cache_tail = sound->prev;
	}
	sound->prev = NULL;
	sound->next = NULL;
}

static void cache_link_front(cached_sound_t *sound) {
	sound->prev = NULL;
	sound->next = g_cache_head;
	if (g_cache_head) {
		g_cache_head->prev = sound;
	} else {
		g_cache_tail = sound;
	}
	g_cache_head = sound;
}

static void cache_remove(cached_sound_t *sound) {
	cache_unlink(sound);
	if (sound->sfxinfo && sound->sfxinfo->driver_data == sound) {
		sound->sfxinfo->driver_data = NULL;
	}
	if (g_cache_bytes >= sound->bytes) {
		g_cache_bytes -= sound->bytes;
	} else {
		g_cache_bytes = 0;
	}
	free(sound->samples);
	free(sound);
}

// Make room for an incoming sound without ever dropping one a channel is
// using. The oldest idle sound goes first.
static void cache_trim(size_t incoming) {
	while (g_cache_tail && g_cache_bytes + incoming > (size_t)snd_cachesize) {
		cached_sound_t *candidate = g_cache_tail;
		while (candidate && candidate->users > 0) {
			candidate = candidate->prev;
		}
		if (!candidate) {
			break;
		}
		cache_remove(candidate);
	}
}

// ---------------------------------------------------------------------------
// WAD sound lumps
// ---------------------------------------------------------------------------

static void sfx_lump_name(sfxinfo_t *sfx, char *name, size_t size) {
	if (sfx->link) {
		sfx = sfx->link;
	}
	if (g_use_sfx_prefix) {
		M_snprintf(name, size, "ds%s", DEH_String(sfx->name));
	} else {
		M_StringCopy(name, DEH_String(sfx->name), size);
	}
}

static int get_sfx_lump_num(sfxinfo_t *sfx) {
	char name[9];
	sfx_lump_name(sfx, name, sizeof(name));
	return W_GetNumForName(name);
}

static cached_sound_t *load_sound(sfxinfo_t *sfxinfo) {
	int lumpnum = sfxinfo->lumpnum;
	if (lumpnum < 0) {
		lumpnum = get_sfx_lump_num(sfxinfo);
		sfxinfo->lumpnum = lumpnum;
	}

	unsigned int lumplen = W_LumpLength(lumpnum);
	if (lumplen < 8 + 32) {
		return NULL;
	}

	byte *lump = W_CacheLumpNum(lumpnum, PU_STATIC);
	if (!lump) {
		return NULL;
	}

	cached_sound_t *sound = NULL;
	int samplerate = 0;
	uint32_t length = 0;
	if (lump[0] == 0x03 && lump[1] == 0x00) {
		samplerate = lump[2] | (lump[3] << 8);
		length = (uint32_t)lump[4] | ((uint32_t)lump[5] << 8) |
				 ((uint32_t)lump[6] << 16) | ((uint32_t)lump[7] << 24);
	}

	// DMX trims sixteen silent bytes at each end of the sample data.
	if (samplerate < 1000 || samplerate > 192000 || length <= 32 ||
		length > lumplen - 8 || length > INT_MAX) {
		W_ReleaseLumpNum(lumpnum);
		return NULL;
	}

	int input_frames = (int)(length - 32);
	uint64_t output_frames_64 =
		((uint64_t)input_frames * OUTPUT_RATE + (uint32_t)samplerate / 2) /
		(uint32_t)samplerate;
	if (output_frames_64 == 0 || output_frames_64 > INT_MAX) {
		W_ReleaseLumpNum(lumpnum);
		return NULL;
	}
	int output_frames = (int)output_frames_64;

	size_t bytes = (size_t)output_frames * sizeof(int16_t);
	sound = calloc(1, sizeof(*sound));
	int16_t *samples = malloc(bytes);
	if (!sound || !samples) {
		free(sound);
		free(samples);
		W_ReleaseLumpNum(lumpnum);
		return NULL;
	}

	const uint8_t *source = lump + 8 + 16;
	uint64_t step = ((uint64_t)(uint32_t)samplerate << 16) / OUTPUT_RATE;
	uint64_t position = 0;
	for (int i = 0; i < output_frames; i++) {
		int index = (int)(position >> 16);
		if (index >= input_frames) {
			index = input_frames - 1;
		}
		int next = index + 1 < input_frames ? index + 1 : index;
		int first = ((int)source[index] - 128) << 8;
		int second = ((int)source[next] - 128) << 8;
		int fraction = (int)(position & 0xffff);
		samples[i] = (int16_t)(first + (((second - first) * fraction) >> 16));
		position += step;
	}

	W_ReleaseLumpNum(lumpnum);

	sound->sfxinfo = sfxinfo;
	sound->samples = samples;
	sound->frames = output_frames;
	sound->bytes = bytes;

	pthread_mutex_lock(&g_lock);
	cache_trim(bytes);
	cache_link_front(sound);
	g_cache_bytes += bytes;
	sfxinfo->driver_data = sound;
	pthread_mutex_unlock(&g_lock);

	return sound;
}

// ---------------------------------------------------------------------------
// mixing
// ---------------------------------------------------------------------------

static void set_channel_params(sound_channel_t *channel, int volume, int separation) {
	// Doom's separation is 0 (left) .. 254 (right); volume is 0..127.
	channel->left_gain = ((254 - separation) * volume * 256) / (254 * 127);
	channel->right_gain = (separation * volume * 256) / (254 * 127);
}

static void mix_block(int16_t *out) {
	int32_t mix[MIX_FRAMES * 2];
	memset(mix, 0, sizeof(mix));

	for (int c = 0; c < NUM_CHANNELS; c++) {
		sound_channel_t *channel = &g_channels[c];
		if (!channel->active || !channel->sound) {
			continue;
		}

		cached_sound_t *sound = channel->sound;
		int position = channel->position;
		int count = sound->frames - position;
		if (count <= 0) {
			if (sound->users > 0) {
				sound->users--;
			}
			channel->sound = NULL;
			channel->active = false;
			channel->position = 0;
			continue;
		}
		if (count > MIX_FRAMES) {
			count = MIX_FRAMES;
		}

		const int16_t *source = sound->samples + position;
		int left = channel->left_gain;
		int right = channel->right_gain;
		for (int i = 0; i < count; i++) {
			int32_t sample = source[i];
			mix[i * 2] += (sample * left) >> 8;
			mix[i * 2 + 1] += (sample * right) >> 8;
		}

		position += count;
		if (position >= sound->frames) {
			if (sound->users > 0) {
				sound->users--;
			}
			channel->sound = NULL;
			channel->active = false;
			channel->position = 0;
		} else {
			channel->position = position;
		}
	}

	// Music is generated by the OPL emulator on this same mixer thread. A
	// little headroom keeps simultaneous effects from clipping the sum.
	int16_t music[MIX_FRAMES * 2];
	dg_opl_render(music, MIX_FRAMES);
	for (int i = 0; i < MIX_FRAMES * 2; i++) {
		mix[i] += (music[i] * 192) >> 8;
	}

	for (int i = 0; i < MIX_FRAMES * 2; i++) {
		int32_t value = mix[i];
		if (value > 32767) {
			value = 32767;
		} else if (value < -32768) {
			value = -32768;
		}
		out[i] = (int16_t)value;
	}
}

static void *mixer_main(void *unused) {
	(void)unused;

	bool opened = morpthis_audio_open(OUTPUT_RATE, OUTPUT_CHANNELS, OUTPUT_BUFFER_MS);
	pthread_mutex_lock(&g_lock);
	g_audio_ready = true;
	g_audio_ok = opened;
	pthread_cond_broadcast(&g_start_cond);
	pthread_mutex_unlock(&g_lock);
	if (!opened) {
		return NULL;
	}

	int16_t block[MIX_FRAMES * OUTPUT_CHANNELS];
	while (!g_stop) {
		pthread_mutex_lock(&g_lock);
		mix_block(block);
		pthread_mutex_unlock(&g_lock);

		if (morpthis_audio_write(block, MIX_FRAMES) == 0) {
			morpthis_logw("doom", "the audio output stopped");
			break;
		}
	}

	morpthis_audio_close();

	pthread_mutex_lock(&g_lock);
	g_audio_ok = false;
	pthread_mutex_unlock(&g_lock);
	return NULL;
}

// ---------------------------------------------------------------------------
// sound_module_t
// ---------------------------------------------------------------------------

static boolean init_sound(boolean use_sfx_prefix) {
	if (g_sound_ready) {
		return true;
	}

	g_use_sfx_prefix = use_sfx_prefix;
	if (snd_cachesize <= 0 || snd_cachesize > CACHE_BYTES) {
		snd_cachesize = CACHE_BYTES;
	}

	memset(g_channels, 0, sizeof(g_channels));
	g_cache_head = NULL;
	g_cache_tail = NULL;
	g_cache_bytes = 0;
	g_stop = false;
	g_audio_ready = false;
	g_audio_ok = false;

	pthread_mutex_lock(&g_lock);
	if (pthread_create(&g_mixer_thread, NULL, mixer_main, NULL) != 0) {
		pthread_mutex_unlock(&g_lock);
		morpthis_loge("doom", "could not start the sound mixer");
		return false;
	}
	g_mixer_started = true;
	while (!g_audio_ready) {
		pthread_cond_wait(&g_start_cond, &g_lock);
	}
	bool opened = g_audio_ok;
	pthread_mutex_unlock(&g_lock);

	if (!opened) {
		pthread_join(g_mixer_thread, NULL);
		pthread_mutex_lock(&g_lock);
		g_mixer_started = false;
		pthread_mutex_unlock(&g_lock);
		morpthis_logw("doom", "no PCM output; playing without sound");
		return false;
	}

	g_sound_ready = true;
	morpthis_logi("doom", "sound ready at %d Hz, %d channels", OUTPUT_RATE, OUTPUT_CHANNELS);
	return true;
}

static void shutdown_sound(void) {
	if (!g_sound_ready && !g_mixer_started) {
		return;
	}

	g_sound_ready = false;
	g_stop = true;
	if (g_mixer_started) {
		pthread_join(g_mixer_thread, NULL);
		g_mixer_started = false;
	}

	pthread_mutex_lock(&g_lock);
	for (int c = 0; c < NUM_CHANNELS; c++) {
		sound_channel_t *channel = &g_channels[c];
		if (channel->sound && channel->sound->users > 0) {
			channel->sound->users--;
		}
		channel->sound = NULL;
		channel->active = false;
	}
	while (g_cache_head) {
		cache_remove(g_cache_head);
	}
	pthread_mutex_unlock(&g_lock);
}

static void update_sound(void) {
	// The mixer retires finished channels on its own.
}

static void update_sound_params(int channel, int volume, int separation) {
	if (!g_sound_ready || channel < 0 || channel >= NUM_CHANNELS) {
		return;
	}
	pthread_mutex_lock(&g_lock);
	if (g_channels[channel].active) {
		set_channel_params(&g_channels[channel], volume, separation);
	}
	pthread_mutex_unlock(&g_lock);
}

static int start_sound(sfxinfo_t *sfxinfo, int channel, int volume, int separation) {
	if (!g_sound_ready || !sfxinfo || channel < 0 || channel >= NUM_CHANNELS) {
		return -1;
	}

	cached_sound_t *sound = sfxinfo->driver_data;
	if (!sound) {
		sound = load_sound(sfxinfo);
		if (!sound) {
			return -1;
		}
	}

	pthread_mutex_lock(&g_lock);
	sound_channel_t *slot = &g_channels[channel];
	if (slot->sound && slot->sound->users > 0) {
		slot->sound->users--;
	}
	slot->sound = sound;
	slot->position = 0;
	slot->active = true;
	set_channel_params(slot, volume, separation);
	sound->users++;

	// Every use makes a sound the most recent entry in the cache.
	cache_unlink(sound);
	cache_link_front(sound);
	pthread_mutex_unlock(&g_lock);

	return channel;
}

static void stop_sound(int channel) {
	if (!g_sound_ready || channel < 0 || channel >= NUM_CHANNELS) {
		return;
	}
	pthread_mutex_lock(&g_lock);
	sound_channel_t *slot = &g_channels[channel];
	if (slot->sound && slot->sound->users > 0) {
		slot->sound->users--;
	}
	slot->sound = NULL;
	slot->active = false;
	slot->position = 0;
	pthread_mutex_unlock(&g_lock);
}

static boolean sound_is_playing(int channel) {
	if (!g_sound_ready || channel < 0 || channel >= NUM_CHANNELS) {
		return false;
	}
	pthread_mutex_lock(&g_lock);
	boolean playing = g_channels[channel].active;
	pthread_mutex_unlock(&g_lock);
	return playing;
}

static void precache_sounds(sfxinfo_t *sounds, int num_sounds) {
	(void)sounds;
	(void)num_sounds;
	// Loading every effect up front would exceed the cache on a full IWAD.
}

static snddevice_t sound_devices[] = {
	SNDDEVICE_SB,
};

sound_module_t DG_sound_module = {
	sound_devices,
	(int)(sizeof(sound_devices) / sizeof(sound_devices[0])),
	init_sound,
	shutdown_sound,
	get_sfx_lump_num,
	update_sound,
	update_sound_params,
	start_sound,
	stop_sound,
	sound_is_playing,
	precache_sounds,
};
