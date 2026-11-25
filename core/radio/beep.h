// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Beeps and alert tones (docs/spec/radio-features.md 4): short tone patterns synthesized as 8 kHz PCM and played
// through the board's PCM audio (hal_audio_t), so they follow the volume knob like received audio does.
//
// Ownership of the PCM stream: while DMR voice streams (core/dmr/ambe), its audio callback mixes the beep in
// (beep_mix). Otherwise beep_play starts the stream with the beep's own callback and beep_service, in the radio task,
// stops it once the pattern has played. Both the DMR voice start and beep_service run in the radio task, so the two
// never race over hal_audio_t.start / stop.
//
// Threading: beep_play and beep_service in the radio task; beep_mix and the own callback in the board's audio task.
// A pattern is handed over with a sequence counter (single producer, single consumer).
#ifndef RADIO_BEEP_H
#define RADIO_BEEP_H

#include <stdint.h>
#include <stdbool.h>
#include "hal_audio.h"

typedef enum {
	BEEP_NONE,
	BEEP_KEY,		// a key press
	BEEP_ERROR,		// a key that does nothing here, a refused request
	BEEP_TX_PERMIT,		// DMR: on air, talk now
	BEEP_TX_DENY,		// PTT refused, or the transmission was cut (time-out)
	BEEP_CALL_END,		// DMR: the call heard has ended
	BEEP_TOT_WARN,		// the TX time-out is near
	BEEP_BATTERY_LOW,
	BEEP_BATTERY_CRIT,
	BEEP_SCAN_HOLD,		// the scan stopped on a signal (unused by default)
	BEEP_EMERGENCY,		// DMR: someone's emergency heard (alarm or emergency call; core/dmr/dmr_emergency.h)
	BEEP_MESSAGE,		// a text message arrived (the UI, if Settings > Sound > Message tone)
	BEEP_TX_PERMIT_SHORT,	// DMR: talk now, played before keying (CAP_BEEP_NOT_ON_AIR): one short tone
	BEEP_SWEEP,		// speaker test (debug link only): a 4 s sweep, BEEP_SWEEP_LO_HZ to BEEP_SWEEP_HI_HZ
	BEEP_SYSTEM_BACK,	// a trunked system found again, the radio back on it from its fallback channel
	BEEP_COUNT
} beep_t;

// A signaling tone (sigtone.h): steps handed to beep_play_custom, not a fixed pattern. Not in the enum: it has no entry
// in the pattern table, and the beep reel and the debug link go through that
#define BEEP_CUSTOM	((beep_t)BEEP_COUNT)

typedef struct {
	uint16_t hz;		// 0 = silence, BEEP_HZ_SWEEP = the sweep
	uint16_t ms;
} beep_seg_t;

// A segment with this hz is a linear sweep from BEEP_SWEEP_LO_HZ to BEEP_SWEEP_HI_HZ over its length, phase continuous
#define BEEP_HZ_SWEEP		1u
#define BEEP_SWEEP_LO_HZ	150u
#define BEEP_SWEEP_HI_HZ	3800u	// the 8 kHz stream's band, short of its 4 kHz edge

typedef struct {
	uint32_t played;	// patterns started
	uint32_t no_stream;	// patterns dropped: no PCM audio, or the stream would not start (no codec clock)
	uint32_t blocks;	// 20 ms blocks with a beep in them
	uint32_t stalls;	// own streams given up: no block for STALL_POLLS polls (the codec clock stopped)
	uint32_t shared;	// patterns mixed into the voice stream (no lead-in of their own)
	uint32_t leads;		// silent lead-in blocks played on the own stream
	uint32_t warms;		// times the amplifier was powered up ahead of a sound (hal_audio_t.speaker_warm)
	uint8_t last;		// the last pattern asked for (beep_t), played or dropped
} beep_stats_t;

#define BEEP_SEGS_MAX	6
#define BEEP_CUSTOM_MAX	10	// steps of a custom pattern (SIG_STEPS)

// audio NULL: beeps are counted as dropped
// The tone generator both the beep engine and the signaling tones (the DMR tone frames, ambeVoice.c) use: 8 kHz mono,
// phase continuous across steps, 2 ms fades at each step's edges. It reads the steps where they are (they must stay
// until it is done); BEEP_HZ_SWEEP is a step too
typedef struct {
	const beep_seg_t *seg;
	uint8_t n, i;
	uint32_t left;		// samples left in the step
	uint32_t len;		// samples in the step
	uint32_t phase, step, dstep;
	int32_t amp;		// Q15 of full scale
} beep_gen_t;
void beep_gen_start(beep_gen_t *g, const beep_seg_t *s, uint8_t n, int32_t amp_q15);
// Fill count samples; the unplayed rest is zero. True while the sequence still has samples after this call
bool beep_gen_block(beep_gen_t *g, int16_t *pcm, unsigned count);

void beep_init(const hal_audio_t *audio);
// Start a pattern at level 1..100 (0 or BEEP_NONE: nothing). shared: the DMR voice stream is running and mixes the
// beep in; otherwise the beep starts its own stream. Replaces a pattern still playing
void beep_play(beep_t b, uint8_t level, bool shared);
// Play a pattern given as steps (n up to BEEP_CUSTOM_MAX; 0 or a step list with no tone: nothing). The steps are copied,
// so the caller's array may go. Two copies alternate: a pattern still playing is not rewritten under the audio side.
// Built with CONFIG_SIG_TONES only
void beep_play_custom(const beep_seg_t *segs, uint8_t n, uint8_t level, bool shared);
// Every radio poll: stop the beep's own stream once the pattern is over (shared: the voice stream took over), and
// let the amplifier go once its hang has run out
void beep_service(bool shared, uint32_t now_ms);
#define BEEP_SERVICE_NOW	1	// beep_service takes now_ms (the beep reel builds older beep.h files too)
// The user or the radio did something a sound may follow (a key, PTT, a reception, a beep): on a board with
// speaker_hang_s, power the amplifier now and hold it for that long
void beep_warm(uint32_t now_ms);
// The amplifier is held powered (beep_warm)
bool beep_warm_on(void);
// From an audio block callback: add the beep to the block (saturating). True while the beep sounds
bool beep_mix(int16_t *spk);
// The speaker's sound reaches the microphone (a handheld's speaker sits next to it): true from the block after
// beep_mix first returned true until BEEP_ECHO_BLOCKS after it last did, the stream's delay from the speaker block to
// the mic block that hears it, plus one. A voice layer that transmits while beeps mix into its stream mutes the mic
// for these blocks (its mic gate, ambeVoiceSetMicGate), so a beep never goes on air with the voice. The same audio
// task as beep_mix, before it in the block
bool beep_mic_echo(void);
#define BEEP_ECHO_BLOCKS	3	// 60 ms: the speaker's block is heard in the mic's two blocks later (I2S, on the radio)
// Whether a pattern is still playing (as last seen by the audio side)
bool beep_busy(void);
// The pattern's segments (for tests and docs); count returned
uint8_t beep_pattern(beep_t b, const beep_seg_t **segs);
void beep_get_stats(beep_stats_t *out);
void beep_reset_stats(void);

// The beep's own audio block callback (exposed for tests)
void beep_audio_block(const int16_t *mic, int16_t *spk, void *ctx);

#endif
