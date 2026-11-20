// SPDX-License-Identifier: MIT AND ISC
// Copyright (c) 2026 The RadioFoundry Authors
// SPDX-FileCopyrightText: 2026 cleanq contributors
// SPDX-FileCopyrightText: 2010 mbelib Author (the bitstream it restates, ISC license, see LICENSES/ISC.txt)
/*
 * D-STAR AMBE 3600x2400 parameter quantizer and dequantizer (docs/spec/dstar-codec.md 3): MBE model parameters to the
 * nine indices b0..b8 of one voice frame (48 speech bits) and back, in integer arithmetic.
 *
 * This layer only maps parameters to bits and back. Analysis (speech -> parameters, dstar_mbe_analysis.h) and synthesis
 * (parameters -> speech, dstar_mbe_synth.h) sit either side of it, so either can be replaced without touching the
 * bitstream code; the frame FEC (dstar_ambe_frame.h) is separate too.
 *
 * The bitstream is defined by mbelib's decoder (ISC): the dequantizer is that decoder in integer arithmetic and the
 * quantizer is its inverse. No heap, no globals: state is the caller's, one state per direction of one stream.
 *
 * Fixed-point conventions:
 *   f0_q24     fundamental frequency in cycles per sample, Q24 (125 Hz at 8 kHz is 125 / 8000 * 2^24 = 262144)
 *   log2_amp   log2 of the spectral magnitude, Q12, in mbelib's convention: its float Ml = exp(0.693 * log2Ml), and
 *              unvoiced bands are further scaled by 0.2046 / sqrt(w0) at synthesis. That scaling is the synthesizer's
 *              business and is not applied here
 */
#ifndef RFOUNDRY_DSTAR_AMBE_QUANT_H
#define RFOUNDRY_DSTAR_AMBE_QUANT_H

#include <stdint.h>

#define DSTAR_AMBE_MAX_L	56	// harmonics at the lowest pitch
#define DSTAR_AMBE_FIELDS	9	// b0..b8

typedef struct {
	uint32_t f0_q24;				// fundamental / sample rate, Q24
	uint8_t L;					// harmonics present, 1..56
	uint8_t voiced[DSTAR_AMBE_MAX_L + 1];		// [l] = 1 if harmonic l is voiced; [0] unused
	int32_t log2_amp[DSTAR_AMBE_MAX_L + 1];		// [l], Q12; [0] unused
} dstar_ambe_params_t;

// Inter-frame memory, the same in meaning at both ends: the previous frame's decoded log magnitudes and gain. The
// encoder keeps its own copy of what the decoder will hold, so the prediction never drifts (closed loop)
typedef struct {
	int32_t gamma_q16;
	uint8_t L;
	int32_t log2_amp[DSTAR_AMBE_MAX_L + 1];
} dstar_ambe_state_t;

#define DSTAR_AMBE_BANDS	24	// mel bands of the quantizer's perceptual measure (dstar_ambe_quant.c)

// The perceptual measure of one frame, built once and read by every candidate (dstar_ambe_quant.c, perc_setup)
typedef struct {
	int16_t zc[4][6][DSTAR_AMBE_BANDS];	// band response of block i's DCT coefficient k, weighted, Q13
	int32_t yr[DSTAR_AMBE_BANDS];		// band response of the target residual, weighted, Q12
	int32_t hq[4][16];			// h'Gh per higher-order codeword of block i, Q22
	uint32_t pa_sum;			// the target's power relative to its loudest harmonic, Q20
} dstar_ambe_perc_t;

// Working memory for one call, dead between calls: the caller may share it with other per-call scratch. It holds what
// would otherwise be about 4 KB of stack (the quantizer's searches, their quadratic form and band responses, and two
// decodes)
typedef struct {
	dstar_ambe_params_t dec;			// dstar_ambe_quantize's own decodes when the caller wants none
	dstar_ambe_state_t trial;			// the prediction memory of its trial decode (the gain match)
	union {
		struct {
			int32_t T[DSTAR_AMBE_MAX_L + 1];	// the inverse block transforms' output
			int32_t c[DSTAR_AMBE_MAX_L + 1];	// the prediction
		} deq;
		struct {
			int32_t a[DSTAR_AMBE_MAX_L + 1];	// input magnitudes on the quantized pitch's harmonics
			int32_t c[DSTAR_AMBE_MAX_L + 1];	// the prediction
			uint8_t v[DSTAR_AMBE_MAX_L + 1];	// input voicing on those harmonics
			union {
				struct {
					int32_t A[7][7];	// the PRBA search's quadratic form
					int64_t Qx[7][8];	// its setup
				} prba;
				struct {			// perc_setup's working values, per band and per harmonic
					uint32_t E[DSTAR_AMBE_BANDS], fl[DSTAR_AMBE_BANDS];
					int32_t lm[DSTAR_AMBE_BANDS], sv[DSTAR_AMBE_BANDS], v10[DSTAR_AMBE_BANDS];
					int32_t tr[DSTAR_AMBE_BANDS], ssum[DSTAR_AMBE_BANDS];
					uint8_t seg[DSTAR_AMBE_MAX_L + 1];
					uint16_t wu[DSTAR_AMBE_MAX_L + 1];
				} perc;
			} w;
			dstar_ambe_perc_t P;			// the perceptual measure, after the PRBA search
		} q;
	} u;
} dstar_ambe_scratch_t;

enum { DSTAR_AMBE_VOICE = 0, DSTAR_AMBE_TONE = 1 };

// mbelib's mbe_initMbeParms: previous L = 30, gain 0, magnitudes 1
void dstar_ambe_init(dstar_ambe_state_t *s);

// b[0..8] (b8 carries only its three high bits; its low bit is ignored). Returns DSTAR_AMBE_VOICE, or DSTAR_AMBE_TONE
// for b0 = 126 or 127 (tone and reserved frames): the state is then reset, as mbelib does, and out->L is 0
int dstar_ambe_dequantize(dstar_ambe_state_t *s, dstar_ambe_scratch_t *t, const uint16_t b[DSTAR_AMBE_FIELDS],
			  dstar_ambe_params_t *out);

// Chooses b[0..8] for *in and advances *s exactly as dstar_ambe_dequantize will at the far end. dec, when not NULL,
// receives the decoded parameters (it may not be inside *t). in->L may differ from the L the quantized pitch implies:
// the magnitudes are resampled. Never emits a tone index. *in may not be inside *t either
void dstar_ambe_quantize(dstar_ambe_state_t *s, dstar_ambe_scratch_t *t, const dstar_ambe_params_t *in,
			 uint16_t b[DSTAR_AMBE_FIELDS], dstar_ambe_params_t *dec);

#endif
