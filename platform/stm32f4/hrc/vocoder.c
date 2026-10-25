// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The stock AMBE+2 codec run in place from flash (vocoder.h; the calls: docs/spec/codec-dm1701.md, "Codec state init
// and calls"). The code reaches its tables through absolute addresses, which the merge keeps; where it moves the code it
// rewrites what points into it. Without a valid descriptor and codec, present() is false and the radio is FM only.
//
// Not reentrant: one task (the audio task, through the audio block callback) makes all calls. The codec needs about
// 4 KB of the caller's stack (3.8 to 4.0 KB measured on the builds in use).
#include "hrc/vocoder.h"

#if CONFIG_DMR	// the whole file: DMR only
#include <string.h>
#include "codec_call.h"
#include "vocoder/ambe2_flags.h"

#define CODEC_SIGNATURE		0x4FF0E92Du	// push.w {r4-r11, lr}

// State sizes found by running the stock code with each block ending at a fault boundary: the codec touches
// D[0, 0x7EC), E[0, 0x16E0), its tables, its arguments and the stack, nothing else. The same in every build
#define DEC_STATE_BYTES		0x7ECu
#define ENC_STATE_BYTES		0x16E0u
#define FEC_STATE_BYTES		0x44u

#define HALF			(VOCODER_FRAME_SAMPLES / 2)

// CCM: CPU only, which is all the codec needs; not zeroed by the start-up code (reset() clears it)
static uint32_t dec_state[DEC_STATE_BYTES / 4] __attribute__((section(".ccmram"), aligned(8)));
static uint32_t enc_state[ENC_STATE_BYTES / 4] __attribute__((section(".ccmram"), aligned(8)));
static uint32_t fec_state[FEC_STATE_BYTES / 4] __attribute__((section(".ccmram"), aligned(8)));
static int16_t bitbuf[72] __attribute__((section(".ccmram"), aligned(4)));	// one bit per halfword
static int16_t pcmbuf[VOCODER_FRAME_SAMPLES] __attribute__((section(".ccmram"), aligned(4)));

static int8_t presence = -1;			// -1 not checked yet
static const stm32_codec_profile_t *prof;	// the build found
static uint32_t base;				// the code's start, from the descriptor
static bool ready;				// states initialized
static uint32_t enc_flags = AMBE2_ENC_DEFAULT;	// set_options (VOCODER_OPT_DEFAULT until then)

static uint32_t call(uint16_t off, uint32_t nargs, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t s0,
		     uint32_t s1, uint32_t s2, uint32_t s3)
{
	const uint32_t args[CODEC_CALL_MAX_ARGS] = { a0, a1, a2, a3, s0, s1, s2, s3 };
	return stm32_codec_call(base + off, args, nargs);
}

// One build's checks: the descriptor (its own checks, the build's tag, its placement and CRCs where the board pins
// them), then the code's signature word (an erased or zero region fails fast), then the CRC of every window
static bool matches(const codec_desc_t *d, const stm32_codec_profile_t *p)
{
	if (!codec_desc_check(d, p->tag, p->flash_lo, CODEC_DESC_ADDR, p->code_len) || d->code.len != p->code_len) {
		return false;
	}
	if ((p->code != 0 && d->code.addr != p->code) || (p->code_crc != 0 && d->code.crc != p->code_crc)) {
		return false;
	}
	if (p->tables != NULL && (d->windows != p->windows ||
				  memcmp(d->table, p->tables, p->windows * sizeof(codec_desc_window_t)) != 0)) {
		return false;
	}
	return *(const volatile uint32_t *)d->code.addr == CODEC_SIGNATURE &&
	       codec_desc_check_bytes(d, (const uint8_t *)p->flash_lo, p->flash_lo);
}

// Once (the window CRCs read about 100-140 KB of flash)
static bool present(void)
{
	if (presence < 0) {
		const codec_desc_t *d = (const codec_desc_t *)CODEC_DESC_ADDR;
		prof = NULL;
		for (uint8_t i = 0; i < stm32_codec_profiles.count && prof == NULL; i++) {
			if (matches(d, stm32_codec_profiles.list[i])) {
				prof = stm32_codec_profiles.list[i];
			}
		}
		base = prof != NULL ? d->code.addr : 0;
		presence = prof != NULL ? 1 : 0;
	}
	return presence == 1;
}

