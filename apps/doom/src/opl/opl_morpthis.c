// OPL2/OPL3 synthesis for the Morpthis Doom port.
//
// This is the small driver half of Chocolate Doom's OPL library: the same
// OPL3_WriteRegBuffered/OPL_SetCallback interface that i_oplmusic.c expects,
// but rendered into the app's existing PCM mixer instead of an SDL audio
// callback. The emulator itself is opl3.c (Nuked OPL3).

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "opl.h"
#include "opl3.h"
#include "opl_queue.h"

#include "../doom_music.h"

static pthread_mutex_t g_lock;
static pthread_once_t g_lock_once = PTHREAD_ONCE_INIT;

static opl3_chip g_chip;
static opl_callback_queue_t *g_callbacks;
static bool g_initialized;
static bool g_paused;
static uint64_t g_current_us;
static uint64_t g_time_remainder;
static unsigned int g_sample_rate = 44100;
static int g_register;

static void init_lock(void) {
	pthread_mutexattr_t attr;
	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&g_lock, &attr);
	pthread_mutexattr_destroy(&attr);
}

static void opl_lock(void) {
	pthread_once(&g_lock_once, init_lock);
	pthread_mutex_lock(&g_lock);
}

static void opl_unlock(void) {
	pthread_mutex_unlock(&g_lock);
}

// ---------------------------------------------------------------------------
// the subset of opl.c that i_oplmusic uses
// ---------------------------------------------------------------------------

unsigned int opl_sample_rate;

opl_init_result_t OPL_Init(unsigned int port_base) {
	(void)port_base;

	opl_lock();
	if (!g_initialized) {
		g_callbacks = OPL_Queue_Create();
		if (!g_callbacks) {
			opl_unlock();
			return OPL_INIT_NONE;
		}
		OPL3_Reset(&g_chip, g_sample_rate);
		g_current_us = 0;
		g_time_remainder = 0;
		g_paused = false;
		g_initialized = true;
	}
	opl_unlock();
	return OPL_INIT_OPL3;
}

void OPL_Shutdown(void) {
	opl_lock();
	if (g_initialized) {
		OPL_Queue_Destroy(g_callbacks);
		g_callbacks = NULL;
		g_initialized = false;
	}
	opl_unlock();
}

void OPL_SetSampleRate(unsigned int rate) {
	if (rate == 0) {
		return;
	}
	opl_lock();
	g_sample_rate = rate;
	opl_sample_rate = rate;
	if (g_initialized) {
		OPL3_Reset(&g_chip, g_sample_rate);
	}
	opl_unlock();
}

void OPL_WritePort(opl_port_t port, unsigned int value) {
	switch (port) {
	case OPL_REGISTER_PORT:
		g_register = (int)value;
		break;
	case OPL_REGISTER_PORT_OPL3:
		g_register = (int)value | 0x100;
		break;
	case OPL_DATA_PORT:
		OPL_WriteRegister(g_register, (int)value);
		break;
	}
}

unsigned int OPL_ReadPort(opl_port_t port) {
	(void)port;
	return 0;
}

unsigned int OPL_ReadStatus(void) {
	return 0;
}

void OPL_WriteRegister(int reg, int value) {
	opl_lock();
	if (g_initialized) {
		OPL3_WriteRegBuffered(&g_chip, (uint16_t)reg, (uint8_t)value);
	}
	opl_unlock();
}

opl_init_result_t OPL_Detect(void) {
	return OPL_INIT_OPL3;
}

