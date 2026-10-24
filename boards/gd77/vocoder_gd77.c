// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// GD-77 vocoder: the radio's own stock AMBE+2 codec, run in place from flash (docs/spec/codec-gd77.md). The codec is the
// one the other HR_C6000 boards carry and runs through the same calls; the audio goes over I2S and the C6000 only frames the voice
// (docs/spec/hardware-gd77.md 5).
//
// The merge on the user's machine (tools/odrflash/odrflash/k22fcodec.py) writes the codec's code at CODE_BASE, its table
// window at the build's own address, and the descriptor at 0x7B700 (platform/k22f/codec_desc.h). The code reaches its
// tables through absolute addresses, which don't change; the merge rewrites what does (a few code pointers and the
// branches between the parts). None of it is part of this repository or a release. Without a valid descriptor and codec,
// present() is false and the radio is FM only.
//
// Not reentrant: one task makes all the calls. The codec needs about 4 KB of the caller's stack (3.8 KB measured).
#include "gd77.h"
#include "config.h"

#if CONFIG_DMR	// the whole file: DMR only
#include "codec_call.h"
#include "codec_desc.h"

#include <string.h>

#define CODEC_SIGNATURE		0x4FF0E92Du	// push.w {r4-r11, lr}
#define CODEC_CODE		0x00055D00u	// where the merge puts the code (memory.ld ends the application here)
#define CODEC_FLASH_LO		CODEC_CODE
#define CODEC_FLASH_HI		CODEC_DESC_ADDR

// Entry points (offsets from the code start): the same for every stock build
#define CODEC_ENCODE		(base + 0x130u)
#define CODEC_DECODE		(base + 0x5D8u)
#define CODEC_ENC_INIT		(base + 0x284u)	// (E, 1, 1)
#define CODEC_FEC_INIT		(base + 0x4CCu)	// (F, 1): state of the codec's own FEC routines
#define CODEC_DEC_INIT		(base + 0x6D0u)	// (D, 1)
#define CODEC_ENC_PARAMS	(base + 0x848u)	// (E, 10 parameters)
#define CODEC_ENC_SET		(base + 0x1C0u)	// (E, a, b)
#define CODEC_DEC_SET		(base + 0x688u)	// (D, a, b)
// Not used: encode ECC +0x4E8 and FEC decode +0xB54. The air frame FEC is ours (core/dmr/ambe) and matches stock's.

// State sizes found by running the stock code with each block ending at a fault boundary: the codec touches
// D[0, 0x7EC), E[0, 0x16E0), its tables, its arguments and the stack, nothing else
#define DEC_STATE_BYTES		0x7ECu
#define ENC_STATE_BYTES		0x16E0u
#define FEC_STATE_BYTES		0x44u

#define HALF			(VOCODER_FRAME_SAMPLES / 2)

// One supported stock build as the merge places it (each pack's "placed" in tools/odrflash/odrflash/codec_packs/gd77/,
// tools/odrflash/odrflash/k22fcodec.py): the descriptor's tag, the code region's length and CRC-32 (IEEE 802.3) after
// the rewrites, and the table window with its CRC. The descriptor must carry exactly these
typedef struct {
	uint32_t tag, code_len, code_crc, tab_addr, tab_len, tab_crc;
} gd77_codec_profile_t;

static const gd77_codec_profile_t profiles[] = {
	{ 0x3201u, 0x11E90u, 0xBC78498Bu, 0x00067C58u, 78891u, 0xE81AFF5Du },	// GD-77 V3.2.1
	{ 0x3202u, 0x11E90u, 0xDBBA763Du, 0x00067C70u, 78867u, 0x4051B25Au },	// V3.2.2
	{ 0x4208u, 0x11E90u, 0x1CA7B3D4u, 0x00068134u, 77647u, 0xAF1FEB2Eu },	// V4.2.8
	{ 0x4303u, 0x11B58u, 0xC5EB3113u, 0x00068268u, 77339u, 0xB9AE6A44u },	// V4.3.3
	{ 0x4306u, 0x11B58u, 0x8BBE7F89u, 0x000684C8u, 78128u, 0xDCEC0213u },	// V4.3.6
	{ 0x4309u, 0x11B58u, 0x9F4B97ACu, 0x000684D0u, 78120u, 0xC59EC61Fu },	// V4.3.9
	{ 0x6001u, 0x11B58u, 0x28E1981Du, 0x00068480u, 78200u, 0xBBC2BCA7u },	// V6.0.1 (Part 90)
};

// CCM-less part: ordinary SRAM, not zeroed by start-up for the states (reset() clears them)
static uint32_t dec_state[DEC_STATE_BYTES / 4] __attribute__((aligned(8)));
static uint32_t enc_state[ENC_STATE_BYTES / 4] __attribute__((aligned(8)));
static uint32_t fec_state[FEC_STATE_BYTES / 4] __attribute__((aligned(8)));
static int16_t bitbuf[72] __attribute__((aligned(4)));	// one bit per halfword
static int16_t pcmbuf[VOCODER_FRAME_SAMPLES] __attribute__((aligned(4)));

