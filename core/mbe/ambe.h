// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Half-rate vocoder of D-STAR voice (3600 x 2400 bit/s: 49 information bits and 23 of FEC in a 72-bit frame every
// 20 ms; docs/spec/dstar.md 5). The front end of the MBE core (mbe.h), as imbe.h is for the full-rate one: the frame's
// FEC, the dequantizer into mbe_params_t with its prediction from the last frame, and the inverse of both for the
// encoder. Not the AMBE+2 of DMR and NXDN: another quantizer, another FEC.
//
// A frame on air is 72 bits, sent as 9 bytes each least significant bit first (as every D-STAR field). Its bits are
// four code words: c0 a (24,12) Golay word, c1 a (23,12) Golay word whitened by a sequence seeded from c0's data, and
// c2 (11 bits) and c3 (14 bits) unprotected; ambe_ilv gives the order. The 49 information bits d0..d48 are c0's 12,
// c1's 12, c2's 11 and c3's 14, in that order.
#ifndef AMBE_H
#define AMBE_H

#include <stdint.h>
#include "mbe.h"

#define AMBE_BYTES		9	// a frame as on air
#define AMBE_DATA_BITS		49
#define AMBE_DATA_BYTES		7	// d0..d48, d0 in bit 7 of byte 0

// What the frame's two Golay words corrected, in one byte: the first word's (0..4, 4: an error it could only detect)
// and the second's (0..3). A frame is repeated rather than used when they corrected more than three bits together
#define AMBE_ERRS(c0, c1)	((uint8_t)((c0) | (c1) << 4))
#define AMBE_ERRS_C0(e)		((e) & 15)
#define AMBE_ERRS_C1(e)		(((e) >> 4) & 15)
#define AMBE_ERRS_TOTAL(e)	(AMBE_ERRS_C0(e) + AMBE_ERRS_C1(e))
#define AMBE_ERRS_LOST		0xFF	// no frame at all
#define AMBE_ERRS_REPEAT(e)	(AMBE_ERRS_TOTAL(e) > 3)

// the pitch code (7 bits): 0..119 speech, 120..123 an erasure, 124..125 silence, 126..127 a tone
enum { AMBE_FRAME_INVALID = -1, AMBE_FRAME_OK = 0, AMBE_FRAME_SILENCE = 1 };

// What the quantizer predicts the next frame from: 124 B
typedef struct {
	int16_t L;
	int16_t gamma;			// the gain, log2 Q11
	int16_t lg[MBE_MAXL + 2];	// log2 of the last frame's harmonic amplitudes, Q11
} ambe_state_t;

// the decoder's: that, and its view of the channel
typedef struct {
	ambe_state_t q;
	uint32_t err_rate;		// the running rate of corrected bits, Q24
} ambe_dec_state_t;

typedef struct {
	ambe_dec_state_t *s;		// kept
	mbe_params_t *p;		// scratch
	mbe_mem_t mbe;			// the synthesis core's memory
} ambe_mem_t;

// The frame's FEC: from the frame as on air to d0..d48 (returns AMBE_ERRS), and back
uint8_t ambe_fec_decode(const uint8_t air[AMBE_BYTES], uint8_t d[AMBE_DATA_BYTES]);
void ambe_fec_encode(const uint8_t d[AMBE_DATA_BYTES], uint8_t air[AMBE_BYTES]);

void ambe_dec_init(const ambe_mem_t *m);
// One frame as on air. A frame whose codes corrected too much (AMBE_ERRS_REPEAT) or that is an erasure repeats the
// last good one, up to MBE_REPEAT_MAX times, then the output is silent; it is silent too while the channel's rate of
// corrected bits is high. A silence or tone frame plays silence. Returns the number of bad frames in a row, 0 for a
// good one
int ambe_dec_frame(const ambe_mem_t *m, const uint8_t air[AMBE_BYTES], int16_t pcm[MBE_FRAME_LEN]);
// The same, from d0..d48 and what the FEC corrected (AMBE_ERRS, AMBE_ERRS_LOST for no frame)
int ambe_dec_data(const ambe_mem_t *m, const uint8_t d[AMBE_DATA_BYTES], uint8_t errs, int16_t pcm[MBE_FRAME_LEN]);
// The frame of silence a transmitter sends when it has no speech (pitch code 124), as on air
extern const uint8_t ambe_silence[AMBE_BYTES];

// The dequantizer alone: p from d0..d48 and the prediction state s, which it advances. Returns an AMBE_FRAME_* code;
// the state is left alone unless the frame is AMBE_FRAME_OK
int ambe_unpack(ambe_state_t *s, const uint8_t d[AMBE_DATA_BYTES], mbe_params_t *p);
void ambe_state_reset(ambe_state_t *s);

// The quantizer alone, the inverse of ambe_unpack. b0: the pitch code, 0..119; b1: the voicing codebook entry;
// p->a: the log2 model amplitudes (Q11, the scale ambe_unpack gives) of ambe_L[b0] harmonics. Writes d0..d48, advances
// s as the receiver's dequantizer will, and leaves in p the parameters the receiver reconstructs
void ambe_pack(ambe_state_t *s, int b0, int b1, mbe_params_t *p, uint8_t d[AMBE_DATA_BYTES]);

// The encoder: 160 samples of speech to a frame as on air. The estimator of the MBE core (mbe_est.h) finds the
// parameters; the scratch can be the decoder's while it is not decoding
struct mbe_est_state;
typedef struct {
	ambe_state_t *s;		// kept
	struct mbe_est_state *est;	// kept: mbe_est_state_t
	mbe_params_t *p;		// scratch
	int32_t *buf;			// scratch: MBE_BUF_WORDS
} ambe_enc_mem_t;
void ambe_enc_init(const ambe_enc_mem_t *m);
void ambe_enc_frame(const ambe_enc_mem_t *m, const int16_t pcm[MBE_FRAME_LEN], uint8_t air[AMBE_BYTES]);

#endif
