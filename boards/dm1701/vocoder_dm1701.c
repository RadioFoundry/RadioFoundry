// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// DM-1701 vocoder: the stock V02.06 AMBE+2 codec run in place from flash (docs/spec/codec-placement.md, "STM32 radios";
// the codec: docs/spec/codec-dm1701.md, "Codec state init and calls (stock V02.06)").
//
// The merge on the user's machine (odrflash, tools/codec_extract.py, the web flasher) writes the codec's tables at their
// stock address (0x080D19F0..0x080E3A72, the codec's own data), its code right after them at 0x080E3A74, and the
// descriptor at the end of sector 11 (platform/stm32f4/codec_desc.h). The code reaches its tables through absolute
// addresses, which don't change. The merge rewrites what does: one pointer into the code and the 12 calls from the code
// that stock keeps in the table window back into the code (the codec map). None of it is part of this repository or a
// release. The calls and the checks are the shared ones (platform/stm32f4/hrc/vocoder.c); this file says which build and
// where. Without a valid descriptor and codec, present() is false and the radio is FM only.
#include "dm1701.h"
#include "config.h"

#if CONFIG_DMR	// the whole file: DMR only
#include "codec_call.h"
#include "vocoder/ambe2_flags.h"
#include "codec_desc.h"

#define CODEC_TAG		0x0206u		// the descriptor's tag for stock V02.06 CSV
#define CODEC_FLASH_LO		0x080C0000u	// sectors 10 and 11: the flash dm1701.ld leaves to the codec

// The V02.06 CSV codec as the merge places it (tools/odrflash/odrflash/stockfw.py, "placed"): CRC-32 (IEEE 802.3) of
// the code and tables after the rewrites. The descriptor must carry exactly these
#define CODEC_CODE		0x080E3A74u
#define CODEC_CODE_LEN		0x1021Cu
#define CODEC_CRC		0x485BFC13u
#define CODEC_TABLES		0x080D19F0u
#define CODEC_TABLES_LEN	0x12082u
#define CODEC_TABLES_CRC	0xC1EEFA8Bu

#define HALF			(VOCODER_FRAME_SAMPLES / 2)

// CCM: CPU only, which is all the codec needs; not zeroed by the start-up code (reset() clears it)
static uint32_t dec_state[DEC_STATE_BYTES / 4] __attribute__((section(".ccmram"), aligned(8)));
static uint32_t enc_state[ENC_STATE_BYTES / 4] __attribute__((section(".ccmram"), aligned(8)));
static uint32_t fec_state[FEC_STATE_BYTES / 4] __attribute__((section(".ccmram"), aligned(8)));
static int16_t bitbuf[72] __attribute__((section(".ccmram"), aligned(4)));	// one bit per halfword
static int16_t pcmbuf[VOCODER_FRAME_SAMPLES] __attribute__((section(".ccmram"), aligned(4)));

static int8_t presence = -1;	// -1 not checked yet
static uint32_t base;		// the code's start, from the descriptor
static bool ready;		// states initialized
static uint32_t enc_flags = AMBE2_ENC_DEFAULT;	// set_options (VOCODER_OPT_DEFAULT until then)

static uint32_t call(uint32_t fn, uint32_t nargs, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t s0,
		     uint32_t s1, uint32_t s2, uint32_t s3)
{
	const uint32_t args[CODEC_CALL_MAX_ARGS] = { a0, a1, a2, a3, s0, s1, s2, s3 };
	return stm32_codec_call(fn, args, nargs);
}

// The descriptor (its own checks, the V02.06 build at its placement and CRCs), then the
// code's signature word (an erased or zero region fails fast), then the CRC of both windows, once (about 140 KB)
static bool present(void)
{
	if (presence < 0) {
		const codec_desc_t *d = (const codec_desc_t *)CODEC_DESC_ADDR;
		bool ok = codec_desc_check(d, CODEC_TAG, CODEC_FLASH_LO, CODEC_FLASH_HI, CODEC_CODE_LEN) &&
			  d->code.addr == CODEC_CODE && d->code.len == CODEC_CODE_LEN && d->code.crc == CODEC_CRC &&
			  d->windows == 1 &&
			  d->table[0].addr == CODEC_TABLES && d->table[0].len == CODEC_TABLES_LEN &&
			  d->table[0].crc == CODEC_TABLES_CRC &&
			  *(const volatile uint32_t *)d->code.addr == CODEC_SIGNATURE &&
			  codec_desc_check_bytes(d, (const uint8_t *)CODEC_FLASH_LO, CODEC_FLASH_LO);
		base = ok ? d->code.addr : 0;
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
		call(CODEC_ENCODE, 8, (uint32_t)bitbuf, 0, (uint32_t)&pcmbuf[h * HALF], HALF, flags,
		     h, 0x2000, (uint32_t)enc_state);
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

// The id tools/odrflash and the release manifests use for this codec: V02.06 CSV placed as above (the descriptor and
// both CRCs matched). A build from before the move reports "dm1701-v0206-csv", which tells odrflash that the codec
// isn't where this layout keeps it
static const char *profile(void)
{
	return present() ? "dm1701-v0206-csv-s11" : NULL;
}

// Encode options (hal_vocoder_t): one flags word for every encode call, kept across reset()
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

const hal_vocoder_t dm1701_vocoder = {
	.present = present, .reset = reset, .encode = encode, .decode = decode, .profile = profile,
	.options = options, .set_options = set_options, .encode_flags = encode_flags,
};

static const stm32_codec_profile_t *const profiles[] = { &v0206 };
const stm32_codec_profiles_t stm32_codec_profiles = { profiles, 1 };

#endif	// CONFIG_DMR
