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

// The other builds with D002.032's codec build (D3.xx and the older D2.xx files): the same entry offsets as d2032, the
// windows and the code's length per file
#define D2_ENTRIES { .encode = 0x14C, .decode = 0x604, .enc_init = 0x294, .fec_init = 0x4FC, .dec_init = 0x6F0, \
		.enc_params = 0x854, .enc_set = 0x1E0, .dec_set = 0x6AC }

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

// TYT-TFT-MD380-D3.32.bin D3.32
static const codec_desc_window_t xmd380d3032_tables[] = {
	{ 0x08090BF0u, 0xAFF8u, 0x2445FE8Fu },
	{ 0x080CFFC0u, 0x24u, 0x7AAFE1D3u },
	{ 0x080F86D8u, 0x18u, 0x33E82E70u },
	{ 0x080F8F2Cu, 0x14u, 0xFFF0E930u },
	{ 0x080FC61Cu, 0x20u, 0xDA18C5EFu },
};
static const stm32_codec_profile_t xmd380d3032 = {
	.tag = 0x00440332u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080E0000u, .code_len = 0x1087Au, .code_crc = 0x07369C2Cu, .tables = xmd380d3032_tables, .windows = 5,
	.at = D2_ENTRIES,
};

// RETEVIS-Vocoder-RT3-with GPS-S14.04.bin S14.04
static const codec_desc_window_t xrt3s1404_tables[] = {
	{ 0x080915E0u, 0x92FCu, 0x2445ED1Bu },
	{ 0x080D098Eu, 0x4Eu, 0xA28CCD40u },
	{ 0x080F90A4u, 0x868u, 0xB8A4973Bu },
	{ 0x080FCFF0u, 0xEu, 0xEFDB66A5u },
};
static const stm32_codec_profile_t xrt3s1404 = {
	.tag = 0x00531404u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x0809A8DCu, .code_len = 0x10342u, .code_crc = 0x82FE5C5Du, .tables = xrt3s1404_tables, .windows = 4,
	.at = UV_ENTRIES,
};

// D013.034.bin D13.034
static const codec_desc_window_t xmd380d13034_tables[] = {
	{ 0x080907A0u, 0x90D4u, 0x15B8E25Au },
	{ 0x080CFACEu, 0x4Eu, 0xA28CCD40u },
	{ 0x080F81E4u, 0x868u, 0x117A8303u },
	{ 0x080FC130u, 0xEu, 0xEFDB66A5u },
};
static const stm32_codec_profile_t xmd380d13034 = {
	.tag = 0x00441334u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x08099874u, .code_len = 0x10356u, .code_crc = 0x2E0C4561u, .tables = xmd380d13034_tables, .windows = 4,
	.at = UV_ENTRIES,
};

// TYT-Vocoder-MD380-D13.14.bin D13.14
static const codec_desc_window_t xmd380d1314_tables[] = {
	{ 0x08090410u, 0x94E4u, 0xF78E94DEu },
	{ 0x080CF71Eu, 0x4Eu, 0xA28CCD40u },
	{ 0x080F7E1Cu, 0x868u, 0x42C39178u },
	{ 0x080FBD50u, 0xEu, 0xEFDB66A5u },
};
static const stm32_codec_profile_t xmd380d1314 = {
	.tag = 0x00441314u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080998F4u, .code_len = 0x10362u, .code_crc = 0x57BE4B8Cu, .tables = xmd380d1314_tables, .windows = 4,
	.at = UV_ENTRIES,
};

// TYT-Vocoder-MD380-D13.20.bin D13.20
static const codec_desc_window_t xmd380d1320_tables[] = {
	{ 0x08090490u, 0x94E4u, 0xEC93E5BCu },
	{ 0x080CF79Eu, 0x4Eu, 0xA28CCD40u },
	{ 0x080F7E9Cu, 0x868u, 0x6DE76FF7u },
	{ 0x080FBDD0u, 0xEu, 0xEFDB66A5u },
};
static const stm32_codec_profile_t xmd380d1320 = {
	.tag = 0x00441320u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x08099974u, .code_len = 0x1034Eu, .code_crc = 0x8AC4FAF5u, .tables = xmd380d1320_tables, .windows = 4,
	.at = UV_ENTRIES,
};

