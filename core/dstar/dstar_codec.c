// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The software D-STAR voice codec (dstar_codec.h)
#include "dstar/dstar_codec.h"
#include "dstar/dstar_ambe_frame.h"

#include <stddef.h>
#include <string.h>

_Static_assert(DSTAR_VOICE_SAMPLES == DSTAR_MBE_FRAME && DSTAR_VOICE_SAMPLES == DSTAR_MBE_SYNTH_FRAME,
	       "a voice frame is one MBE frame");

enum { DIR_NONE, DIR_ENCODE, DIR_DECODE };

// After a concealed frame, one with this many corrections in C0 and C1 together is concealed too (bad_rule)
#define DOUBTFUL_ERRORS	4u

void dstar_codec_bind(dstar_codec_t *c, dstar_codec_scratch_t *t)
{
	c->t = t;
	if (t == NULL) {
		c->dir = DIR_NONE;
	}
}

void dstar_codec_start(dstar_codec_t *c, bool encode)
{
	dstar_codec_scratch_t *t = c->t;
	memset(c, 0, sizeof(*c));
	c->t = t;
	c->errors = DSTAR_CODEC_FAILED;
	if (t == NULL) {
		return;
	}
	if (encode) {
		dstar_mbe_ana_init(&c->u.enc.ana);
		dstar_mbe_level_init(&c->u.enc.level);
		dstar_ambe_init(&c->u.enc.q);
		c->dir = DIR_ENCODE;
	} else {
		dstar_mbe_synth_init(&c->u.dec.synth);
		dstar_ambe_init(&c->u.dec.q);
		c->dir = DIR_DECODE;
	}
}

bool dstar_codec_encode(void *ctx, const int16_t pcm[DSTAR_VOICE_SAMPLES], uint8_t frame[DSTAR_VOICE_BYTES])
{
	dstar_codec_t *c = ctx;
	if (c == NULL || c->dir != DIR_ENCODE)
		return false;
	dstar_codec_enc_t *e = &c->u.enc;
	uint16_t b[DSTAR_AMBE_FIELDS];
	dstar_mbe_analyze(&e->ana, &c->t->ana, pcm, &e->p);
	// Before the quantizer, as in the research codec: the quantizer's gain index then codes the leveled frame, and
	// the frames stay bit for bit the research encoder's (dstar_mbe_level.c)
	dstar_mbe_level(&e->level, &e->p);
	dstar_ambe_quantize(&e->q, &c->t->q, &e->p, b, NULL);
	dstar_ambe_pack(b, frame);
	return true;
}

// A bad or missing frame: the synthesizer's concealment. When a long run of them ends in silence the synthesizer
// resets, and the parameter memory resets with it, as mbelib's does (mbe_initMbeParms), so the prediction restarts from
// the same state at both ends of a link that recovers. The prediction memory doesn't advance on a concealed frame, as
// in mbelib (it decodes, then puts the previous parameters back)
static void bad_frame(dstar_codec_dec_t *d, dstar_codec_scratch_t *t, int16_t pcm[DSTAR_VOICE_SAMPLES])
{
	dstar_mbe_synth_bad(&d->synth, &t->synth, pcm);
	if (!d->synth.have_last)
		dstar_ambe_init(&d->q);
	d->was_bad = 1;
}

// Whether a frame that arrived is concealed rather than decoded. mbelib conceals any frame with more than 3 corrections
// in C0 and C1 together, which throws away frames the Golay decoder got right: the research measured, on 12 inputs with
// three seeds each,
//   - at 5 % random bit errors mbelib's rule conceals 18 % of frames. Trusting the Golay decoder unless C0 fails conceals
//     2 %, and mean MCD falls from 29.2 to 25.8 dB, STOI rises from 0.742 to 0.796: a corrected frame with a few wrong
//     unprotected bits sounds better than a repeat
//   - concealing any word with 3 corrections, or a frame whose C1 failed, made random errors worse (27.7 and 26.0 dB at
//     5 %). So C1's failure alone conceals nothing, which also keeps the rule clear of the open question of whether
//     C1's parity bit is whitened (docs/spec/dstar-codec.md 5)
//   - in a fade (25 % bit errors for 80 ms on average) most frames are garbage that passes C0's check by chance. Once a
//     frame has been concealed, a following one with 4 or more corrections is concealed too: fades 27.9 -> 27.6 dB, STOI
//     0.762 -> 0.764, and random errors unchanged, since 4 corrections right after a loss are rare without a fade
// The research's receiver read C1 as the 23-bit Golay word without its parity bit, as mbelib does. A C1 that this tree's
// extended decoder reports failed has 4 errors, all in those 23 bits or three and the parity bit; the 23-bit decoder
// counts 3 for either, so 3 it is here
static bool bad_rule(const dstar_ambe_fec_t *fec, bool was_bad)
{
	if (fec->c0_failed)
		return true;
	unsigned n = fec->c0_errors + (fec->c1_failed ? 3u : fec->c1_errors);
	return was_bad && n >= DOUBTFUL_ERRORS;
}

bool dstar_codec_decode(void *ctx, const uint8_t frame[DSTAR_VOICE_BYTES], int16_t pcm[DSTAR_VOICE_SAMPLES])
{
	dstar_codec_t *c = ctx;
	if (c == NULL || c->dir != DIR_DECODE)
		return false;
	dstar_codec_dec_t *d = &c->u.dec;
	uint16_t b[DSTAR_AMBE_FIELDS];
	dstar_ambe_fec_t fec;
	dstar_ambe_unpack(frame, b, &fec);
	if (bad_rule(&fec, d->was_bad)) {
		c->errors = DSTAR_CODEC_FAILED;
		bad_frame(d, c->t, pcm);
		return true;
	}
	c->errors = (uint8_t)(fec.c0_errors + (fec.c1_failed ? 4u : fec.c1_errors));
	d->was_bad = 0;
	// A tone or reserved frame comes back with L = 0, which the synthesizer plays as silence and a reset
	dstar_ambe_dequantize(&d->q, &c->t->q, b, &d->p);
	dstar_mbe_synth_frame(&d->synth, &c->t->synth, &d->p, pcm);
	return true;
}

void dstar_codec_lost(dstar_codec_t *c, int16_t pcm[DSTAR_VOICE_SAMPLES])
{
	if (c->dir != DIR_DECODE) {
		memset(pcm, 0, DSTAR_VOICE_SAMPLES * sizeof(pcm[0]));
		return;
	}
	c->errors = DSTAR_CODEC_FAILED;
	bad_frame(&c->u.dec, c->t, pcm);
}

void dstar_codec_vocoder(dstar_codec_t *c, dstar_vocoder_t *v)
{
	v->encode = dstar_codec_encode;
	v->decode = dstar_codec_decode;
	v->ctx = c;
}

uint8_t dstar_codec_errors(const dstar_codec_t *c)
{
	return c->errors;
}
