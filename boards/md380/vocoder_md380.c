// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// MD-380 vocoder: the radio's own AMBE+2 codec run in place from flash (docs/spec/codec-placement.md, "STM32
// radios"; the codec: docs/spec/codec-md380.md, "State init and calls").
//
// The merge on the user's machine (rfoundry, tools/codec_extract.py, the web flasher) puts the whole codec in sector 11
// (docs/spec/codec-placement.md, "Layouts"): its code from 0x080E0000, then its main table window, then its small
// table windows, each moved as one block, and the descriptor at the end of the sector (platform/stm32f4/codec_desc.h).
// The merge rewrites every address in the codec that points into a moved window (the codec map): the code's references
// to itself and to its tables, and the calls from the code that stock keeps in the table window back into the code.
// None of it is part of this repository or a release. The 35.14 builds (MD-380 D35.14, MD-390 S35.14) and the 13.xx
// and 14.xx builds carry the UV380 family's codec instead (their entry offsets), with three or four table windows.
// The calls and the checks are the shared ones (platform/stm32f4/hrc/vocoder.c); this file says which build and where.
// Without a valid descriptor and codec, present() is false and the radio is FM only ("No codec: voice off"). That is
// also what a codec where an earlier release put it gives: its windows lie below CODEC_FLASH_LO, so the descriptor
// is refused before any byte of it is read as code, and the flashing tools move it on the next install.
#include "md380.h"
#include "config.h"

#if CONFIG_DMR	// the whole file: DMR only
#include "hrc/vocoder.h"

#define CODEC_TAG		0x2032u		// the descriptor's tag for stock D002.032
#define CODEC_FLASH_LO		0x080E0000u	// sector 11: the flash boards/md380/memory.ld leaves to the codec

// The D002.032 codec as the merge places it (tools/rfoundry/rfoundry/stockfw.py, "placed"): CRC-32 (IEEE 802.3) of
// the code and the main table window after the rewrites, and of the small windows (unchanged). The descriptor must
// carry exactly these. Every window keeps its stock address modulo 16
#define CODEC_CODE		0x080E0004u
#define CODEC_CODE_LEN		0x10884u
#define CODEC_CRC		0xBAA2C858u
#define CODEC_WINDOWS		5u

static const codec_desc_window_t tables[CODEC_WINDOWS] = {
	{ 0x080F0894u, 0xAFF8u, 0x04DF1EE4u },
	{ 0x080FB898u, 0x24u, 0x7AAFE1D3u },
	{ 0x080FB8C8u, 0x18u, 0x33E82E70u },
	{ 0x080FB8E4u, 0x14u, 0xFFF0E930u },
	{ 0x080FB900u, 0x20u, 0xDA18C5EFu },
};

// The id tools/rfoundry and the release manifests use for a codec placed as these profiles say (the descriptor and
// every CRC matched; stockfw.py MD380_CODEC): one for every build, named after D002.032, the first. The radio keeps
// its codec across updates going forward only: a release older than the one that added a build's profile doesn't
// know its tag and finds no codec. Releases with the codec in sectors 8-11 reported "md380-d2032-s11", which tells
// the flashing tools to read the codec back from there and move it (stockfw.py "former")
#define CODEC_ID		"md380-d2032-s11v2"

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

// The other builds with D002.032's codec build (D3.xx and the older D2.xx files): the same entry offsets as d2032, the
// windows and the code's length per file
#define D2_ENTRIES { .encode = 0x14C, .decode = 0x604, .enc_init = 0x294, .fec_init = 0x4FC, .dec_init = 0x6F0, \
		.enc_params = 0x854, .enc_set = 0x1E0, .dec_set = 0x6AC }

// MD-380 D35.14
static const codec_desc_window_t d3514_tables[] = {
	{ 0x080F0368u, 0xCD68u, 0x3F7F60A1u },
	{ 0x080FD0D8u, 0x87Cu, 0xA5F0D272u },
	{ 0x080FD954u, 0xEu, 0xEFDB66A5u },
};
static const stm32_codec_profile_t d3514 = {
	.tag = 0x00443514u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080E0000u, .code_len = 0x10366u, .code_crc = 0x632E7F32u, .tables = d3514_tables, .windows = 3,
	.at = UV_ENTRIES,
};

