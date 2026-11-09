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
#define AUDIO_BLOCK_MS		20u

// Called once per 20 ms block from the driver's deferred context (not an ISR): mic samples in, fill speaker samples out
typedef void (*audio_block_cb_t)(const int16_t *mic, int16_t *spk, void *ctx);

// The stream's timing since boot: a late block plays the speaker buffer's old contents again (a block of the sound
// before it), a dropped one is never seen by the callback
typedef struct {
	uint32_t blocks;	// blocks the callback filled
	uint32_t late;		// a block's turn came before the callback had filled it
	uint32_t dropped;	// blocks skipped because the callback was a whole block late
} audio_stream_stats_t;

typedef struct {
	bool (*start)(audio_block_cb_t cb, void *ctx);
	void (*stop)(void);
	// Route PCM to the speaker (DMR voice, beeps) instead of the modem's own FM path
	void (*speaker_pcm)(bool on);
	// Silence (ms) a beep's own stream plays with the speaker routed before the tone, while the amplifier comes up;
	// 0 = one block (20 ms). An amplifier that unmutes slowly needs more, or short beeps are lost
	uint8_t speaker_lead_ms;
	// Keep the amplifier powered with nothing routed to it (silent), so a sound that follows needs no lead-in. The
	// core holds it on for speaker_hang_s after the last key, PTT, reception or beep (0 or NULL: never; a battery
	// radio leaves it 0 unless its amplifier is slow too)
	void (*speaker_warm)(bool on);
	uint8_t speaker_hang_s;
	// Optional: speaker audio other than silence is still queued or playing (a beep's tail in the driver's buffers).
	// NULL: none is held past the block callback
	bool (*spk_pending)(void);
	// Optional: the stream's timing (debug link view audio.*). NULL: not counted
	void (*stream_stats)(audio_stream_stats_t *out);
} hal_audio_t;

#endif
