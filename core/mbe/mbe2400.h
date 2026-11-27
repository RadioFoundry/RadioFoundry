// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// 2400 bit/s MBE front end (docs/spec/mbe-2400.md): 48 parameter bits a frame in a 72-bit frame with FEC (3600 x
// 2400 bit/s), compatible with D-STAR's voice frames. The quantizer, the dequantizer and the frame's bit map and FEC;
// the MBE core (mbe.h, mbe_est.h) does the analysis, the synthesis and the concealment.
#ifndef MBE2400_H
#define MBE2400_H

#include <stdint.h>
#include "mbe.h"

#define MBE2400_BYTES		9	// the 72-bit frame: bit a is bit a % 8 of byte a / 8, least significant bit first
#define MBE2400_FIELDS		9	// the parameter fields b0..b8 (7, 4, 6, 9, 7, 4, 4, 4, 4 bits; b8's lowest is not sent)
#define MBE2400_PITCHES		126	// pitch codes b0 0..125; 126 and 127 mark frames played as silence

// What the frame's FEC found, in one byte: the bits corrected in each of the two Golay words, 0..3, or 4 when a word
// had more errors than it corrects (detected)
#define MBE2400_ERRS(c0, c1)	((uint8_t)((c0) | (c1) << 3))
#define MBE2400_ERRS_C0(e)	((e) & 7)
#define MBE2400_ERRS_C1(e)	(((e) >> 3) & 7)
// A frame is not used, and the last one repeated, when its first word failed (the second word's descrambling and the
// pitch and gain bits are then unknown), or when the first word needed three corrections and the second failed
#define MBE2400_ERRS_BAD(e)	(MBE2400_ERRS_C0(e) > 3 || (MBE2400_ERRS_C0(e) == 3 && MBE2400_ERRS_C1(e) > 3))

enum { MBE2400_FRAME_OK = 0, MBE2400_FRAME_SILENCE = 1 };

// What the quantizer predicts the next frame from, the same at both ends: 124 B
typedef struct {
	mbe_pred_t q;			// the last frame's log amplitudes, model units; lg[0] = lg[1]
	int32_t gamma;			// the last frame's gain, log2 Q16
} mbe2400_state_t;

// the fields <-> the 72-bit frame. Unpacking corrects what the Golay words can and returns MBE2400_ERRS
void mbe2400_frame_pack(const uint16_t b[MBE2400_FIELDS], uint8_t out[MBE2400_BYTES]);
uint8_t mbe2400_frame_unpack(const uint8_t in[MBE2400_BYTES], uint16_t b[MBE2400_FIELDS]);

// A frame that decodes to near silence: what a transmitter sends when it has no speech (b0..b8 = 124 0 4 165 41 10 7 7 10)
extern const uint8_t mbe2400_silence[MBE2400_BYTES];

// pitch code b0 (0..125): its fundamental as mbe_params_t.w0, and its harmonic count (9..56)
uint32_t mbe2400_w0(int b0);
int mbe2400_pitch_harmonics(int b0);

void mbe2400_state_reset(mbe2400_state_t *s);
// The dequantizer: p->L, p->w0, p->v and p->a (log2 of the model amplitudes, Q11, before the output gain of
// mbe2400_dec_frame) from the fields and the state s, which it advances. b0 126 or 127 (another kind of frame, not
// decoded): MBE2400_FRAME_SILENCE, p->L 0 and s reset
int mbe2400_unpack(mbe2400_state_t *s, const uint16_t b[MBE2400_FIELDS], mbe_params_t *p);
// The quantizer, its inverse. b0: the pitch code (0..125). p->v and p->a hold the voicing and the log2 model amplitudes
// of mbe2400_pitch_harmonics(b0) harmonics. Writes the fields (the gain code so that the receiver's frame has p's
// energy), advances s as the receiver's dequantizer will, and leaves in p the parameters the receiver reconstructs
void mbe2400_pack(mbe2400_state_t *s, int b0, mbe_params_t *p, uint16_t b[MBE2400_FIELDS]);

// The decoder: kept state 124 B plus the synthesis core's
typedef struct {
	mbe2400_state_t *s;		// kept
	mbe_params_t *p;		// scratch
	mbe_mem_t mbe;			// the synthesis core's memory
} mbe2400_mem_t;
void mbe2400_dec_init(const mbe2400_mem_t *m);
// One frame: in is the 72-bit frame, NULL when none came. errs, when not NULL, gets what the FEC found
// (MBE2400_ERRS, 0xFF for no frame). A bad frame repeats the last good one, fading, up to three times; then the output
// is silent. Returns the number of bad frames in a row, 0 for a good one
int mbe2400_dec_frame(const mbe2400_mem_t *m, const uint8_t *in, uint8_t *errs, int16_t pcm[MBE_FRAME_LEN]);

// The encoder: 160 samples of speech to a 72-bit frame. The scratch can be the decoder's while it is not decoding
struct mbe_est_state;
typedef struct {
	mbe2400_state_t *s;		// kept
	struct mbe_est_state *est;	// kept: mbe_est_state_t
	mbe_params_t *p;		// scratch
	int32_t *buf;			// scratch: MBE_BUF_WORDS
} mbe2400_enc_mem_t;
void mbe2400_enc_init(const mbe2400_enc_mem_t *m);
void mbe2400_enc_frame(const mbe2400_enc_mem_t *m, const int16_t pcm[MBE_FRAME_LEN], uint8_t out[MBE2400_BYTES]);

#endif