// MD-390 S35.14
static const codec_desc_window_t s3514_tables[] = {
	{ 0x080F0378u, 0xCDE8u, 0x2DCB2F57u },
	{ 0x080FD168u, 0x87Cu, 0xA2B85A33u },
	{ 0x080FD9E4u, 0xEu, 0xEFDB66A5u },
};
static const stm32_codec_profile_t s3514 = {
	.tag = 0x00533514u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080E0004u, .code_len = 0x10372u, .code_crc = 0x446271B6u, .tables = s3514_tables, .windows = 3,
	.at = UV_ENTRIES,
};

// TYT-TFT-MD380-D3.32.bin D3.32
static const codec_desc_window_t xmd380d3032_tables[] = {
	{ 0x080F0890u, 0xAFF8u, 0xA0E259E3u },
	{ 0x080FB890u, 0x24u, 0x7AAFE1D3u },
	{ 0x080FB8B8u, 0x18u, 0x33E82E70u },
	{ 0x080FB8DCu, 0x14u, 0xFFF0E930u },
	{ 0x080FB8FCu, 0x20u, 0xDA18C5EFu },
};
static const stm32_codec_profile_t xmd380d3032 = {
	.tag = 0x00440332u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080E000Cu, .code_len = 0x1087Au, .code_crc = 0xCC4C18C5u, .tables = xmd380d3032_tables, .windows = 5,
	.at = D2_ENTRIES,
};

// RETEVIS-Vocoder-RT3-with GPS-S14.04.bin S14.04
static const codec_desc_window_t xrt3s1404_tables[] = {
	{ 0x080F0350u, 0x92FCu, 0xE60B5532u },
	{ 0x080F964Eu, 0x4Eu, 0xA28CCD40u },
	{ 0x080F96A4u, 0x868u, 0xB8A4973Bu },
	{ 0x080F9F10u, 0xEu, 0xEFDB66A5u },
};
static const stm32_codec_profile_t xrt3s1404 = {
	.tag = 0x00531404u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080E0004u, .code_len = 0x10342u, .code_crc = 0x8540F233u, .tables = xrt3s1404_tables, .windows = 4,
	.at = UV_ENTRIES,
};

// D013.034.bin D13.034
static const codec_desc_window_t xmd380d13034_tables[] = {
	{ 0x080F0360u, 0x90D4u, 0x9041E916u },
	{ 0x080F943Eu, 0x4Eu, 0xA28CCD40u },
	{ 0x080F9494u, 0x868u, 0x117A8303u },
	{ 0x080F9D00u, 0xEu, 0xEFDB66A5u },
};
static const stm32_codec_profile_t xmd380d13034 = {
	.tag = 0x00441334u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080E0000u, .code_len = 0x10356u, .code_crc = 0x2BFDA5D3u, .tables = xmd380d13034_tables, .windows = 4,
	.at = UV_ENTRIES,
};

// TYT-Vocoder-MD380-D13.14.bin D13.14
static const codec_desc_window_t xmd380d1314_tables[] = {
	{ 0x080F0370u, 0x94E4u, 0x093CF69Cu },
	{ 0x080F985Eu, 0x4Eu, 0xA28CCD40u },
	{ 0x080F98ACu, 0x868u, 0x42C39178u },
	{ 0x080FA120u, 0xEu, 0xEFDB66A5u },
};
static const stm32_codec_profile_t xmd380d1314 = {
	.tag = 0x00441314u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080E0004u, .code_len = 0x10362u, .code_crc = 0x76E8A208u, .tables = xmd380d1314_tables, .windows = 4,
	.at = UV_ENTRIES,
};

// TYT-Vocoder-MD380-D13.20.bin D13.20
static const codec_desc_window_t xmd380d1320_tables[] = {
	{ 0x080F0360u, 0x94E4u, 0x47BFD116u },
	{ 0x080F984Eu, 0x4Eu, 0xA28CCD40u },
	{ 0x080F989Cu, 0x868u, 0x6DE76FF7u },
	{ 0x080FA110u, 0xEu, 0xEFDB66A5u },
};
static const stm32_codec_profile_t xmd380d1320 = {
	.tag = 0x00441320u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080E0008u, .code_len = 0x1034Eu, .code_crc = 0x414EB952u, .tables = xmd380d1320_tables, .windows = 4,
	.at = UV_ENTRIES,
};

