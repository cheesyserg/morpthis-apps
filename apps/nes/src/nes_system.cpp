// InfoNES's system layer for the Morpthis SDK.
//
// The emulator runs on its own thread. Frames are handed to nes.c, which
// rotates them into the player's canvas and notifies the GUI thread; audio is
// written straight to the player's PCM output on this thread, so a blocking
// write paces the emulator.

#include "InfoNES.h"
#include "InfoNES_System.h"
#include "InfoNES_pAPU.h"

#include "nes.h"

#include <morpthis/morpthis.h>

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// ---------------------------------------------------------------------------
// constants
// ---------------------------------------------------------------------------

#define AUDIO_RATE 44100
#define AUDIO_CHANNELS 2
#define AUDIO_BUFFER_MS 40
#define FRAME_US 16639 // 60.0988 Hz, the NES frame rate
#define SAVE_EVERY_FRAMES 3600 // a battery save about once a minute

// ---------------------------------------------------------------------------
// palette
// ---------------------------------------------------------------------------

WORD NesPalette[64] = {
	0x39ce, 0x1071, 0x0015, 0x2013, 0x440e, 0x5402, 0x5000, 0x3c20,
	0x20a0, 0x0100, 0x0140, 0x00e2, 0x0ceb, 0x0000, 0x0000, 0x0000,
	0x5ef7, 0x01dd, 0x10fd, 0x401e, 0x5c17, 0x700b, 0x6ca0, 0x6521,
	0x45c0, 0x0240, 0x02a0, 0x0247, 0x0211, 0x0000, 0x0000, 0x0000,
	0x7fff, 0x1eff, 0x2e5f, 0x223f, 0x79ff, 0x7dd6, 0x7dcc, 0x7e67,
	0x7ae7, 0x4342, 0x2769, 0x2ff3, 0x03bb, 0x0000, 0x0000, 0x0000,
	0x7fff, 0x579f, 0x635f, 0x6b3f, 0x7f1f, 0x7f1b, 0x7ef6, 0x7f75,
	0x7f94, 0x73f4, 0x57d7, 0x5bf9, 0x4ffe, 0x0000, 0x0000, 0x0000,
};

// ---------------------------------------------------------------------------
// file state
// ---------------------------------------------------------------------------

static int nSRAM_SaveFlag;

static int LoadSRAM(void);
static int SaveSRAM(void);

// ---------------------------------------------------------------------------
// clock
// ---------------------------------------------------------------------------

static uint64_t now_us(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static void sleep_us(uint64_t us) {
	struct timespec ts;
	ts.tv_sec = (time_t)(us / 1000000u);
	ts.tv_nsec = (long)((us % 1000000u) * 1000u);
	while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
	}
}

// ---------------------------------------------------------------------------
// audio
// ---------------------------------------------------------------------------

static bool s_audio_open;
static bool s_audio_playing;
static uint64_t s_audio_retry_at;
static int s_audio_rate = AUDIO_RATE;

static void audio_close(void) {
	if (s_audio_open) {
		if (s_audio_playing) {
			morpthis_audio_set_playing(false);
		}
		morpthis_audio_close();
		s_audio_open = false;
	}
	s_audio_playing = false;
	nes_backend_set_audio_pacing(false);
}

static bool audio_open(void) {
	if (s_audio_open) {
		return true;
	}
	if (!nes_backend_sound_wanted() || now_us() < s_audio_retry_at) {
		return false;
	}
	if (!morpthis_audio_open(s_audio_rate, AUDIO_CHANNELS, AUDIO_BUFFER_MS)) {
		morpthis_logw("nes", "the DAC is busy; running without sound");
		s_audio_retry_at = now_us() + 3000000u;
		return false;
	}
	s_audio_open = true;
	nes_backend_set_audio_pacing(true);
	if (!s_audio_playing) {
		morpthis_audio_set_playing(true);
		s_audio_playing = true;
	}
	return true;
}

void InfoNES_SoundInit(void) {
}

