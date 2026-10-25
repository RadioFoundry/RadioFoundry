// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// PCM audio between the MCU and the radio's codec (mic in, speaker out): 8 kHz, 16-bit signed, 20 ms blocks.
// Used for DMR voice (vocoder in / out), beeps and voice prompts. FM audio does not pass through here on radios
// whose modem chip routes it (the HR_C6000).
#ifndef HAL_AUDIO_H
#define HAL_AUDIO_H

#include <stdint.h>
#include <stdbool.h>

#define AUDIO_RATE_HZ		8000
#define AUDIO_BLOCK_SAMPLES	160	// 20 ms

// Called once per 20 ms block from the driver's deferred context (not an ISR): mic samples in, fill speaker samples out
typedef void (*audio_block_cb_t)(const int16_t *mic, int16_t *spk, void *ctx);

typedef struct {
	bool (*start)(audio_block_cb_t cb, void *ctx);
	void (*stop)(void);
	// Route PCM to the speaker (DMR voice, beeps) instead of the modem's own FM path
	void (*speaker_pcm)(bool on);
} hal_audio_t;

#endif