// D014.004.bin D14.004
static const codec_desc_window_t xmd380d14004_tables[] = {
	{ 0x080F0350u, 0x90D4u, 0x7EEC43F6u },
	{ 0x080F942Eu, 0x4Eu, 0xA28CCD40u },
	{ 0x080F9484u, 0x868u, 0x117A8303u },
	{ 0x080F9CF0u, 0xEu, 0xEFDB66A5u },
};
static const stm32_codec_profile_t xmd380d14004 = {
	.tag = 0x00441404u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080E0004u, .code_len = 0x10342u, .code_crc = 0x51932859u, .tables = xmd380d14004_tables, .windows = 4,
	.at = UV_ENTRIES,
};

// TYT-Vocoder-MD380-D14.07.bin D14.07
static const codec_desc_window_t xmd380d1407_tables[] = {
	{ 0x080F0370u, 0x9122u, 0x31D913FCu },
	{ 0x080F949Eu, 0x4Eu, 0xA28CCD40u },
	{ 0x080F94F4u, 0x868u, 0x091CD076u },
	{ 0x080F9D60u, 0xEu, 0xEFDB66A5u },
};
static const stm32_codec_profile_t xmd380d1407 = {
	.tag = 0x00441407u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080E0000u, .code_len = 0x10366u, .code_crc = 0x5359BBD8u, .tables = xmd380d1407_tables, .windows = 4,
	.at = UV_ENTRIES,
};

// Upgraded software for newest version.bin D2.24 (upgrade pack)
static const codec_desc_window_t xmd380d2024up_tables[] = {
	{ 0x080F08B4u, 0xAFF8u, 0xA072ACC8u },
	{ 0x080FB8B8u, 0x24u, 0x7AAFE1D3u },
	{ 0x080FB8E0u, 0x18u, 0x33E82E70u },
	{ 0x080FB8FCu, 0x14u, 0xFFF0E930u },
	{ 0x080FB910u, 0x20u, 0xDA18C5EFu },
};
static const stm32_codec_profile_t xmd380d2024up = {
	.tag = 0x00440224u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080E0008u, .code_len = 0x1089Eu, .code_crc = 0x05D45115u, .tables = xmd380d2024up_tables, .windows = 5,
	.at = D2_ENTRIES,
};

// MD-380-D2.25.bin D2.25
static const codec_desc_window_t xmd380d2025_tables[] = {
	{ 0x080F0894u, 0xAFF8u, 0x53F6D661u },
	{ 0x080FB898u, 0x24u, 0x7AAFE1D3u },
	{ 0x080FB8C0u, 0x18u, 0x33E82E70u },
	{ 0x080FB8DCu, 0x14u, 0xFFF0E930u },
	{ 0x080FB8F0u, 0x20u, 0xDA18C5EFu },
};
static const stm32_codec_profile_t xmd380d2025 = {
	.tag = 0x00440225u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080E000Cu, .code_len = 0x1087Au, .code_crc = 0x2DCEC323u, .tables = xmd380d2025_tables, .windows = 5,
	.at = D2_ENTRIES,
};

// MD-380-D2.26.bin D2.26
static const codec_desc_window_t xmd380d2026_tables[] = {
	{ 0x080F0874u, 0xAFF8u, 0xA47D4B16u },
	{ 0x080FB878u, 0x24u, 0x7AAFE1D3u },
	{ 0x080FB8A0u, 0x18u, 0x33E82E70u },
	{ 0x080FB8BCu, 0x14u, 0xFFF0E930u },
	{ 0x080FB8D0u, 0x20u, 0xDA18C5EFu },
};
static const stm32_codec_profile_t xmd380d2026 = {
	.tag = 0x00440226u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080E0000u, .code_len = 0x10866u, .code_crc = 0xB7094E15u, .tables = xmd380d2026_tables, .windows = 5,
	.at = D2_ENTRIES,
};

// MD-380-D2.27.bin D2.27
static const codec_desc_window_t xmd380d2027_tables[] = {
	{ 0x080F0874u, 0xAFF8u, 0xA46E1E05u },
	{ 0x080FB878u, 0x24u, 0x7AAFE1D3u },
	{ 0x080FB8A0u, 0x18u, 0x33E82E70u },
	{ 0x080FB8BCu, 0x14u, 0xFFF0E930u },
	{ 0x080FB8D0u, 0x20u, 0xDA18C5EFu },
};
static const stm32_codec_profile_t xmd380d2027 = {
	.tag = 0x00440227u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080E0000u, .code_len = 0x10866u, .code_crc = 0x15D5AAAFu, .tables = xmd380d2027_tables, .windows = 5,
	.at = D2_ENTRIES,
};