// TYT-Vocoder-MD380-D14.07.bin D14.07
static const codec_desc_window_t xmd380d1407_tables[] = {
	{ 0x080909E0u, 0x9122u, 0x0571A6F1u },
	{ 0x080CFD0Eu, 0x4Eu, 0xA28CCD40u },
	{ 0x080F8424u, 0x868u, 0x091CD076u },
	{ 0x080FC370u, 0xEu, 0xEFDB66A5u },
};
static const stm32_codec_profile_t xmd380d1407 = {
	.tag = 0x00441407u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x08099B04u, .code_len = 0x10366u, .code_crc = 0x6AC85EF3u, .tables = xmd380d1407_tables, .windows = 4,
	.at = UV_ENTRIES,
};

// Upgraded software for newest version.bin D2.24 (upgrade pack)
static const codec_desc_window_t xmd380d2024up_tables[] = {
	{ 0x08091024u, 0xAFF8u, 0x68F0B40Du },
	{ 0x080D2338u, 0x24u, 0x7AAFE1D3u },
	{ 0x080FA9F0u, 0x18u, 0x33E82E70u },
	{ 0x080FB1CCu, 0x14u, 0xFFF0E930u },
	{ 0x080FE7C0u, 0x20u, 0xDA18C5EFu },
};
static const stm32_codec_profile_t xmd380d2024up = {
	.tag = 0x00440224u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080E0000u, .code_len = 0x1089Eu, .code_crc = 0x318B0FDAu, .tables = xmd380d2024up_tables, .windows = 5,
	.at = D2_ENTRIES,
};

// MD-380-D2.25.bin D2.25
static const codec_desc_window_t xmd380d2025_tables[] = {
	{ 0x0808FA64u, 0xAFF8u, 0x8C7DA6F8u },
	{ 0x080D0DC8u, 0x24u, 0x7AAFE1D3u },
	{ 0x080F9480u, 0x18u, 0x33E82E70u },
	{ 0x080F9C5Cu, 0x14u, 0xFFF0E930u },
	{ 0x080FD250u, 0x20u, 0xDA18C5EFu },
};
static const stm32_codec_profile_t xmd380d2025 = {
	.tag = 0x00440225u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080E0000u, .code_len = 0x1087Au, .code_crc = 0x308F0A9Cu, .tables = xmd380d2025_tables, .windows = 5,
	.at = D2_ENTRIES,
};

// MD-380-D2.26.bin D2.26
static const codec_desc_window_t xmd380d2026_tables[] = {
	{ 0x0808F9E4u, 0xAFF8u, 0xEB9AE318u },
	{ 0x080D0D48u, 0x24u, 0x7AAFE1D3u },
	{ 0x080F9400u, 0x18u, 0x33E82E70u },
	{ 0x080F9BDCu, 0x14u, 0xFFF0E930u },
	{ 0x080FD1D0u, 0x20u, 0xDA18C5EFu },
};
static const stm32_codec_profile_t xmd380d2026 = {
	.tag = 0x00440226u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080E0000u, .code_len = 0x10866u, .code_crc = 0xC4F140C2u, .tables = xmd380d2026_tables, .windows = 5,
	.at = D2_ENTRIES,
};

// MD-380-D2.27.bin D2.27
static const codec_desc_window_t xmd380d2027_tables[] = {
	{ 0x0808F9E4u, 0xAFF8u, 0xEB89B60Bu },
	{ 0x080D0D48u, 0x24u, 0x7AAFE1D3u },
	{ 0x080F9400u, 0x18u, 0x33E82E70u },
	{ 0x080F9BDCu, 0x14u, 0xFFF0E930u },
	{ 0x080FD1D0u, 0x20u, 0xDA18C5EFu },
};
static const stm32_codec_profile_t xmd380d2027 = {
	.tag = 0x00440227u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080E0000u, .code_len = 0x10866u, .code_crc = 0x662DA478u, .tables = xmd380d2027_tables, .windows = 5,
	.at = D2_ENTRIES,
};

