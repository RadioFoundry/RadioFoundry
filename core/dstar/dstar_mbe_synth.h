// SPDX-License-Identifier: MIT AND ISC
// Copyright (c) 2026 The RadioFoundry Authors
// SPDX-FileCopyrightText: 2026 mbe contributors
// SPDX-FileCopyrightText: 2010 mbelib Author (the synthesizer it ports, ISC license, see LICENSES/ISC.txt)
/*
 * Fixed-point MBE speech synthesizer for the D-STAR codec (docs/spec/dstar-codec.md 4): the quantizer's parameters
 * (dstar_ambe_quant.h) -> 160 samples of 8 kHz speech a 20 ms frame.
 *
 * A port to integer arithmetic of mbelib's mbe_spectralAmpEnhance and mbe_synthesizeSpeech (uvquality 3) (szechyjs/mbelib
 * 9a04ed5, ISC). It departs from mbelib in these places, all measured (docs/spec/dstar-codec.md 4):
 *   - no high-band random term: mbelib adds a uniform random value, with a non-zero mean, to every unvoiced band above
 *     2.7 kHz ("uvrand"), which puts a wandering DC offset and broadband noise into the output (about 1.4 dB of MCD)
 *   - the spectral enhancement runs on the transmitted magnitudes and the unvoiced 0.2046 / sqrt(w0) scale is applied
 *     after it. mbelib scales first, so its enhancer sees unvoiced bands as spectral valleys and cuts them (about
 *     0.1 dB of MCD)
 *   - concealment: a bad frame repeats the last good one a little quieter each time, then continues it as fading
 *     unvoiced noise, and only a long run mutes; mbelib repeats at full level three times, then cuts to silence
 *   - the spectral shape is expanded to 1/0.73 about each frame's mean, power kept: depth put back after the analyzer's
 *     DVSI match (dstar_mbe_analysis.c, 0.6) and a DVSI encoder's flatter analysis, measured on both (dstar_mbe_synth.c)
 *   - voiced harmonics whose pitch moved under 10 % are one sinusoid with interpolated amplitude and frequency (the
 *     IMBE interpolation mbelib leaves commented out) instead of a cross-fade
 *   - voiced phase is the minimum phase of the spectral envelope instead of re-randomized every frame
 *   - unvoiced bands are windowed noise from a 256-point inverse FFT instead of three fixed sines per band
 *   - a soft limiter instead of a hard clip
 *
 * Integer only, no heap, no globals: one state per stream.
 */
#ifndef RFOUNDRY_DSTAR_MBE_SYNTH_H
#define RFOUNDRY_DSTAR_MBE_SYNTH_H

#include <stdint.h>
#include "dstar/dstar_ambe_quant.h"

#define DSTAR_MBE_SYNTH_FRAME	160	// samples a frame
#define DSTAR_MBE_SYNTH_NFFT	256	// the unvoiced noise's inverse FFT
#define DSTAR_MBE_SYNTH_NTAIL	105	// noise samples a frame carries into the next (its fade-out)

typedef struct {
	uint32_t f0_q24;				// the previous frame's f0, Q24 cycles per sample
	uint8_t L;					// the previous frame's harmonic count
	uint8_t repeat;					// consecutive repeated (bad) frames
	uint8_t have_last;				// last holds a good frame
	uint32_t rng;					// noise generator state, never 0
	uint8_t voiced[DSTAR_AMBE_MAX_L + 1];		// the previous frame's voicing
	int32_t log2A[DSTAR_AMBE_MAX_L + 1];		// the previous enhanced amplitude, log2 PCM, Q12
	uint32_t psi[DSTAR_AMBE_MAX_L + 1];		// mbelib's PSIl, phase (2^32 = one cycle)
	uint32_t phi[DSTAR_AMBE_MAX_L + 1];		// mbelib's PHIl
	dstar_ambe_params_t last;			// the last good parameters, for repeats
	int32_t ntail[DSTAR_MBE_SYNTH_NTAIL];		// unvoiced noise carried into the next frame, PCM
} dstar_mbe_synth_t;

// Working memory for one call, dead between calls (about 4.6 KB that would otherwise be stack)
typedef struct {
	dstar_ambe_params_t ex;				// the frame with its shape expanded
	uint32_t theta[DSTAR_AMBE_MAX_L + 1];		// the envelope's phase per harmonic
	int32_t re[DSTAR_MBE_SYNTH_NFFT], im[DSTAR_MBE_SYNTH_NFFT];	// the noise's spectrum, then the noise
	uint8_t band[DSTAR_MBE_SYNTH_NFFT / 2];		// the harmonic each noise bin belongs to, 0 for none
	uint8_t ncnt[DSTAR_AMBE_MAX_L + 2];		// noise bins per harmonic
	int32_t nlm[DSTAR_AMBE_MAX_L + 1];		// a noise bin's log2 magnitude per harmonic
	int32_t nz[DSTAR_MBE_SYNTH_FRAME];		// the noise for this frame, PCM
	int32_t lm[DSTAR_AMBE_MAX_L + 1];		// the frame's log2 magnitudes, enhanced
	int32_t cA[DSTAR_AMBE_MAX_L + 1];		// the current frame's log2 amplitudes
	uint8_t cV[DSTAR_AMBE_MAX_L + 1];		// and voicing
	uint32_t pphi[DSTAR_AMBE_MAX_L + 1];		// the previous frame's phases
	int32_t acc[DSTAR_MBE_SYNTH_FRAME];		// the output before scaling
	dstar_ambe_params_t rep;			// a concealed frame's parameters
} dstar_mbe_synth_scratch_t;

// mbelib's mbe_initMbeParms
void dstar_mbe_synth_init(dstar_mbe_synth_t *s);

// A good frame. p->L == 0 (a tone or reserved frame, as dstar_ambe_dequantize reports it) mutes and resets, as mbelib
// does. *p may not be inside *t
void dstar_mbe_synth_frame(dstar_mbe_synth_t *s, dstar_mbe_synth_scratch_t *t, const dstar_ambe_params_t *p,
			   int16_t pcm[DSTAR_MBE_SYNTH_FRAME]);

// Concealment of a frame that failed its error check, or never arrived: the last good frame repeated 1 dB quieter each
// time for three frames, then its spectrum as unvoiced noise still fading 1 dB a frame, and after 28 bad frames in a
// row (0.56 s, 28 dB down) silence and a reset (dstar_mbe_synth.c)
void dstar_mbe_synth_bad(dstar_mbe_synth_t *s, dstar_mbe_synth_scratch_t *t, int16_t pcm[DSTAR_MBE_SYNTH_FRAME]);

// Silence now and reset, e.g. at the end of a transmission
void dstar_mbe_synth_mute(dstar_mbe_synth_t *s, int16_t pcm[DSTAR_MBE_SYNTH_FRAME]);

#endif