// MD-380-D2.30.bin D2.30
static const codec_desc_window_t xmd380d2030_tables[] = {
	{ 0x080F0884u, 0xAFF8u, 0xCE99EE52u },
	{ 0x080FB888u, 0x24u, 0x7AAFE1D3u },
	{ 0x080FB8B8u, 0x18u, 0x33E82E70u },
	{ 0x080FB8D4u, 0x14u, 0xFFF0E930u },
	{ 0x080FB8F0u, 0x20u, 0xDA18C5EFu },
};
static const stm32_codec_profile_t xmd380d2030 = {
	.tag = 0x00440230u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080E000Cu, .code_len = 0x1086Au, .code_crc = 0x0670381Au, .tables = xmd380d2030_tables, .windows = 5,
	.at = D2_ENTRIES,
};

// MD-380-D2.34.bin D2.34
static const codec_desc_window_t xmd380d2034_tables[] = {
	{ 0x080F08A4u, 0xAFF8u, 0x26F75D98u },
	{ 0x080FB8A8u, 0x24u, 0x7AAFE1D3u },
	{ 0x080FB8D8u, 0x18u, 0x33E82E70u },
	{ 0x080FB8F4u, 0x14u, 0xFFF0E930u },
	{ 0x080FB910u, 0x20u, 0xDA18C5EFu },
};
static const stm32_codec_profile_t xmd380d2034 = {
	.tag = 0x00440234u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080E0004u, .code_len = 0x10892u, .code_crc = 0xD85CA32Eu, .tables = xmd380d2034_tables, .windows = 5,
	.at = D2_ENTRIES,
};

// D003.008.bin D3.008
static const codec_desc_window_t xmd380d3008_tables[] = {
	{ 0x080F08A0u, 0xAFF8u, 0x4B8A648Du },
	{ 0x080FB8A0u, 0x24u, 0x7AAFE1D3u },
	{ 0x080FB8D0u, 0x18u, 0x33E82E70u },
	{ 0x080FB8F4u, 0x14u, 0xFFF0E930u },
	{ 0x080FB90Cu, 0x20u, 0xDA18C5EFu },
};
static const stm32_codec_profile_t xmd380d3008 = {
	.tag = 0x00440308u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080E0000u, .code_len = 0x10896u, .code_crc = 0xC4751D4Cu, .tables = xmd380d3008_tables, .windows = 5,
	.at = D2_ENTRIES,
};

// TYT-TFT-MD380-D3.20.bin D3.20
static const codec_desc_window_t xmd380d3020_tables[] = {
	{ 0x080F0890u, 0xAFF8u, 0x2C1DC5A0u },
	{ 0x080FB890u, 0x24u, 0x7AAFE1D3u },
	{ 0x080FB8C0u, 0x18u, 0x33E82E70u },
	{ 0x080FB8E4u, 0x14u, 0xFFF0E930u },
	{ 0x080FB8FCu, 0x20u, 0xDA18C5EFu },
};
static const stm32_codec_profile_t xmd380d3020 = {
	.tag = 0x00440320u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080E0004u, .code_len = 0x10882u, .code_crc = 0x9DE0DD67u, .tables = xmd380d3020_tables, .windows = 5,
	.at = D2_ENTRIES,
};

// TYT-Vocoder-MD390-S13.12.bin S13.12
static const codec_desc_window_t xmd390s1312_tables[] = {
	{ 0x080F0350u, 0x9294u, 0x89E08BDCu },
	{ 0x080F95EEu, 0x4Eu, 0xA28CCD40u },
	{ 0x080F963Cu, 0x868u, 0x1240DAC1u },
	{ 0x080F9EB0u, 0xEu, 0xEFDB66A5u },
};
static const stm32_codec_profile_t xmd390s1312 = {
	.tag = 0x00531312u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080E0000u, .code_len = 0x10346u, .code_crc = 0xA6598879u, .tables = xmd390s1312_tables, .windows = 4,
	.at = UV_ENTRIES,
};