// MD-380-D2.30.bin D2.30
static const codec_desc_window_t xmd380d2030_tables[] = {
	{ 0x0808FC24u, 0xAFF8u, 0x41CFB9A5u },
	{ 0x080D0F88u, 0x24u, 0x7AAFE1D3u },
	{ 0x080F9658u, 0x18u, 0x33E82E70u },
	{ 0x080F9E34u, 0x14u, 0xFFF0E930u },
	{ 0x080FD450u, 0x20u, 0xDA18C5EFu },
};
static const stm32_codec_profile_t xmd380d2030 = {
	.tag = 0x00440230u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080E0000u, .code_len = 0x1086Au, .code_crc = 0xF47AF71Au, .tables = xmd380d2030_tables, .windows = 5,
	.at = D2_ENTRIES,
};

// MD-380-D2.34.bin D2.34
static const codec_desc_window_t xmd380d2034_tables[] = {
	{ 0x0808FDA4u, 0xAFF8u, 0x2F6FFE2Fu },
	{ 0x080D1108u, 0x24u, 0x7AAFE1D3u },
	{ 0x080F97D8u, 0x18u, 0x33E82E70u },
	{ 0x080F9FB4u, 0x14u, 0xFFF0E930u },
	{ 0x080FD5D0u, 0x20u, 0xDA18C5EFu },
};
static const stm32_codec_profile_t xmd380d2034 = {
	.tag = 0x00440234u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080E0000u, .code_len = 0x10892u, .code_crc = 0x777AD2ABu, .tables = xmd380d2034_tables, .windows = 5,
	.at = D2_ENTRIES,
};

// D003.008.bin D3.008
static const codec_desc_window_t xmd380d3008_tables[] = {
	{ 0x08090660u, 0xAFF8u, 0xBF6A2D83u },
	{ 0x080CFA10u, 0x24u, 0x7AAFE1D3u },
	{ 0x080F8110u, 0x18u, 0x33E82E70u },
	{ 0x080F8964u, 0x14u, 0xFFF0E930u },
	{ 0x080FC03Cu, 0x20u, 0xDA18C5EFu },
};
static const stm32_codec_profile_t xmd380d3008 = {
	.tag = 0x00440308u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080E0000u, .code_len = 0x10896u, .code_crc = 0xB3773804u, .tables = xmd380d3008_tables, .windows = 5,
	.at = D2_ENTRIES,
};

// TYT-TFT-MD380-D3.20.bin D3.20
static const codec_desc_window_t xmd380d3020_tables[] = {
	{ 0x08090960u, 0xAFF8u, 0x1B844817u },
	{ 0x080CFD10u, 0x24u, 0x7AAFE1D3u },
	{ 0x080F8410u, 0x18u, 0x33E82E70u },
	{ 0x080F8C64u, 0x14u, 0xFFF0E930u },
	{ 0x080FC33Cu, 0x20u, 0xDA18C5EFu },
};
static const stm32_codec_profile_t xmd380d3020 = {
	.tag = 0x00440320u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080E0000u, .code_len = 0x10882u, .code_crc = 0xAA1D7DD6u, .tables = xmd380d3020_tables, .windows = 5,
	.at = D2_ENTRIES,
};

// TYT-Vocoder-MD390-S13.12.bin S13.12
static const codec_desc_window_t xmd390s1312_tables[] = {
	{ 0x08091090u, 0x9294u, 0xA649816Du },
	{ 0x080D041Eu, 0x4Eu, 0xA28CCD40u },
	{ 0x080F8B1Cu, 0x868u, 0x1240DAC1u },
	{ 0x080FCA50u, 0xEu, 0xEFDB66A5u },
};
static const stm32_codec_profile_t xmd390s1312 = {
	.tag = 0x00531312u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x0809A324u, .code_len = 0x10346u, .code_crc = 0x159EF17Eu, .tables = xmd390s1312_tables, .windows = 4,
	.at = UV_ENTRIES,
};

// TYT-Vocoder-MD390-S13.20.bin S13.20
static const codec_desc_window_t xmd390s1320_tables[] = {
	{ 0x080912D0u, 0x9564u, 0x7E7F431Du },
	{ 0x080D065Eu, 0x4Eu, 0xA28CCD40u },
	{ 0x080F8D5Cu, 0x868u, 0xFB8963BFu },
	{ 0x080FCC90u, 0xEu, 0xEFDB66A5u },
};
static const stm32_codec_profile_t xmd390s1320 = {
	.tag = 0x00531320u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x0809A834u, .code_len = 0x1034Eu, .code_crc = 0xB91DF0D1u, .tables = xmd390s1320_tables, .windows = 4,
	.at = UV_ENTRIES,
};

