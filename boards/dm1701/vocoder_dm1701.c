// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// DM-1701 vocoder: the stock V02.06 AMBE+2 codec run in place from flash (docs/spec/codec-dm1701.md, sections
// "Recommended hosting" and "Codec state init and calls (stock V02.06)").
//
// The codec code (0x08074714..0x08084930) and its tables (0x080D19F0..0x080E4240) are merged into the image on the
// user's machine (tools/codec_extract.py); they are never part of this repository or a release. Without them,
// present() is false and the radio is FM only.
//
// Not reentrant: one task (the audio task, through the audio block callback) makes all calls. The codec needs about
// 4 KB of the caller's stack (3.8 KB measured).
#include "dm1701.h"
#include "config.h"

#if CONFIG_DMR	// the whole file: DMR only
#include "codec_call.h"

#include <string.h>

#define CODEC_BASE		0x08074714u
#define CODEC_END		0x08084930u
#define CODEC_TABLES		0x080D19F0u
#define CODEC_TABLES_END	0x080E4240u
#define CODEC_SIGNATURE		0x4FF0E92Du	// push.w {r4-r11, lr}

// Entry points (offsets from CODEC_BASE)
#define CODEC_ENCODE		(CODEC_BASE + 0x130u)
#define CODEC_DECODE		(CODEC_BASE + 0x5D8u)
#define CODEC_ENC_INIT		(CODEC_BASE + 0x284u)	// (E, 1, 1)
#define CODEC_FEC_INIT		(CODEC_BASE + 0x4CCu)	// (F, 1): state of the codec's own FEC routines
#define CODEC_DEC_INIT		(CODEC_BASE + 0x6D0u)	// (D, 1)
#define CODEC_ENC_PARAMS	(CODEC_BASE + 0x848u)	// (E, 10 parameters)
#define CODEC_ENC_SET		(CODEC_BASE + 0x1C0u)	// (E, a, b)
#define CODEC_DEC_SET		(CODEC_BASE + 0x688u)	// (D, a, b)
// Not used: encode ECC +0x4E8 and FEC decode +0xB54. The air frame FEC is ours (core/dmr/ambe) and matches stock's.

// State sizes found by running the stock code with each block ending at a fault boundary: the codec touches
// D[0, 0x7EC), E[0, 0x16E0), its tables, its arguments and the stack, nothing else
#define DEC_STATE_BYTES		0x7ECu
#define ENC_STATE_BYTES		0x16E0u
#define FEC_STATE_BYTES		0x44u

// CRC-32 (IEEE 802.3) of the decrypted windows of the stock V02.06 CSV file (their SHA-256 are in the spec)
#define CODEC_CRC		0x42CF501Du
#define CODEC_TABLES_CRC	0x673D0006u

#define HALF			(VOCODER_FRAME_SAMPLES / 2)

// CCM: CPU only, which is all the codec needs; not zeroed by the start-up code (reset() clears it)
static uint32_t dec_state[DEC_STATE_BYTES / 4] __attribute__((section(".ccmram"), aligned(8)));
static uint32_t enc_state[ENC_STATE_BYTES / 4] __attribute__((section(".ccmram"), aligned(8)));
static uint32_t fec_state[FEC_STATE_BYTES / 4] __attribute__((section(".ccmram"), aligned(8)));
static int16_t bitbuf[72] __attribute__((section(".ccmram"), aligned(4)));	// one bit per halfword
static int16_t pcmbuf[VOCODER_FRAME_SAMPLES] __attribute__((section(".ccmram"), aligned(4)));

static int8_t presence = -1;	// -1 not checked yet
static bool ready;		// states initialized
static bool enc_first;		// next encode is the first of a transmission

static uint32_t call(uint32_t fn, uint32_t nargs, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t s0,
		     uint32_t s1, uint32_t s2, uint32_t s3)
{
	const uint32_t args[CODEC_CALL_MAX_ARGS] = { a0, a1, a2, a3, s0, s1, s2, s3 };
	return stm32_codec_call(fn, args, nargs);
}

static uint32_t crc32(const uint8_t *p, uint32_t len)
{
	static const uint32_t nib[16] = {
		0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
		0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C, 0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C,
	};
	uint32_t c = 0xFFFFFFFFu;
	while (len--) {
		c ^= *p++;
		c = (c >> 4) ^ nib[c & 15];
		c = (c >> 4) ^ nib[c & 15];
	}
	return ~c;
}

// Signature word first (an erased or zero region fails fast), then the CRC of both windows, once (about 140 KB)
static bool present(void)
{
	if (presence < 0) {
		bool ok = *(const volatile uint32_t *)CODEC_BASE == CODEC_SIGNATURE &&
			  crc32((const uint8_t *)CODEC_BASE, CODEC_END - CODEC_BASE) == CODEC_CRC &&
			  crc32((const uint8_t *)CODEC_TABLES, CODEC_TABLES_END - CODEC_TABLES) == CODEC_TABLES_CRC;
		presence = ok ? 1 : 0;
	}
	return presence == 1;
}

// The stock firmware's own init sequence (V02.06, function at 0x08068D4C), on our state blocks
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
		stm32_codec_call(CODEC_ENC_PARAMS, args, 11);
	}
	call(CODEC_ENC_SET, 3, E, 128, (uint32_t)-20, 0, 0, 0, 0, 0);
	call(CODEC_DEC_SET, 3, D, 128, (uint32_t)-20, 0, 0, 0, 0, 0);
	// Last, as stock: one decode of an all-zero frame with flags 8
	call(CODEC_DECODE, 7, (uint32_t)pcmbuf, HALF, (uint32_t)bitbuf, 0, 8, 0, D, 0);
	enc_first = true;
	ready = true;
}

// Two calls per 20 ms frame, 80 samples each, half index 0 then 1. Flags 0x1840 on the first call after reset (stock
// uses its init value 0x1840 for the first call of a transmission), 0x800 after that. r1 = 0 and the 0x2000 argument
// are what stock passes from its wrapper variables. The 49 parameter bits come back one per halfword.
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
		call(CODEC_DECODE, 7, (uint32_t)&pcmbuf[h * HALF], HALF, (uint32_t)bitbuf, 0, 0, h, (uint32_t)dec_state, 0);
	}
	memcpy(pcm, pcmbuf, sizeof pcmbuf);
}

// The id tools/odrflash and the release manifests use for this codec (signature and both CRCs above matched)
static const char *profile(void)
{
	return present() ? "dm1701-v0206-csv" : NULL;
}

const hal_vocoder_t dm1701_vocoder = {
	.present = present, .reset = reset, .encode = encode, .decode = decode, .profile = profile,
};

#endif	// CONFIG_DMR
