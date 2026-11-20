// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The D-STAR voice codec seen from the DV layer (docs/spec/dstar-modem.md 4): 20 ms of 8 kHz speech is one 72-bit
// frame (AMBE at 3600 bps with its FEC, JARL D-STAR standard 4.1.2 b). RadioFoundry ships no codec; a board that can
// run one fills this in, and the framing (dstar_frame.h) never looks inside a frame. Nine bytes, air order: the
// codec's bits as the standard sends them, byte 0 first, each byte least significant bit first.
#ifndef RFOUNDRY_DSTAR_VOICE_H
#define RFOUNDRY_DSTAR_VOICE_H

#include <stdint.h>
#include <stdbool.h>

#define DSTAR_VOICE_BITS	72u
#define DSTAR_VOICE_BYTES	9u
#define DSTAR_VOICE_SAMPLES	160u	// 20 ms at 8 kHz

typedef struct {
	// 160 samples of speech into one frame. False when the codec can't (not loaded, busy)
	bool (*encode)(void *ctx, const int16_t pcm[DSTAR_VOICE_SAMPLES], uint8_t frame[DSTAR_VOICE_BYTES]);
	// One frame into 160 samples. False when the codec can't; the caller plays silence
	bool (*decode)(void *ctx, const uint8_t frame[DSTAR_VOICE_BYTES], int16_t pcm[DSTAR_VOICE_SAMPLES]);
	void *ctx;
} dstar_vocoder_t;

// The voice frame the standard gives for a lost packet (6.6, the first nine bytes of 9E 8D 32 88 26 1A 3F 61 E8):
// what the transmitter sends with no codec (a data-only or test transmission) and what a gateway substitutes
extern const uint8_t dstar_voice_silence[DSTAR_VOICE_BYTES];

#endif
