// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// PCM audio over I2S3 to the built-in codec of an HR_C6000 or HR_C5000, shared by the STM32F4 radios with one of
// them: the stream behind hal_audio_t's start and stop (i2s_pcm.c). The chip is the I2S master; SPI3 receives the
// microphone on PC12 (I2S3_SD), I2S3ext sends speaker PCM on PC11 (I2S3ext_SD), CK on PC10 and WS on PA15. The pins are
// the same on every such board. The board keeps the speaker routing (hal_audio_t.speaker_pcm).
#ifndef STM32_I2S_PCM_H
#define STM32_I2S_PCM_H

#include <stdint.h>
#include <stdbool.h>
#include "config.h"
#include "hal_audio.h"

typedef struct {
	bool ckpol;		// clock idle high, else idle low: as the board's chip set-up leaves it
	// Bench knob: when neither CK nor WS is driven, the STM32 generates them (SPI3 master receive). Only for a chip
	// that doesn't drive its I2S; both lines must follow a pull-up and a pull-down first
	bool master_if_free;
	// Optional: the chip's I2S clocks on (start) and off (stop), for chips whose set-ups leave them off
	void (*clock)(bool on);
	// Optional: every block as received, left and right interleaved (2 x AUDIO_BLOCK_SAMPLES), before the callback.
	// Audio task context
	void (*raw_block)(const int16_t *lr);
} stm32_i2s_pcm_cfg_t;

typedef struct {
	uint32_t blocks;	// 20 ms blocks processed
	uint32_t overruns;	// a DMA half completed before the task took the previous one
	uint32_t dropped;	// blocks skipped because the task was a whole block late
} stm32_i2s_pcm_stats_t;

#if CONFIG_DMR
// hal_audio_t.start with the board's set-up (cfg stays valid). False: no I2S clock from the chip (powered down, or its
// codec not running)
bool stm32_i2s_pcm_start(const stm32_i2s_pcm_cfg_t *cfg, audio_block_cb_t cb, void *ctx);
// hal_audio_t.stop. Safe from the block callback itself: streaming stops, the task stays for the next start
void stm32_i2s_pcm_stop(void);
bool stm32_i2s_pcm_running(void);
void stm32_i2s_pcm_stats(stm32_i2s_pcm_stats_t *out);
void stm32_i2s_pcm_reset_stats(void);
#endif

#endif