// TYT-Vocoder-MD390-S14.07.bin S14.07
static const codec_desc_window_t xmd390s1407_tables[] = {
	{ 0x08091620u, 0x9154u, 0x12AC6B06u },
	{ 0x080D09CEu, 0x4Eu, 0xA28CCD40u },
	{ 0x080F90E4u, 0x868u, 0x8A5DE917u },
	{ 0x080FD030u, 0xEu, 0xEFDB66A5u },
};
static const stm32_codec_profile_t xmd390s1407 = {
	.tag = 0x00531407u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x0809A774u, .code_len = 0x1033Eu, .code_crc = 0x41705E75u, .tables = xmd390s1407_tables, .windows = 4,
	.at = UV_ENTRIES,
};

// TYT-GPS-MD390-MD380-S003.012.bin S3.012
static const codec_desc_window_t xmd390s3012_tables[] = {
	{ 0x08091560u, 0xAFF8u, 0xB25BA64Cu },
	{ 0x080D0990u, 0x24u, 0x7AAFE1D3u },
	{ 0x080F9090u, 0x18u, 0x33E82E70u },
	{ 0x080F98E4u, 0x14u, 0xFFF0E930u },
	{ 0x080FCFBCu, 0x20u, 0xDA18C5EFu },
};
static const stm32_codec_profile_t xmd390s3012 = {
	.tag = 0x00530312u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080E0000u, .code_len = 0x1087Au, .code_crc = 0x04E6AF17u, .tables = xmd390s3012_tables, .windows = 5,
	.at = D2_ENTRIES,
};

// TYT-Vocoder-MD446-D13.20.bin D13.20
static const codec_desc_window_t xmd446d1320_tables[] = {
	{ 0x080904D0u, 0x90D4u, 0xCC12D2AAu },
	{ 0x080CF7FEu, 0x4Eu, 0xA28CCD40u },
	{ 0x080F7EE4u, 0x868u, 0x9C044751u },
	{ 0x080FBE1Cu, 0xEu, 0xEFDB66A5u },
};
static const stm32_codec_profile_t xmd446d1320 = {
	.tag = 0x04441320u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080995A4u, .code_len = 0x10372u, .code_crc = 0xBD3277AEu, .tables = xmd446d1320_tables, .windows = 4,
	.at = UV_ENTRIES,
};

// TYT-TFT-MD446-D3.20.bin D3.20
static const codec_desc_window_t xmd446d3020_tables[] = {
	{ 0x08090960u, 0xAFF8u, 0x55648D6Bu },
	{ 0x080CFD30u, 0x24u, 0x7AAFE1D3u },
	{ 0x080F8418u, 0x18u, 0x33E82E70u },
	{ 0x080F8C6Cu, 0x14u, 0xFFF0E930u },
	{ 0x080FC348u, 0x20u, 0xDA18C5EFu },
};
static const stm32_codec_profile_t xmd446d3020 = {
	.tag = 0x04440320u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080E0000u, .code_len = 0x10866u, .code_crc = 0x54F942E7u, .tables = xmd446d3020_tables, .windows = 5,
	.at = D2_ENTRIES,
};

static const stm32_codec_profile_t *const profiles[] = {
	&d2032, &d3514, &s3514,
	&xmd380d3032, &xrt3s1404,
	&xmd380d13034, &xmd380d1314, &xmd380d1320, &xmd380d1407, &xmd380d2024up, &xmd380d2025, &xmd380d2026, &xmd380d2027, &xmd380d2030, &xmd380d2034, &xmd380d3008, &xmd380d3020, &xmd390s1312, &xmd390s1320, &xmd390s1407, &xmd390s3012, &xmd446d1320, &xmd446d3020,
};
const stm32_codec_profiles_t stm32_codec_profiles = { profiles, 23 };

#endif	// CONFIG_DMR