int InfoNES_SoundOpen(int samples_per_sync, int sample_rate) {
	(void)samples_per_sync;
	if (sample_rate > 0) {
		s_audio_rate = sample_rate;
	}
	if (nes_backend_sound_wanted()) {
		audio_open();
	}
	return 1;
}

void InfoNES_SoundClose(void) {
	audio_close();
}

void InfoNES_SoundOutput(int samples, BYTE *wave1, BYTE *wave2, BYTE *wave3,
						 BYTE *wave4, BYTE *wave5) {
	if (!nes_backend_sound_wanted()) {
		audio_close();
		return;
	}

	if (nes_backend_paused() || nes_backend_turbo()) {
		if (s_audio_playing) {
			morpthis_audio_set_playing(false);
			s_audio_playing = false;
		}
		return;
	}

	if (!s_audio_open && !audio_open()) {
		return;
	}

	// The pAPU averages five channels whose levels are not centred on 128.
	// Remove the DC with a one-pole high-pass and give the result gain; the
	// exact level is a matter of taste and of the player's volume.
	static int32_t dc_in;
	static int32_t dc_out;
	static int16_t pcm[735 * AUDIO_CHANNELS];
	if (samples > 735) {
		samples = 735;
	}
	for (int i = 0; i < samples; i++) {
		int mixed = (wave1[i] + wave2[i] + wave3[i] + wave4[i] + wave5[i]) / 5;
		int32_t filtered = (int32_t)mixed - dc_in + (dc_out * 255) / 256;
		dc_in = mixed;
		dc_out = filtered;
		int value = filtered * 96;
		if (value < -32768) {
			value = -32768;
		} else if (value > 32767) {
			value = 32767;
		}
		pcm[i * 2] = (int16_t)value;
		pcm[i * 2 + 1] = (int16_t)value;
	}

	if (!s_audio_playing) {
		morpthis_audio_set_playing(true);
		s_audio_playing = true;
	}
	if (morpthis_audio_write(pcm, (size_t)samples) == 0) {
		morpthis_logw("nes", "the DAC stopped accepting samples");
		audio_close();
		s_audio_retry_at = now_us() + 3000000u;
	}
}

// ---------------------------------------------------------------------------
// ROM
// ---------------------------------------------------------------------------

int InfoNES_ReadRom(const char *pszFileName) {
	InfoNES_ReleaseRom();

	FILE *fp = fopen(pszFileName, "rb");
	if (!fp) {
		morpthis_loge("nes", "open %s: %s", pszFileName, strerror(errno));
		return -1;
	}

	if (fread(&NesHeader, 1, 16, fp) != 16 || memcmp(NesHeader.byID, "NES\x1a", 4) != 0) {
		fclose(fp);
		return -1;
	}
	if (NesHeader.byRomSize == 0) {
		fclose(fp);
		return -1;
	}

	size_t prg_size = (size_t)NesHeader.byRomSize * 0x4000u;
	size_t chr_size = (size_t)NesHeader.byVRomSize * 0x2000u;
	if (prg_size > 8u * 1024u * 1024u || chr_size > 8u * 1024u * 1024u) {
		fclose(fp);
		return -1;
	}

	long start = ftell(fp);
	if (start < 0 || fseek(fp, 0, SEEK_END) != 0) {
		fclose(fp);
		return -1;
	}
	long end = ftell(fp);
	if (end < 0 || fseek(fp, start, SEEK_SET) != 0) {
		fclose(fp);
		return -1;
	}

	size_t need = prg_size + chr_size + ((NesHeader.byInfo1 & 4) ? 512u : 0u);
	if ((size_t)(end - start) < need) {
		fclose(fp);
		return -1;
	}

	memset(SRAM, 0, SRAM_SIZE);

	// Trainer, when present, lives at 0x7000 in CPU space, which InfoNES keeps
	// at SRAM[0x1000].
	if (NesHeader.byInfo1 & 4) {
		if (fread(&SRAM[0x1000], 1, 512, fp) != 512) {
			fclose(fp);
			return -1;
		}
	}

	ROM = (BYTE *)malloc(prg_size);
	if (!ROM) {
		fclose(fp);
		return -1;
	}
	if (fread(ROM, 1, prg_size, fp) != prg_size) {
		free(ROM);
		ROM = NULL;
		fclose(fp);
		return -1;
	}

	if (chr_size > 0) {
		VROM = (BYTE *)malloc(chr_size);
		if (!VROM) {
			free(ROM);
			ROM = NULL;
			fclose(fp);
			return -1;
		}
		if (fread(VROM, 1, chr_size, fp) != chr_size) {
			free(VROM);
			free(ROM);
			VROM = NULL;
			ROM = NULL;
			fclose(fp);
			return -1;
		}
	}

	fclose(fp);
	morpthis_logi("nes", "loaded %s: mapper %u, %uK PRG, %uK CHR",
				  pszFileName, (unsigned)((NesHeader.byInfo1 >> 4) | (NesHeader.byInfo2 & 0xF0)),
				  (unsigned)(prg_size / 1024), (unsigned)(chr_size / 1024));
	return 0;
}

