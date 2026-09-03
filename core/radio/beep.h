// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
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
	BEEP_COUNT
} beep_t;

typedef struct {
	uint16_t hz;		// 0 = silence
	uint16_t ms;
} beep_seg_t;

typedef struct {
	uint32_t played;	// patterns started
	uint32_t no_stream;	// patterns dropped: no PCM audio, or the stream would not start (no codec clock)
	uint32_t blocks;	// 20 ms blocks with a beep in them
} beep_stats_t;

#define BEEP_SEGS_MAX	6

// audio NULL: beeps are counted as dropped
void beep_init(const hal_audio_t *audio);
// Start a pattern at level 1..100 (0 or BEEP_NONE: nothing). shared: the DMR voice stream is running and mixes the
// beep in; otherwise the beep starts its own stream. Replaces a pattern still playing
void beep_play(beep_t b, uint8_t level, bool shared);
// Every radio poll: stop the beep's own stream once the pattern is over (shared: the voice stream took over)
void beep_service(bool shared);
// From an audio block callback: add the beep to the block (saturating). True while the beep sounds
bool beep_mix(int16_t *spk);
// Whether a pattern is still playing (as last seen by the audio side)
bool beep_busy(void);
// The pattern's segments (for tests and docs); count returned
uint8_t beep_pattern(beep_t b, const beep_seg_t **segs);
void beep_get_stats(beep_stats_t *out);
void beep_reset_stats(void);

// The beep's own audio block callback (exposed for tests)
void beep_audio_block(const int16_t *mic, int16_t *spk, void *ctx);

#endif