static int8_t presence = -1;	// -1 not checked yet
static uint32_t base;		// the code's start, from the descriptor
static bool ready;		// states initialized
static bool enc_first;		// next encode is the first of a transmission

static uint32_t call(uint32_t fn, uint32_t nargs, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t s0,
		     uint32_t s1, uint32_t s2, uint32_t s3)
{
	const uint32_t args[CODEC_CALL_MAX_ARGS] = { a0, a1, a2, a3, s0, s1, s2, s3 };
	return k22f_codec_call(fn, args, nargs);
}

// The descriptor (its own checks, one of the supported builds at its placement and CRCs), then the code's signature
// word (an erased or zero region fails fast), then the CRC of both windows, once (about 150 KB)
static bool present(void)
{
	if (presence < 0) {
		const codec_desc_t *d = (const codec_desc_t *)CODEC_DESC_ADDR;
		bool ok = false;
		for (unsigned i = 0; i < sizeof profiles / sizeof profiles[0] && !ok; i++) {
			const gd77_codec_profile_t *p = &profiles[i];
			ok = codec_desc_check(d, p->tag, CODEC_FLASH_LO, CODEC_FLASH_HI, p->code_len) &&
			     d->code.addr == CODEC_CODE && d->code.len == p->code_len && d->code.crc == p->code_crc &&
			     d->windows == 1 && d->table[0].addr == p->tab_addr && d->table[0].len == p->tab_len &&
			     d->table[0].crc == p->tab_crc &&
			     *(const volatile uint32_t *)d->code.addr == CODEC_SIGNATURE &&
			     codec_desc_check_bytes(d, (const uint8_t *)CODEC_FLASH_LO, CODEC_FLASH_LO);
		}
		base = ok ? d->code.addr : 0;
		presence = ok ? 1 : 0;
	}
	return presence == 1;
}

// The stock firmware's own init sequence (the same on the other HR_C6000 boards), on our state blocks
static void reset(void)
{
	if (!present()) {
		return;
	}
	const uint32_t D = (uint32_t)dec_state, E = (uint32_t)enc_state;
	memset(dec_state, 0, sizeof dec_state);	// stock keeps these in .bss: zero before init
	memset(enc_state, 0, sizeof enc_state);
	memset(fec_state, 0, sizeof fec_state);
	memset(bitbuf, 0, sizeof bitbuf);

	call(CODEC_ENC_INIT, 3, E, 1, 1, 0, 0, 0, 0, 0);
	call(CODEC_FEC_INIT, 2, (uint32_t)fec_state, 1, 0, 0, 0, 0, 0, 0);	// stock: the encoder side's FEC state
	call(CODEC_DEC_INIT, 2, D, 1, 0, 0, 0, 0, 0, 0);
	{
		const uint32_t args[CODEC_CALL_MAX_ARGS] = {
			E, (uint32_t)-22, 20, (uint32_t)-10, (uint32_t)-40, 0x4000, 3276, 3276, 1310, 10, 20,
		};
		k22f_codec_call(CODEC_ENC_PARAMS, args, 11);
	}
	call(CODEC_ENC_SET, 3, E, 128, (uint32_t)-20, 0, 0, 0, 0, 0);
	call(CODEC_DEC_SET, 3, D, 128, (uint32_t)-20, 0, 0, 0, 0, 0);
	// Last, as stock: one decode of an all-zero frame with flags 8
	call(CODEC_DECODE, 7, (uint32_t)pcmbuf, HALF, (uint32_t)bitbuf, 0, 8, 0, D, 0);
	enc_first = true;
	ready = true;
}

// Two calls per 20 ms frame, 80 samples each, half index 0 then 1. Flags 0x1840 on the first call after reset (stock
// uses its init value 0x1840 for the first call of a transmission), 0x800 after that. The 49 parameter bits come back one
// per halfword.
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
	for (uint32_t h = 0; h < 2; h++) {
		call(CODEC_ENCODE, 8, (uint32_t)bitbuf, 0, (uint32_t)&pcmbuf[h * HALF], HALF, enc_first ? 0x1840u : 0x800u,
		     h, 0x2000, (uint32_t)enc_state);
		enc_first = false;
	}
	for (int i = 0; i < VOCODER_BITS; i++) {
		bits[i] = (uint8_t)(bitbuf[i] & 1);
	}
}

// Two calls per frame on the same bit buffer (the codec rewrites it during the first call, as with stock). The flags
// word is the codec's frame quality input: 0 for a good frame; the caller conceals bad frames
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
		call(CODEC_DECODE, 7, (uint32_t)&pcmbuf[h * HALF], HALF, (uint32_t)bitbuf, 0, 0, h, (uint32_t)dec_state, 0);
	}
	memcpy(pcm, pcmbuf, sizeof pcmbuf);
}

// The id tools/odrflash and the release manifests use for this codec: a supported build placed as above (the descriptor
// and both CRCs matched)
static const char *profile(void)
{
	return present() ? "gd77-p1" : NULL;
}

const hal_vocoder_t gd77_vocoder = {
	.present = present, .reset = reset, .encode = encode, .decode = decode, .profile = profile,
};

#endif	// CONFIG_DMR