void InfoNES_ReleaseRom(void) {
	if (ROM) {
		free(ROM);
		ROM = NULL;
	}
	if (VROM) {
		free(VROM);
		VROM = NULL;
	}
}

// ---------------------------------------------------------------------------
// battery save
// ---------------------------------------------------------------------------

static int LoadSRAM(void) {
	nSRAM_SaveFlag = 0;
	if (!ROM_SRAM) {
		return 0;
	}
	nSRAM_SaveFlag = 1;

	const char *path = nes_save_path();
	if (!path || !path[0]) {
		return -1;
	}

	FILE *fp = fopen(path, "rb");
	if (!fp) {
		return -1;
	}

	unsigned char src[SRAM_SIZE];
	size_t got = fread(src, 1, sizeof src, fp);
	fclose(fp);
	if (got < SRAM_SIZE) {
		return -1;
	}

	int decoded = 0;
	int written = 0;
	unsigned char tag = src[decoded++];
	while (written < SRAM_SIZE && decoded < SRAM_SIZE) {
		unsigned char data = src[decoded++];
		if (data == tag) {
			if (decoded + 1 >= SRAM_SIZE) {
				break;
			}
			data = src[decoded++];
			int run = src[decoded++];
			for (int i = 0; i <= run && written < SRAM_SIZE; i++) {
				SRAM[written++] = data;
			}
		} else {
			SRAM[written++] = data;
		}
	}
	return 0;
}

static int SaveSRAM(void) {
	if (!nSRAM_SaveFlag) {
		return 0;
	}

	const char *path = nes_save_path();
	if (!path || !path[0]) {
		return -1;
	}

	int used[256];
	unsigned char dst[SRAM_SIZE];
	memset(used, 0, sizeof used);
	for (int i = 0; i < SRAM_SIZE; i++) {
		used[SRAM[i]]++;
	}
	unsigned char tag = 0;
	for (int i = 1; i < 256; i++) {
		if (used[i] < used[tag]) {
			tag = (unsigned char)i;
		}
	}

	int encoded = 0;
	int out = 0;
	int run = 1;
	dst[out++] = tag;

	unsigned char previous = SRAM[encoded++];
	while (encoded < SRAM_SIZE && out < SRAM_SIZE - 133) {
		unsigned char data = SRAM[encoded++];
		if (previous == data && run < 256) {
			run++;
		} else {
			if (run >= 4 || previous == tag) {
				dst[out++] = tag;
				dst[out++] = previous;
				dst[out++] = (unsigned char)(run - 1);
			} else {
				for (int i = 0; i < run; i++) {
					dst[out++] = previous;
				}
			}
			previous = data;
			run = 1;
		}
	}
	if (run >= 4 || previous == tag) {
		dst[out++] = tag;
		dst[out++] = previous;
		dst[out++] = (unsigned char)(run - 1);
	} else {
		for (int i = 0; i < run; i++) {
			dst[out++] = previous;
		}
	}

	char temp[600];
	if (snprintf(temp, sizeof temp, "%s.tmp", path) >= (int)sizeof temp) {
		return -1;
	}

	FILE *fp = fopen(temp, "wb");
	if (!fp) {
		morpthis_logw("nes", "save %s: %s", temp, strerror(errno));
		return -1;
	}
	size_t wrote = fwrite(dst, 1, (size_t)out, fp);
	if (fclose(fp) != 0 || wrote != (size_t)out) {
		morpthis_logw("nes", "short save write for %s", temp);
		morpthis_fs_remove_absolute(temp);
		return -1;
	}
	if (morpthis_fs_rename_absolute(temp, path) != 0) {
		morpthis_logw("nes", "rename %s to %s: %s", temp, path, strerror(errno));
		morpthis_fs_remove_absolute(temp);
		return -1;
	}
	return 0;
}

