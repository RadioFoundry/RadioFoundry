// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
/*
 * D-STAR voice between the engine (dstar_engine.h, 72-bit voice frames as on air) and the board's PCM audio
 * (hal_audio_t). docs/spec/dstar.md 5, the same design as the P25 voice path (core/p25/p25Voice.h).
 *
 *  RX: the engine's rx_voice hook queues each frame; the audio block callback takes one per 20 ms and decodes it
 *      with the half-rate decoder (core/mbe/ambe.h), its FEC included. Frames come one every 20 ms, a chunk of air at
 *      a time, so playback starts once DSTAR_VOICE_PRIME are queued. A block with nothing queued during a call goes
 *      to the decoder as a lost frame: it repeats the last good frame, then mutes.
 *  TX: while a transmission is on (dstarVoiceTx) the audio block callback encodes each mic block with the half-rate
 *      encoder and queues the frame; the engine's tx_voice hook takes them one at a time. With none ready the hook
 *      says so and the engine sends silence (its tx_filler). The speaker is silent meanwhile.
 *
 * Threading: as the P25 voice path's. No RTOS call. Memory is the caller's (dstar_voice_mem_t): it may be lent by
 * something idle while D-STAR runs (hal_vocoder_t.lend).
 */
#ifndef RFOUNDRY_DSTAR_VOICE_H
#define RFOUNDRY_DSTAR_VOICE_H

#include <stdbool.h>
#include <stdint.h>
#include "hal_audio.h"
#include "../mbe/ambe.h"
#include "../mbe/mbe_est.h"
#include "dstar_engine.h"

#define DSTAR_VOICE_QUEUE	16	// frames (320 ms), power of two: room for the air's chunks and jitter
#define DSTAR_VOICE_PRIME	3	// frames queued before playback starts: a 30 ms chunk of air and a frame to spare
#define DSTAR_VOICE_TX_QUEUE	8	// encoded frames waiting for the engine (160 ms), power of two

typedef struct {
	uint8_t seq;		// the call it belongs to
	uint8_t voice[DSTAR_VOICE_BYTES];
} dstar_voice_entry_t;

typedef struct {
	uint8_t gen;		// the transmission it belongs to
	uint8_t voice[DSTAR_VOICE_BYTES];
} dstar_voice_tx_entry_t;

// Everything the voice path needs while D-STAR runs: the queues, and the decoder's and the encoder's kept and scratch
// memory. A radio transmits or receives, so the two share the prediction state and the scratch; only the estimator's
// history is the encoder's own
typedef struct {
	dstar_voice_entry_t q[DSTAR_VOICE_QUEUE];
	dstar_voice_tx_entry_t tq[DSTAR_VOICE_TX_QUEUE];
	mbe_est_state_t est;
	ambe_dec_state_t st;
	mbe_params_t pr;
	mbe_state_t ms;
	mbe_frame_t prev, cur;
	int32_t buf[MBE_BUF_WORDS];
} dstar_voice_mem_t;

typedef struct {
	uint32_t rx_in;		// frames the engine handed in
	uint32_t rx_frames;	// good frames decoded and played
	uint32_t rx_bad;	// frames the FEC could not trust (played as concealment)
	uint32_t rx_concealed;	// blocks the decoder filled from a bad or missing frame (repeat, then mute)
	uint32_t rx_calls;	// calls that started playing
	uint32_t rx_underruns;	// blocks with nothing queued during a call
	uint32_t rx_underrun_at;	// frames the call had played at its latest underrun
	uint32_t rx_overflows;	// frames dropped, queue full
	uint32_t rx_bit_errors;	// bits corrected by the FEC in good frames
	uint32_t rx_queue_max;	// most frames queued at once (of DSTAR_VOICE_QUEUE)
	uint32_t rx_peak;	// largest decoded sample (absolute) in the latest call that played
	uint32_t tx_frames;	// frames handed to the engine for transmission
	uint32_t tx_encoded;	// mic blocks encoded
	uint32_t tx_dropped;	// encoded frames dropped, queue full (the engine took none in time)
	uint32_t tx_silent;	// of tx_encoded, the frames the encoder made silence frames (no signal at all)
	uint32_t tx_mic_peak;	// largest mic sample (absolute, after the gain) in the latest transmission
} dstar_voice_stats_t;

// The board's PCM audio (NULL: frames are counted, nothing plays). Once, at start-up
void dstarVoiceInit(const hal_audio_t *audio);
// Start on mem (radio task, before dstar_engine_begin); the stream starts with the first call (dstarVoicePoll)
void dstarVoiceBegin(dstar_voice_mem_t *mem);
// Radio task, every tick while D-STAR runs: starts the audio stream once a call is heard
void dstarVoicePoll(void);
// A transmission starts (the stream starts if it was not running; from the next block the mic is encoded) or ends.
// Radio task, around the engine's transmit calls
void dstarVoiceTx(bool on);
// The mic samples' gain step, as ambeVoiceSetMicGain (radio/mic_gain.h). Any task
void dstarVoiceSetMicGain(uint8_t step);
// Stop (radio task, after dstar_engine_end): the stream stops and mem is free again
void dstarVoiceEnd(void);
// Whether the voice stream runs (its callback owns hal_audio_t). Radio task only
bool dstarVoiceStreaming(void);
// Something else to hear while the stream runs (beeps), as ambeVoiceSetMixer
typedef bool (*dstar_voice_mixer_t)(int16_t *spk);
void dstarVoiceSetMixer(dstar_voice_mixer_t mixer);
// The engine's hooks into this module (ctx unused)
const dstar_engine_hooks_t *dstarVoiceHooks(void);
void dstarVoiceGetStats(dstar_voice_stats_t *out);
void dstarVoiceResetStats(void);

// The audio block callback (exposed for tests): one 20 ms block in, one out
void dstarVoiceAudioBlock(const int16_t *mic, int16_t *spk, void *ctx);

#endif
