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
	// A second path to the speaker, for a mode where the stream's own output doesn't reach it (a baseband chip's
	// FIFO): 32-sample chunks written, those written as silence because no block was ready, blocks with no room
	uint32_t fifo_chunks;
	uint32_t fifo_gaps;
	uint32_t fifo_full;
	// The second path carries the speaker's PCM in the mode the radio is in now (a beep in FM receive goes there)
	bool fifo_fm;
} audio_stream_stats_t;

// The power of a second speaker output (hal_spk_aux_t.power)
enum {
	SPK_AUX_OFF = 0,	// amplifier off
	SPK_AUX_ARMED = 1,	// amplifier on, speaker muted: the output settles at rest
	SPK_AUX_OPEN = 2,	// speaker open: the output plays its queue, oldest block first
};

// A second output to the speaker, for while the stream's own can't be heard there (a speaker with no level while DMR
// is on air). The core (core/radio/spk_aux.h) moves each speaker block with sound here while active() says so, powers
// the output around it (lead_ms before the sound, tail_ms after) and sends silence down the stream instead. Audio task
struct spk_aux_core;
typedef struct hal_spk_aux {
	bool (*active)(void);			// the stream's speaker output can't be heard now
	void (*power)(uint8_t state);		// SPK_AUX_*
	void (*write)(const int16_t *pcm);	// one block, queued behind the ones before it (the output holds 4 or more)
	uint8_t lead_ms;	// the amplifier's settling before the speaker opens (the sound waits in the queue)
	uint8_t tail_ms;	// the speaker muted before the amplifier goes off
	// The core's step: SPK_AUX_CORE (core/radio/spk_aux.h). The core reaches its step only through here, so a board
	// without the output links none of it
	const struct spk_aux_core *core;
} hal_spk_aux_t;

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
	// Optional: the speaker audio queued now is thrown away (the DMR engine is on air and the audio path is gated).
	// A sound that must be heard whole waits until this is false. NULL: never. Set only where the path drops all
	// speaker audio for the whole of a DMR transmission: the DMR mic gate lifts on such a board (radio_sm.c)
	bool (*speaker_blocked)(void);
	// Optional: a second output to the speaker for while the stream's can't be heard. NULL: none
	const hal_spk_aux_t *spk_aux;
	// Optional: the stream's timing (debug link view audio.*). NULL: not counted
	void (*stream_stats)(audio_stream_stats_t *out);
} hal_audio_t;

#endif
