// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The MBE (multiband excitation) synthesis core: one 20 ms frame of model parameters in, 160 samples at 8 kHz out.
// A harmonic model of the voiced part (an oscillator per harmonic, the upper ones dispersed by the frame's share of
// noise), noise
// shaped per band for the unvoiced part (inverse FFT, overlap-add), spectral amplitude enhancement, and concealment of
// bad frames. Codec front ends (core/mbe/imbe.h, and others of the family) only fill mbe_params_t.
//
// Memory is placed by the caller (mbe_mem_t): the kept parts live as long as the call, the scratch parts only during
// mbe_synth_frame, so the scratch can be shared with other code. No heap, no float, no libm, no division in the
// per-sample loops. Sizes (32-bit target): state 516 B + previous frame 288 B kept; scratch: current frame 288 B and
// MBE_BUF_WORDS * 4 = 2048 B.
//
// Reference: TIA-102.BABA (MBE speech model: spectral amplitude enhancement, voiced and unvoiced synthesis).
#ifndef MBE_H
#define MBE_H

#include <stdint.h>

#define MBE_MAXL		56	// most harmonics in a frame
#define MBE_FRAME_LEN		160	// samples per frame (20 ms at 8 kHz)
#define MBE_BUF_WORDS		512	// scratch words: the noise FFT and the output sum
#define MBE_REPEAT_MAX		4	// bad frames in a row that repeat the last good one; the next one mutes

// A harmonic's amplitude: log2 in Q11 of its level in PCM units (a harmonic of 15 * 2048 is a full-scale sinusoid; an
// unvoiced one gets the same level as noise power in its band). MBE_A_SILENT marks a harmonic that is not there.
#define MBE_A_SILENT		(-32768)
// flags
#define MBE_NO_ENHANCE		0x0001	// skip the spectral amplitude enhancement (the front end already did it)

typedef struct {
	int16_t L;			// harmonics, 0..MBE_MAXL (0: a silent frame)
	uint16_t flags;
	uint32_t w0;			// fundamental as a phase step per sample, 2^32 = one turn (w0 = 2 pi f0 / 8000); limited to
					// 2^22 .. 2^29 - 1 (8 Hz .. 1 kHz)
	int16_t a[MBE_MAXL];		// harmonic l + 1: log2 amplitude, Q11
	uint8_t v[MBE_MAXL];		// harmonic l + 1: 1 voiced, 0 unvoiced
} mbe_params_t;

// a frame as synthesized: the parameters after enhancement, and each harmonic's phase offset
typedef struct {
	int16_t L;
	uint32_t w0;
	int16_t a[MBE_MAXL];		// log2 Q11, enhanced
	uint8_t v[MBE_MAXL];
	uint16_t th[MBE_MAXL];		// phase offset of harmonic l + 1 against l + 1 times the fundamental's, 2^16 = one turn
} mbe_frame_t;

// what is kept from frame to frame
typedef struct {
	uint32_t psi;			// phase of the fundamental at the end of the last frame
	uint32_t rng;			// noise seed, stepped every frame
	int16_t utail[MBE_FRAME_LEN];	// second half of the last noise window
	int32_t dc[2];			// output DC blocker: last input and output, Q4
	int16_t run, have_last;		// bad frames in a row; a good frame seen
	mbe_params_t last;		// the repeat source: the last good frame
} mbe_state_t;

typedef struct {
	mbe_state_t *s;			// kept
	mbe_frame_t *prev;		// kept: the last synthesized frame
	mbe_frame_t *cur;		// scratch
	int32_t *buf;			// scratch: MBE_BUF_WORDS
} mbe_mem_t;

void mbe_synth_init(const mbe_mem_t *m);
// The spectral amplitude enhancement alone, for a front end that has more to do between it and the synthesis (it
// then sets MBE_NO_ENHANCE). a, out: log2 Q11, L values; out may be a. The frame's energy is kept
void mbe_enhance(int16_t *out, const int16_t *a, int L, uint32_t w0);
// One frame. in: the parameters (NULL when bad). bad != 0: the front end could not use the frame; the last good one is
// repeated for up to MBE_REPEAT_MAX frames in a row, then the output fades to silence. Returns the number of bad frames
// in a row (0 for a good one): above MBE_REPEAT_MAX the front end drops what it predicts from.
int mbe_synth_frame(const mbe_mem_t *m, const mbe_params_t *in, int bad, int16_t pcm[MBE_FRAME_LEN]);

#endif
