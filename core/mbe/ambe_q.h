// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// What the half-rate vocoder's dequantizer (ambe_dec.c) and quantizer (ambe_enc.c) share: where each quantizer value
// sits among d0..d48, the inverse DCTs, and the prediction from the last frame. Not for other code.
#ifndef AMBE_Q_H
#define AMBE_Q_H

#include <stdint.h>
#include "ambe.h"

#define AMBE_OUT_GAIN	5751	// log2 7 in Q11: the model amplitude to PCM units (the full-rate decoder's scale)
#define AMBE_B0_SILENCE	124
#define AMBE_BLOCKS	4
#define AMBE_JMAX	17	// the longest residual block

// the quantizer values b0..b8
enum { AMBE_Q_B0, AMBE_Q_B1, AMBE_Q_B2, AMBE_Q_B3, AMBE_Q_B4, AMBE_Q_B5, AMBE_Q_B6, AMBE_Q_B7, AMBE_Q_B8, AMBE_Q_N };

// value b from d0..d48 (b8 is three bits, its lowest bit 0), and into them
int ambe_q_get(const uint8_t d[AMBE_DATA_BYTES], int b);
void ambe_q_put(uint8_t d[AMBE_DATA_BYTES], int b, int v);
// Harmonic l's (1..) voicing band: 500 Hz each, 0..7
int ambe_q_band(uint32_t w0, int l);
// pr[1..L]: 0.65 times the last frame's log amplitudes (s) resampled to L harmonics, Q11. Returns their mean, which a
// frame's amplitudes are predicted without
int32_t ambe_q_predict(ambe_state_t *s, int L, int32_t pr[MBE_MAXL + 1]);
// cos(2 pi num / 2^32 * unit), Q15
int32_t ambe_q_cos(uint32_t unit, int num);

#endif