// TYT-Vocoder-MD390-S13.20.bin S13.20
static const codec_desc_window_t xmd390s1320_tables[] = {
	{ 0x080F0360u, 0x9564u, 0x06D75947u },
	{ 0x080F98CEu, 0x4Eu, 0xA28CCD40u },
	{ 0x080F991Cu, 0x868u, 0xFB8963BFu },
	{ 0x080FA190u, 0xEu, 0xEFDB66A5u },
};
static const stm32_codec_profile_t xmd390s1320 = {
	.tag = 0x00531320u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080E0008u, .code_len = 0x1034Eu, .code_crc = 0xD62D8CB7u, .tables = xmd390s1320_tables, .windows = 4,
	.at = UV_ENTRIES,
};

// TYT-Vocoder-MD390-S14.07.bin S14.07
static const codec_desc_window_t xmd390s1407_tables[] = {
	{ 0x080F0350u, 0x9154u, 0xDDF71172u },
	{ 0x080F94AEu, 0x4Eu, 0xA28CCD40u },
	{ 0x080F9504u, 0x868u, 0x8A5DE917u },
	{ 0x080F9D70u, 0xEu, 0xEFDB66A5u },
};
static const stm32_codec_profile_t xmd390s1407 = {
	.tag = 0x00531407u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080E0008u, .code_len = 0x1033Eu, .code_crc = 0xEF3F72D3u, .tables = xmd390s1407_tables, .windows = 4,
	.at = UV_ENTRIES,
};

// TYT-GPS-MD390-MD380-S003.012.bin S3.012
static const codec_desc_window_t xmd390s3012_tables[] = {
	{ 0x080F0890u, 0xAFF8u, 0x5C73A624u },
	{ 0x080FB890u, 0x24u, 0x7AAFE1D3u },
	{ 0x080FB8C0u, 0x18u, 0x33E82E70u },
	{ 0x080FB8E4u, 0x14u, 0xFFF0E930u },
	{ 0x080FB8FCu, 0x20u, 0xDA18C5EFu },
};
static const stm32_codec_profile_t xmd390s3012 = {
	.tag = 0x00530312u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080E000Cu, .code_len = 0x1087Au, .code_crc = 0x0B79AD0Au, .tables = xmd390s3012_tables, .windows = 5,
	.at = D2_ENTRIES,
};

// TYT-Vocoder-MD446-D13.20.bin D13.20
static const codec_desc_window_t xmd446d1320_tables[] = {
	{ 0x080F0380u, 0x90D4u, 0x312BCC55u },
	{ 0x080F945Eu, 0x4Eu, 0xA28CCD40u },
	{ 0x080F94B4u, 0x868u, 0x9C044751u },
	{ 0x080F9D1Cu, 0xEu, 0xEFDB66A5u },
};
static const stm32_codec_profile_t xmd446d1320 = {
	.tag = 0x04441320u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080E0004u, .code_len = 0x10372u, .code_crc = 0x1E969669u, .tables = xmd446d1320_tables, .windows = 4,
	.at = UV_ENTRIES,
};

// TYT-TFT-MD446-D3.20.bin D3.20
static const codec_desc_window_t xmd446d3020_tables[] = {
	{ 0x080F0870u, 0xAFF8u, 0x9CA289D8u },
	{ 0x080FB870u, 0x24u, 0x7AAFE1D3u },
	{ 0x080FB898u, 0x18u, 0x33E82E70u },
	{ 0x080FB8BCu, 0x14u, 0xFFF0E930u },
	{ 0x080FB8D8u, 0x20u, 0xDA18C5EFu },
};
static const stm32_codec_profile_t xmd446d3020 = {
	.tag = 0x04440320u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080E0000u, .code_len = 0x10866u, .code_crc = 0x00406609u, .tables = xmd446d3020_tables, .windows = 5,
	.at = D2_ENTRIES,
};

static const stm32_codec_profile_t *const profiles[] = {
	&d2032, &d3514, &s3514,
	&xmd380d3032, &xrt3s1404,
	&xmd380d13034, &xmd380d1314, &xmd380d1320, &xmd380d14004, &xmd380d1407, &xmd380d2024up, &xmd380d2025, &xmd380d2026, &xmd380d2027, &xmd380d2030, &xmd380d2034, &xmd380d3008, &xmd380d3020, &xmd390s1312, &xmd390s1320, &xmd390s1407, &xmd390s3012, &xmd446d1320, &xmd446d3020,
};
const stm32_codec_profiles_t stm32_codec_profiles = { profiles, 24 };

#endif	// CONFIG_DMR
