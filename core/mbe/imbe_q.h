// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// What the full-rate vocoder's dequantizer (imbe_dec.c) and quantizer (imbe_enc.c) share: the bit allocation, the order
// of the bits in a frame, and the prediction from the last frame. Not for other code.
#ifndef IMBE_Q_H
#define IMBE_Q_H

#include <stdint.h>
#include "imbe.h"
#include "../dsp/fxp.h"

#define IMBE_OUT_GAIN	5751	// log2 7 in Q11: the model amplitude to PCM units
#define IMBE_W0_STEPS	208	// valid pitch codes 0..207
#define IMBE_Q16_STEP	21475	// 1e-5 / 2 in Q16, as the table steps are in 1e-5 and the levels are odd multiples of half a step

typedef struct {
	const uint8_t *in;		// the frame read from, when out is NULL
	uint8_t *out;			// the frame built (zeroed by the caller)
	int i;				// next bit of the frame
	uint16_t val[MBE_MAXL + 2];	// the quantizer values b1 .. b(L+1)
} imbe_bits_t;

// the fundamental of pitch code b0 (0..207), as mbe_params_t.w0
uint32_t imbe_q_w0(int b0);
// Pitch code b0: the harmonic count L (returned, 9..56), the voicing bands K, and nb[3..L+1], the bits of b3..b(L+1)
int imbe_q_alloc(int b0, int *K, uint8_t nb[MBE_MAXL + 2]);
// The frame's bits 6..84, each moved between the frame and its quantizer value (which way: c->out)
void imbe_q_layout(imbe_bits_t *c, int L, int K, const uint8_t *nb);
// pr[1..L]: what the last frame (s) predicts of this frame's log amplitudes, Q16, its mean removed. A frame's
// amplitudes are the residual the DCT blocks carry plus this
void imbe_q_predict(imbe_state_t *s, int L, int32_t pr[MBE_MAXL + 1]);

// a uniform quantizer's level, Q16: lev is the odd number 2 b - 2^bits + 1, step in units of 1e-5
FXP_INLINE int32_t imbe_q_level(int32_t lev, int32_t step) { return fxp_mulshr(lev * step, IMBE_Q16_STEP, 16); }

#endif
