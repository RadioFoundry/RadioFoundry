// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
/*
 * The software D-STAR voice codec (docs/spec/dstar-codec.md): AMBE 3600x2400, 160 samples of 8 kHz speech <-> one
 * 72-bit voice frame, behind dstar_vocoder_t (dstar_voice.h). Integer only, no heap, no globals, no RTOS.
 *
 *   encode:  dstar_mbe_analyze -> dstar_mbe_level -> dstar_ambe_quantize -> dstar_ambe_pack
 *   decode:  dstar_ambe_unpack -> dstar_ambe_dequantize -> dstar_mbe_synth_frame (concealment on a bad frame)
 *
 * D-STAR is half duplex and a radio runs one mode at a time, so the encoder's and decoder's state share one union:
 * dstar_codec_start picks the direction about to run and the other's state is gone. All memory is the caller's
 * (docs/spec/dstar-codec.md 6), so a call needs little stack, in two parts: the state that lives from frame to frame
 * (dstar_codec_t, about 3 KB) and each call's working memory (dstar_codec_scratch_t, about 5 KB), which no call
 * leaves anything in. They are apart because a radio has them in different places: the scratch fits the DMR
 * vocoder's state RAM, which the radio lends while DMR is off (hal_vocoder_t.lend, 7 KB on the HR_C7000), and the
 * whole codec doesn't. The encoder's output lags its input by 60 ms (the analysis looks ahead two frames).
 */
#ifndef RFOUNDRY_DSTAR_CODEC_H
#define RFOUNDRY_DSTAR_CODEC_H

#include <stdbool.h>
#include <stdint.h>
#include "dstar/dstar_voice.h"
#include "dstar/dstar_ambe_quant.h"
#include "dstar/dstar_mbe_analysis.h"
#include "dstar/dstar_mbe_level.h"
#include "dstar/dstar_mbe_synth.h"

// dstar_codec_errors for a frame that was concealed: C0 failed, a doubtful frame after a loss, or one that never arrived
#define DSTAR_CODEC_FAILED	0xFFu

// Each call's working memory: the encoder's analysis, then its quantizer; the decoder's dequantizer, then its
// synthesizer. Nothing in it outlives the call that used it
typedef union {
	dstar_mbe_ana_scratch_t ana;
	dstar_ambe_scratch_t q;
	dstar_mbe_synth_scratch_t synth;
} dstar_codec_scratch_t;

typedef struct {
	dstar_mbe_ana_t ana;
	dstar_mbe_level_t level;		// the leveler between the analysis and the quantizer
	dstar_ambe_state_t q;			// the quantizer's copy of the decoder's prediction memory
	dstar_ambe_params_t p;			// the analysis's output, the quantizer's input
} dstar_codec_enc_t;

typedef struct {
	dstar_mbe_synth_t synth;
	dstar_ambe_state_t q;			// the prediction memory
	dstar_ambe_params_t p;			// the dequantizer's output, the synthesizer's input
	uint8_t was_bad;			// the previous frame was concealed (the bad-frame rule's memory)
} dstar_codec_dec_t;

typedef struct {
	union {
		dstar_codec_enc_t enc;
		dstar_codec_dec_t dec;
	} u;
	dstar_codec_scratch_t *t;	// dstar_codec_bind's; kept by dstar_codec_start
	uint8_t dir;		// which side u holds: 0 none, 1 encoder, 2 decoder
	uint8_t errors;		// dstar_codec_errors
} dstar_codec_t;

// The working memory c's calls use, from now until another bind; NULL leaves c unable to code (the calls return
// false). Leaves the direction as it was
void dstar_codec_bind(dstar_codec_t *c, dstar_codec_scratch_t *t);

// (Re)initializes the direction about to run, encode or decode; the other direction's state is gone, the bound
// working memory stays bound. Without working memory bound (dstar_codec_bind) the codec stays stopped
void dstar_codec_start(dstar_codec_t *c, bool encode);

// The dstar_vocoder_t functions; ctx is the dstar_codec_t. Each returns false, doing nothing, when the codec was
// started for the other direction (or not at all)
bool dstar_codec_encode(void *ctx, const int16_t pcm[DSTAR_VOICE_SAMPLES], uint8_t frame[DSTAR_VOICE_BYTES]);
// A frame is concealed (dstar_mbe_synth_bad: fading repeats, then comfort noise, then silence) when C0 failed, or when
// the previous frame was concealed and this one needed 4 or more corrections in C0 and C1; otherwise it is decoded,
// whatever C1 and the unprotected bits went through (docs/spec/dstar-codec.md 1)
bool dstar_codec_decode(void *ctx, const uint8_t frame[DSTAR_VOICE_BYTES], int16_t pcm[DSTAR_VOICE_SAMPLES]);

// A frame that never arrived: concealed as a bad one. Silence when the codec isn't decoding
void dstar_codec_lost(dstar_codec_t *c, int16_t pcm[DSTAR_VOICE_SAMPLES]);

// Fills the interface with this codec
void dstar_codec_vocoder(dstar_codec_t *c, dstar_vocoder_t *v);

// The last decoded frame's corrected bit errors in C0 and C1 (0..7: a C1 that failed, its frame played anyway, counts
// 4), or DSTAR_CODEC_FAILED when it was concealed, for the debug link and a bit error rate. The 24 unprotected bits are not
// counted
uint8_t dstar_codec_errors(const dstar_codec_t *c);

#endif
