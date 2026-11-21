// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// SPDX-FileCopyrightText: 2026 mbe contributors
/*
 * Fixed-point MBE speech analysis for the D-STAR codec (docs/spec/dstar-codec.md 2): 160 samples of 8 kHz speech a
 * 20 ms frame -> the quantizer's parameters (f0, L, voicing, log2 magnitudes, dstar_ambe_quant.h).
 *
 * Method (Griffin & Lim 1988; Hardwick 1988; the expired MBE pitch-tracking patents, docs/spec/dstar-codec.md
 * Provenance): a time-domain pitch error function on a half-sample grid, tracked with look-back and two frames of
 * look-ahead, refined to an eighth of a sample by fitting harmonics to a 256-point spectrum; voicing from the residual
 * of that fit per 1 kHz pair of bands; magnitudes from band energy, measured at the harmonics of the D-STAR pitch grid.
 *
 * Output scaling (what the quantizer and mbelib's decoder expect; derived rather than swept, see
 * dstar_mbe_analysis.c):
 *   voiced band l:    a sinusoid of PCM amplitude a gives log2_amp = log2(a / 7), because mbelib's output is
 *                     7 x sum(Ml cos(...))
 *   unvoiced band l:  band power P (PCM units squared) gives
 *                     log2_amp = log2(P) / 2 + log2(w0) / 2 - log2(0.2046) - log2(Kuv),
 *                     Kuv^2 = 1.5 x (7 x 1.35295)^2 x 0.8959, the mean power mbelib's three-sine unvoiced mix puts in
 *                     a band per unit Ml after its 0.2046 / sqrt(w0) scale
 * Both are then divided by 0.99979 because mbelib forms Ml = exp(0.693 x) rather than 2^x.
 *
 * Delay: the parameters are those of the frame centered 480 samples (60 ms) before the end of the newest block: 150
 * samples of pitch window, 10 of low-pass filter and two 160-sample frames of look-ahead for the tracker.
 *
 * Integer only, no heap, no globals. The state is the caller's; the scratch is live only during a call and may be
 * shared with other per-call scratch.
 */
#ifndef RFOUNDRY_DSTAR_MBE_ANALYSIS_H
#define RFOUNDRY_DSTAR_MBE_ANALYSIS_H

#include <stdint.h>
#include "dstar/dstar_ambe_quant.h"

#define DSTAR_MBE_FRAME		160	// samples a frame
#define DSTAR_MBE_ANA_BUF	640	// input history, samples
#define DSTAR_MBE_NPITCH	215	// the half-sample pitch grid, 20.0 .. 127.0

typedef struct {
	int16_t buf[DSTAR_MBE_ANA_BUF];		// high-passed input, newest last
	int32_t hp_x1, hp_y1;			// DC blocker state (y in Q8)
	uint16_t E[3][DSTAR_MBE_NPITCH];	// pitch error, Q12, a ring of three frames
	uint8_t ring;				// E[ring] is the frame being analyzed
	uint8_t prev_v[8];			// last frame's voicing per 500 Hz band
	uint16_t prev_p2;			// last pitch, half samples
	uint16_t prev_e1, prev_e2;		// E at the chosen pitch, one and two frames back
	uint16_t e_tracked;			// E at the pitch just tracked, Q12 (the voicing decision reads it)
	uint32_t p8;				// the last frame's refined pitch, eighth samples (for tests)
	uint8_t full_depth;			// 1: no DVSI match (dstar_mbe_analysis.c): the magnitudes as measured, for
						// tests of the measurement. dstar_mbe_ana_init clears it
	uint8_t flat;				// 1: the DVSI match without DVSI's band balance (dvsi_eq_q12), for tests of
						// the codec as a transparent coder, as the research's MBE_DVSI_EQ=0 build.
						// dstar_mbe_ana_init clears it
} dstar_mbe_ana_t;

typedef struct {
	union {
		struct {
			int32_t re[256], im[256];	// the FFT
		} f;
		struct {
			int32_t r[301], d[301];		// the autocorrelation, before the FFT
		} a;
	} u;
	int32_t pw[129];			// |X|^2 >> 4 per bin
	int16_t x[301];				// pitch window samples
	int32_t g[DSTAR_MBE_NPITCH], ce[DSTAR_MBE_NPITCH];
} dstar_mbe_ana_scratch_t;

void dstar_mbe_ana_init(dstar_mbe_ana_t *s);

// Takes 160 new samples; *out receives the parameters of the frame three frames earlier (zero-signal parameters for the
// first two calls). *out may not be inside *t
void dstar_mbe_analyze(dstar_mbe_ana_t *s, dstar_mbe_ana_scratch_t *t, const int16_t pcm[DSTAR_MBE_FRAME],
		       dstar_ambe_params_t *out);

#endif
