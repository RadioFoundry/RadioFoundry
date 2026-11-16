// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Full-rate vocoder decoder (7200 x 4400 bit/s, as P25 Phase 1 voice carries it): the 88 information bits of a frame to
// 160 samples of speech. The front end of the MBE core (mbe.h): it dequantizes the frame into mbe_params_t and keeps the
// inter-frame prediction; the core does the synthesis and the concealment.
// Reference: TIA-102.BABA (the vocoder description, bit allocation and quantizer annexes).
#ifndef IMBE_H
#define IMBE_H

#include <stdint.h>
#include "mbe.h"

#define IMBE_BYTES	11	// the 88 information bits, first bit first: u0..u3 12 bits each, u4..u6 11 each, u7 7
#define IMBE_ERRS_MAX	5	// bits the frame's FEC corrected, above which the frame is not used (it is repeated)

// the pitch codes 208..215 and 220..255 are not used; 216..219 mark a silence frame
enum { IMBE_FRAME_INVALID = -1, IMBE_FRAME_OK = 0, IMBE_FRAME_SILENCE = 1 };

// what the dequantizer predicts the next frame's amplitudes from: 120 B
typedef struct {
	int16_t L;
	int16_t lg[MBE_MAXL + 2];	// log2 of the last frame's harmonic amplitudes, Q11, [0] = 0
} imbe_state_t;

typedef struct {
	imbe_state_t *s;		// kept
	mbe_params_t *p;		// scratch: the frame's parameters
	mbe_mem_t mbe;			// the synthesis core's memory
} imbe_mem_t;

void imbe_dec_init(const imbe_mem_t *m);
// One frame. imbe: the 88 bits after the frame's FEC and descrambling. errs: the bits that FEC corrected (more than
// IMBE_ERRS_MAX, or a pitch code that is not used: the last good frame is repeated up to MBE_REPEAT_MAX times, then the
// output is silent). Returns the number of bad frames in a row, 0 for a good one.
int imbe_dec_frame(const imbe_mem_t *m, const uint8_t imbe[IMBE_BYTES], uint8_t errs, int16_t pcm[MBE_FRAME_LEN]);
// A frame of silence (pitch code 216): what a transmitter sends when it has no speech, and what decodes as silence
extern const uint8_t imbe_silence[IMBE_BYTES];

// The dequantizer alone. p->L, p->w0, p->v and p->a (log2 of the model amplitude M_l, Q11, before the output gain of
// imbe_dec_frame) from a frame and the prediction state s, which it advances. Returns an IMBE_FRAME_* code; the state is
// left alone unless the frame is IMBE_FRAME_OK.
int imbe_unpack(imbe_state_t *s, const uint8_t imbe[IMBE_BYTES], mbe_params_t *p);
void imbe_state_reset(imbe_state_t *s);

// The quantizer alone, the inverse of imbe_unpack. b0: the pitch code, 0..207 (the pitch period is (b0 + 39.5) / 2
// samples). p->v and p->a hold the voicing and the log2 model amplitudes (Q11, the scale imbe_unpack gives) of the
// imbe_pitch_harmonics(b0) harmonics; a voicing band is three harmonics and takes its first one's. Writes the frame,
// advances s as the receiver's dequantizer will, and leaves in p the parameters the receiver reconstructs.
void imbe_pack(imbe_state_t *s, int b0, mbe_params_t *p, uint8_t out[IMBE_BYTES]);
int imbe_pitch_harmonics(int b0);

// The encoder: 160 samples of speech to a frame's 88 bits (the caller adds the frame's FEC). The estimator of the MBE
// core (mbe_est.h) finds the parameters; the scratch can be the decoder's while it is not decoding.
struct mbe_est_state;
typedef struct {
	imbe_state_t *s;		// kept
	struct mbe_est_state *est;	// kept: mbe_est_state_t
	mbe_params_t *p;		// scratch
	int32_t *buf;			// scratch: MBE_BUF_WORDS
} imbe_enc_mem_t;
void imbe_enc_init(const imbe_enc_mem_t *m);
void imbe_enc_frame(const imbe_enc_mem_t *m, const int16_t pcm[MBE_FRAME_LEN], uint8_t out[IMBE_BYTES]);

#endif