// Initialize all registers on startup. This mirrors opl.c's register setup
// without the bus-timing reads, which a software emulator does not need.
void OPL_InitRegisters(int opl3) {
	int r;

	for (r = OPL_REGS_LEVEL; r <= OPL_REGS_LEVEL + OPL_NUM_OPERATORS; ++r) {
		OPL_WriteRegister(r, 0x3f);
	}
	for (r = OPL_REGS_ATTACK; r <= OPL_REGS_WAVEFORM + OPL_NUM_OPERATORS; ++r) {
		OPL_WriteRegister(r, 0x00);
	}
	for (r = 1; r < OPL_REGS_LEVEL; ++r) {
		OPL_WriteRegister(r, 0x00);
	}

	OPL_WriteRegister(OPL_REG_TIMER_CTRL, 0x60);
	OPL_WriteRegister(OPL_REG_TIMER_CTRL, 0x80);
	OPL_WriteRegister(OPL_REG_WAVEFORM_ENABLE, 0x20);

	if (opl3) {
		OPL_WriteRegister(OPL_REG_NEW, 0x01);
		for (r = OPL_REGS_LEVEL; r <= OPL_REGS_LEVEL + OPL_NUM_OPERATORS; ++r) {
			OPL_WriteRegister(r | 0x100, 0x3f);
		}
		for (r = OPL_REGS_ATTACK; r <= OPL_REGS_WAVEFORM + OPL_NUM_OPERATORS; ++r) {
			OPL_WriteRegister(r | 0x100, 0x00);
		}
		for (r = 1; r < OPL_REGS_LEVEL; ++r) {
			OPL_WriteRegister(r | 0x100, 0x00);
		}
	}

	OPL_WriteRegister(OPL_REG_FM_MODE, 0x40);
	if (opl3) {
		OPL_WriteRegister(OPL_REG_NEW, 0x01);
	}
}

void OPL_SetCallback(uint64_t us, opl_callback_t callback, void *data) {
	opl_lock();
	if (g_initialized && g_callbacks) {
		OPL_Queue_Push(g_callbacks, callback, data, g_current_us + us);
	}
	opl_unlock();
}

void OPL_ClearCallbacks(void) {
	opl_lock();
	if (g_callbacks) {
		OPL_Queue_Clear(g_callbacks);
	}
	opl_unlock();
}

void OPL_Lock(void) {
	opl_lock();
}

void OPL_Unlock(void) {
	opl_unlock();
}

void OPL_Delay(uint64_t us) {
	(void)us;
}

void OPL_SetPaused(int paused) {
	opl_lock();
	g_paused = paused != 0;
	opl_unlock();
}

void OPL_AdjustCallbacks(float factor) {
	opl_lock();
	if (g_callbacks) {
		OPL_Queue_AdjustCallbacks(g_callbacks, g_current_us, factor);
	}
	opl_unlock();
}

// ---------------------------------------------------------------------------
// rendering
// ---------------------------------------------------------------------------

void dg_opl_render(int16_t *stereo, int frames) {
	if (!stereo || frames <= 0) {
		return;
	}

	opl_lock();
	if (!g_initialized || g_paused || !g_callbacks) {
		memset(stereo, 0, (size_t)frames * 2 * sizeof(int16_t));
		opl_unlock();
		return;
	}

	int filled = 0;
	while (filled < frames) {
		// Run every callback that is due at the current emulated time.
		while (!OPL_Queue_IsEmpty(g_callbacks) &&
			   OPL_Queue_Peek(g_callbacks) <= g_current_us) {
			opl_callback_t callback;
			void *data;
			if (!OPL_Queue_Pop(g_callbacks, &callback, &data)) {
				break;
			}
			if (callback) {
				callback(data);
			}
		}

		int count = frames - filled;
		if (!OPL_Queue_IsEmpty(g_callbacks)) {
			uint64_t next = OPL_Queue_Peek(g_callbacks);
			if (next > g_current_us) {
				uint64_t delta = next - g_current_us;
				uint64_t samples = (delta * g_sample_rate + 999999) / 1000000;
				if (samples == 0) {
					samples = 1;
				}
				if (samples < (uint64_t)count) {
					count = (int)samples;
				}
			}
		}

		OPL3_GenerateStream(&g_chip, stereo + filled * 2, (uint32_t)count);
		g_time_remainder += (uint64_t)count * 1000000;
		g_current_us += g_time_remainder / g_sample_rate;
		g_time_remainder %= g_sample_rate;
		filled += count;
	}

	opl_unlock();
}
