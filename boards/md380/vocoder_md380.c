// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// MD-380 vocoder: the radio's own AMBE+2 codec run in place from flash (docs/spec/codec-placement.md, "STM32
// radios"; the codec: docs/spec/codec-md380.md, "State init and calls").
//
// The merge on the user's machine (rfoundry, tools/codec_extract.py, the web flasher) writes the codec's main table
// window at its stock address (0x0808FC64..0x0809AC5C, sector 8) and its four small table windows at theirs (sectors
// 10 and 11), its code at the start of sector 11 (0x080E0000), and the descriptor at the end of sector 11
// (platform/stm32f4/codec_desc.h). The code reaches its tables through absolute addresses, which don't change. The
// merge rewrites what does: two address pairs in the code that point into the code, and the 12 calls from the code
// that stock keeps in the table window back into the code (the codec map). None of it is part of this repository or a
// release. The 35.14 builds (MD-380 D35.14, MD-390 S35.14) carry the UV380 family's codec instead, with three table
// windows: the main one, from sector 9 into sector 10, stays there, the code goes right after it, the two small ones
// stay in sector 11. The calls and the checks are the shared ones (platform/stm32f4/hrc/vocoder.c); this file says
// which build and where. Without a valid descriptor and codec, present() is false and the radio is FM only.
#include "md380.h"
#include "config.h"

#if CONFIG_DMR	// the whole file: DMR only
#include "hrc/vocoder.h"

#define CODEC_TAG		0x2032u		// the descriptor's tag for stock D002.032
#define CODEC_FLASH_LO		0x08080000u	// sectors 8-11: the flash boards/md380/memory.ld leaves to the codec

// The D002.032 codec as the merge places it (tools/rfoundry/rfoundry/stockfw.py, "placed"): CRC-32 (IEEE 802.3) of
// the code and the main table window after the rewrites, and of the small windows (unchanged). The descriptor must
// carry exactly these
#define CODEC_CODE		0x080E0000u
#define CODEC_CODE_LEN		0x10884u
#define CODEC_CRC		0x067E6A77u
#define CODEC_WINDOWS		5u

static const codec_desc_window_t tables[CODEC_WINDOWS] = {
	{ 0x0808FC64u, 0xAFF8u, 0x2A5C9EE1u },
	{ 0x080D0FC8u, 0x24u, 0x7AAFE1D3u },
	{ 0x080F9698u, 0x18u, 0x33E82E70u },
	{ 0x080F9E74u, 0x14u, 0xFFF0E930u },
	{ 0x080FD490u, 0x20u, 0xDA18C5EFu },
};

// The id tools/rfoundry and the release manifests use for a codec placed as these profiles say (the descriptor and
// every CRC matched; stockfw.py MD380_CODEC): one for every build, named after D002.032, the first. The radio keeps
// its codec across updates going forward only: a release older than the one that added a build's profile doesn't
// know its tag and finds no codec. A build from before the move reports "md380-d2032", which tells rfoundry that the
// codec isn't where this layout keeps it
#define CODEC_ID		"md380-d2032-s11"

static const stm32_codec_profile_t d2032 = {
	.tag = CODEC_TAG, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = CODEC_CODE, .code_len = CODEC_CODE_LEN, .code_crc = CODEC_CRC, .tables = tables, .windows = CODEC_WINDOWS,
	// Not used: encode ECC +0x514, FEC decode +0xAC8. Our air frame FEC (core/dmr/ambe) matches stock's
	.at = { .encode = 0x14C, .decode = 0x604, .enc_init = 0x294, .fec_init = 0x4FC, .dec_init = 0x6F0,
		.enc_params = 0x854, .enc_set = 0x1E0, .dec_set = 0x6AC },
};

// The 35.14 builds as the merge places them (stockfw.py, "placed"); the entry offsets are the UV380 family's
// (codec-mduv380.md). Not used: encode ECC +0x4E8 and FEC decode +0xB78
#define UV_ENTRIES { .encode = 0x130, .decode = 0x5D8, .enc_init = 0x284, .fec_init = 0x4CC, .dec_init = 0x6D0, \
		.enc_params = 0x848, .enc_set = 0x1C0, .dec_set = 0x688 }

// MD-380 D35.14
static const codec_desc_window_t d3514_tables[] = {
	{ 0x080B7868u, 0xCD68u, 0x810CC7B3u },
	{ 0x080ECC98u, 0x87Cu, 0xA5F0D272u },
	{ 0x080F0C14u, 0xEu, 0xEFDB66A5u },
};
static const stm32_codec_profile_t d3514 = {
	.tag = 0x00443514u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080C45D0u, .code_len = 0x10366u, .code_crc = 0x83A0361Fu, .tables = d3514_tables, .windows = 3,
	.at = UV_ENTRIES,
};

// MD-390 S35.14
static const codec_desc_window_t s3514_tables[] = {
	{ 0x080B8928u, 0xCDE8u, 0x68B2CC40u },
	{ 0x080EDDD8u, 0x87Cu, 0xA2B85A33u },
	{ 0x080F1D54u, 0xEu, 0xEFDB66A5u },
};
static const stm32_codec_profile_t s3514 = {
	.tag = 0x00533514u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080C5710u, .code_len = 0x10372u, .code_crc = 0xD953C7A8u, .tables = s3514_tables, .windows = 3,
	.at = UV_ENTRIES,
};

static const stm32_codec_profile_t *const profiles[] = { &d2032, &d3514, &s3514 };
const stm32_codec_profiles_t stm32_codec_profiles = { profiles, 3 };

#endif	// CONFIG_DMR
