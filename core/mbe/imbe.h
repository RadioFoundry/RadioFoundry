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
// What the frame's codes corrected, in one byte, as the decoder's rules need it: the bits corrected in all seven code
// words (0..15), those in the first (u0's Golay word, 0..3: the other words are descrambled with it) and that in the
// fifth (u4's Hamming word, 0 or 1)
#define IMBE_ERRS(total, c0, c4)	((uint8_t)((total) | (c0) << 4 | (c4) << 6))
#define IMBE_ERRS_TOTAL(e)	((e) & 15)
#define IMBE_ERRS_C0(e)		(((e) >> 4) & 3)
#define IMBE_ERRS_C4(e)		(((e) >> 6) & 1)
#define IMBE_ERRS_LOST		0xFF	// no frame at all
// A frame is not used, and the last one repeated, when its first word had two or more errors and the frame eleven or
// more: the descrambling was then most likely wrong, and the other words' "corrections" are what that looks like
#define IMBE_ERRS_REPEAT(e)	(IMBE_ERRS_C0(e) >= 2 && IMBE_ERRS_TOTAL(e) >= 11)

// the pitch codes 208..215 and 220..255 are not used; 216..219 mark a silence frame
enum { IMBE_FRAME_INVALID = -1, IMBE_FRAME_OK = 0, IMBE_FRAME_SILENCE = 1 };

// what the quantizer predicts the next frame's amplitudes from: 120 B
typedef struct {
	int16_t L;
	int16_t lg[MBE_MAXL + 2];	// log2 of the last frame's harmonic amplitudes, Q11, [0] = 0
} imbe_state_t;

// the decoder's: that, and its view of the channel: 132 B
typedef struct {
	imbe_state_t q;
	uint32_t err_rate;		// the running rate of corrected bits, Q24
	uint32_t energy;		// the running energy of the frames (sum of the squared model amplitudes)
	int32_t amp_max;		// what the sum of a frame's amplitudes may be
} imbe_dec_state_t;

typedef struct {
	imbe_dec_state_t *s;		// kept
	mbe_params_t *p;		// scratch: the frame's parameters
	mbe_mem_t mbe;			// the synthesis core's memory
} imbe_mem_t;

void imbe_dec_init(const imbe_mem_t *m);
// One frame. imbe: the 88 bits after the frame's FEC and descrambling. errs: what that FEC corrected (IMBE_ERRS). A frame
// that is not used (IMBE_ERRS_REPEAT, or a pitch code that is not used) repeats the last good one, up to
// MBE_REPEAT_MAX times, then the output is silent; it is silent too while the rate of corrected bits is over 8.5 %.
// Under errors the frame's voicing and amplitudes are smoothed. Returns the number of bad frames in a row, 0 for a
// good one.
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