// The stock firmware's own init sequence, on our state blocks
static void reset(void)
{
	if (!present()) {
		return;
	}
	const stm32_codec_entries_t *at = &prof->at;
	const uint32_t D = (uint32_t)dec_state, E = (uint32_t)enc_state;
	memset(dec_state, 0, sizeof dec_state);	// stock keeps these in .bss: zero before init
	memset(enc_state, 0, sizeof enc_state);
	memset(fec_state, 0, sizeof fec_state);
	memset(bitbuf, 0, sizeof bitbuf);

	call(at->enc_init, 3, E, 1, 1, 0, 0, 0, 0, 0);
	call(at->fec_init, 2, (uint32_t)fec_state, 1, 0, 0, 0, 0, 0, 0);	// stock: the encoder side's FEC state
	call(at->dec_init, 2, D, 1, 0, 0, 0, 0, 0, 0);
	{
		const uint32_t args[CODEC_CALL_MAX_ARGS] = {
			E, (uint32_t)-22, 20, (uint32_t)-10, (uint32_t)-40, 0x4000, 3276, 3276, 1310, 10, 20,
		};
		stm32_codec_call(base + at->enc_params, args, 11);
	}
	call(at->enc_set, 3, E, 128, (uint32_t)-20, 0, 0, 0, 0, 0);
	call(at->dec_set, 3, D, 128, (uint32_t)-20, 0, 0, 0, 0, 0);
	// Last, as stock: one decode of an all-zero frame with flags 8
	call(at->decode, 7, (uint32_t)pcmbuf, HALF, (uint32_t)bitbuf, 0, 8, 0, D, 0);
	ready = true;
}

// Two calls per 20 ms frame, 80 samples each, half index 0 then 1. Both calls pass the same flags word, read once
// per frame (set_options; 0x1840 by default). r1 = 0 and the 0x2000 argument are what stock passes from its wrapper
// variables. The 49 parameter bits come back one per halfword.
static void encode(const int16_t pcm[VOCODER_FRAME_SAMPLES], uint8_t bits[VOCODER_BITS])
{
	if (!present()) {
		memset(bits, 0, VOCODER_BITS);
		return;
	}
	if (!ready) {
		reset();
	}
	memcpy(pcmbuf, pcm, sizeof pcmbuf);
	const uint32_t flags = enc_flags;
	for (uint32_t h = 0; h < 2; h++) {
		call(prof->at.encode, 8, (uint32_t)bitbuf, 0, (uint32_t)&pcmbuf[h * HALF], HALF, flags, h, 0x2000,
		     (uint32_t)enc_state);
	}
	for (int i = 0; i < VOCODER_BITS; i++) {
		bits[i] = (uint8_t)(bitbuf[i] & 1);
	}
}

// Two calls per frame on the same bit buffer (the codec rewrites it during the first call, as with stock). The flags
// word is the codec's frame quality input: 0 for a good frame; stock passes 4 when its FEC finds C0 uncorrectable
// (the codec then repeats the previous frame). hal_vocoder_t has no quality input, so the caller conceals bad frames
// (core/dmr/ambe/ambeVoice.c) and this always passes 0.
static void decode(const uint8_t bits[VOCODER_BITS], int16_t pcm[VOCODER_FRAME_SAMPLES])
{
	if (!present()) {
		memset(pcm, 0, VOCODER_FRAME_SAMPLES * sizeof(int16_t));
		return;
	}
	if (!ready) {
		reset();
	}
	for (int i = 0; i < VOCODER_BITS; i++) {
		bitbuf[i] = bits[i] & 1;
	}
	for (uint32_t h = 0; h < 2; h++) {
		call(prof->at.decode, 7, (uint32_t)&pcmbuf[h * HALF], HALF, (uint32_t)bitbuf, 0, 0, h,
		     (uint32_t)dec_state, 0);
	}
	memcpy(pcm, pcmbuf, sizeof pcmbuf);
}

// The id tools/rfoundry and the release manifests use for the build found
static const char *profile(void)
{
	return present() ? prof->id : NULL;
}

// Encode options (hal_vocoder_t): one flags word for every encode call, kept across reset(). The same library in
// every build these profiles accept, so the same bits
static uint32_t options(void)
{
	return AMBE2_OPTIONS;
}

static void set_options(uint32_t opts)
{
	enc_flags = ambe2_enc_flags(opts);
}

static uint32_t encode_flags(void)
{
	return enc_flags;
}

const hal_vocoder_t stm32_vocoder = {
	.present = present, .reset = reset, .encode = encode, .decode = decode, .profile = profile,
	.options = options, .set_options = set_options, .encode_flags = encode_flags,
};

#endif	// CONFIG_DMR