// ---------------------------------------------------------------------------
// system interface
// ---------------------------------------------------------------------------

void InfoNES_LoadFrame(void) {
	nes_backend_frame(WorkFrame, NES_DISP_WIDTH, NES_DISP_HEIGHT);

	// With PCM open the blocking write at VBlank is the clock. Without it,
	// pace the emulator here so a silent game does not run at host speed.
	if (!nes_backend_audio_pacing() && !nes_backend_turbo()) {
		static uint64_t next;
		uint64_t now = now_us();
		if (next == 0 || now > next + 100000u) {
			next = now;
		}
		next += FRAME_US;
		if (next > now) {
			sleep_us(next - now);
		}
	}
}

void InfoNES_PadState(DWORD *pdwPad1, DWORD *pdwPad2, DWORD *pdwSystem) {
	*pdwPad1 = (DWORD)nes_backend_pad();
	*pdwPad2 = 0;
	*pdwSystem = 0;
}

int InfoNES_Menu(void) {
	if (nes_backend_should_quit()) {
		return -1;
	}
	if (nes_backend_take_reset()) {
		if (InfoNES_Reset() != 0) {
			nes_backend_error("This ROM uses a mapper the emulator does not support.");
			return -1;
		}
		morpthis_logi("nes", "game reset");
	}
	static int frames;
	if (++frames >= SAVE_EVERY_FRAMES) {
		frames = 0;
		SaveSRAM();
	}
	return 0;
}

void InfoNES_Wait(void) {
	nes_backend_wait();
}

void *InfoNES_MemoryCopy(void *dest, const void *src, int count) {
	memcpy(dest, src, (size_t)count);
	return dest;
}

void *InfoNES_MemorySet(void *dest, int c, int count) {
	memset(dest, c, (size_t)count);
	return dest;
}

void InfoNES_DebugPrint(char *pszMsg) {
	if (pszMsg) {
		morpthis_logd("nes", "%s", pszMsg);
	}
}

void InfoNES_MessageBox(char *pszMsg, ...) {
	if (!pszMsg) {
		return;
	}
	char message[256];
	va_list args;
	va_start(args, pszMsg);
	vsnprintf(message, sizeof message, pszMsg, args);
	va_end(args);
	morpthis_logw("nes", "%s", message);
}

// ---------------------------------------------------------------------------
// thread
// ---------------------------------------------------------------------------

extern "C" void *nes_emulator_thread(void *arg) {
	const char *path = (const char *)arg;
	if (!path || !path[0]) {
		nes_backend_error("The game file is missing.");
		nes_backend_thread_done();
		return NULL;
	}

	if (InfoNES_Load(path) != 0) {
		InfoNES_Fin();
		nes_backend_error("This ROM could not be loaded. It may use an unsupported mapper.");
		nes_backend_thread_done();
		return NULL;
	}

	LoadSRAM();
	morpthis_logi("nes", "emulator started");
	InfoNES_Main();
	SaveSRAM();
	morpthis_logi("nes", "emulator stopped");
	nes_backend_thread_done();
	return NULL;
}
