// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The MBE core's parameter estimator: 20 ms of speech in, what a codec front end quantizes out. It finds the pitch,
// and for a pitch the front end settles on (its own code's value) the voicing of each band and every harmonic's
// amplitude. The model is TIA-102.BABA's (a windowed spectrum fitted harmonic by harmonic with the window's own
// transform); the pitch search, the window, the filters and the thresholds are this project's design.
//
// No heap, no float, no libm. Kept state 516 B; scratch MBE_BUF_WORDS words, shared with the synthesis core's.
#ifndef MBE_EST_H
#define MBE_EST_H

#include <stdint.h>
#include "mbe.h"

#define MBE_EST_WIN	221					// samples under the spectral window
#define MBE_EST_HIST	(MBE_EST_WIN - MBE_FRAME_LEN)
#define MBE_EST_LP	190					// low-passed samples at 4 kHz the pitch search keeps

typedef struct mbe_est_state {
	int16_t hist[MBE_EST_HIST];	// the last high-passed samples: with a new frame, one window
	int16_t lp[MBE_EST_LP];
	int32_t hp_y;			// the high-pass: its last output, Q4
	int16_t hp_x;			// and input
	uint16_t vprev;			// the last frame's voiced bands (bit k: band k)
	int16_t period4;		// the last frame's pitch period, quarter samples; 0: none
	int16_t pad;
} mbe_est_state_t;

// what mbe_est_frame found; the spectrum it belongs to is in the scratch until that is used for something else
typedef struct {
	int16_t exp;			// the stored spectrum is the true one times 2^exp
	int16_t period4;		// pitch period by the time-domain search, quarter samples; 0: none found
	int16_t periodic;		// log2 of the normalized autocorrelation at that period, Q11: 0 periodic, lower less so
	int16_t zero;			// 1: the frame is nothing but zeros (no spectrum either)
} mbe_est_t;

void mbe_est_init(mbe_est_state_t *s);
// One frame of speech: the high-passed window's spectrum is left in buf (MBE_BUF_WORDS words), the pitch period in e
void mbe_est_frame(mbe_est_state_t *s, const int16_t pcm[MBE_FRAME_LEN], int32_t *buf, mbe_est_t *e);
// How badly harmonics at the fundamental w0 (mbe_params_t.w0) fit the spectrum below about 3 kHz: the share of the
// energy they leave unexplained, Q15 (0: all harmonic). For choosing between pitch values close to e->period4
int mbe_est_mismatch(const int32_t *buf, uint32_t w0);
// The parameters at p->w0 for p->L harmonics: p->v, by K bands, and p->a, the log2 amplitudes in PCM units as
// mbe_synth_frame takes them (unvoiced ones as p->flags says: MBE_UV_DENSITY). A band is `per` harmonics, or with per
// 0 wb wide (as w0) from 0 Hz; the K-th takes the harmonics above (K at most 16). Remembers the voicing for the next
// frame's decisions
void mbe_est_bands(mbe_est_state_t *s, const int32_t *buf, const mbe_est_t *e, mbe_params_t *p, int per, uint32_t wb,
		   int K);
static inline void mbe_est_params(mbe_est_state_t *s, const int32_t *buf, const mbe_est_t *e, mbe_params_t *p, int per,
				  int K)
{
	mbe_est_bands(s, buf, e, p, per, 0, K);
}
static inline void mbe_est_params_hz(mbe_est_state_t *s, const int32_t *buf, const mbe_est_t *e, mbe_params_t *p,
				     int hz, int K)
{
	// 2^32 / 8000 = 536870.912 a Hz, exact for whole kHz (1 kHz = 2^29, the edge the dequantizers test)
	mbe_est_bands(s, buf, e, p, 0, (uint32_t)hz * 536870u + (uint32_t)hz * 912u / 1000u, K);
}

// A front end's pitch codes
typedef struct {
	int (*code)(int period4);	// the code nearest a period in quarter samples (may be outside 0..n - 1)
	uint32_t (*w0)(int code);	// a code's fundamental, as mbe_params_t.w0
	int16_t n;			// codes 0..n - 1
	int16_t min_period4;		// the shortest period a code stands for, quarter samples
} mbe_grid_t;

// The pitch code of the frame mbe_est_frame described in e and left in buf: the code nearest e->period4, or a
// neighbor of it the spectrum fits better, or the one of half the period when the spectrum fits that as well. Returns
// -1 for a frame with no pitch (not periodic enough; a frame after one without voiced bands needs more), with
// e->period4 set to 0; e->period4 is halved when half the period won
int mbe_est_pitch(const mbe_est_state_t *s, const int32_t *buf, mbe_est_t *e, const mbe_grid_t *g);

#endif
