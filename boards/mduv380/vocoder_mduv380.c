// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// MD-UV380 vocoder: the radio's own AMBE+2 codec run in place from flash (docs/spec/codec-placement.md, "STM32 radios";
// the codec: docs/spec/codec-mduv380.md). Any build listed below can be the donor (TYT and Retevis, 18.11 to 219.74);
// they hold the same codec at their own addresses. The merge on the user's machine (rfoundry, tools/codec_extract.py,
// the web flasher) writes the codec's tables at their address in that build (cut to the codec's own data), its code
// right after them, word aligned, or at the start of sector 10 when it doesn't fit there, and the descriptor at the end
// of sector 11 (platform/stm32f4/codec_desc.h). The code reaches its tables
// through absolute addresses, which don't change; the merge rewrites one pointer into the code and the 12 calls from
// the code kept in the table window back into the code (the codec map). The calls and the checks are the shared ones
// (platform/stm32f4/hrc/vocoder.c); this file says which builds and where. Without a valid descriptor and codec,
// present() is false and the radio is FM only.
#include "../uv380_family/uvf.h"
#include "config.h"

#if CONFIG_DMR	// the whole file: DMR only
#include "hrc/vocoder.h"

#define CODEC_FLASH_LO		0x080C0000u	// sectors 10 and 11: the flash boards/mduv380/memory.ld leaves to the codec

// The id tools/rfoundry and the release manifests use for the codec (stockfw.py MDUV380_CODEC): one for every build,
// since it is the same codec, named after the first build supported
#define CODEC_ID		"mduv380-1926-s11"

// Entry offsets from the code start: the UV380 family's codec, the same in every build (codec-mduv380.md). Not used:
// encode ECC +0x4E8 and FEC decode (+0xB54, +0xB68 or +0xB78 by build). The air frame FEC is ours (core/dmr/ambe)
// and matches the radio's own
#define CODEC_ENTRIES { .encode = 0x130, .decode = 0x5D8, .enc_init = 0x284, .fec_init = 0x4CC, .dec_init = 0x6D0, \
		.enc_params = 0x848, .enc_set = 0x1C0, .dec_set = 0x688 }

// Each build as the merge places it (tools/rfoundry/rfoundry/stockfw.py, "placed"): the descriptor's tag (the build
// letter and version), the code and the tables with the CRC-32 (IEEE 802.3) of each after the rewrites. The descriptor
// must carry exactly these

// V19.26 CSV
static const codec_desc_window_t v1926_tables[] = { { 0x080D8C94u, 0x1266Au, 0xCE661968u } };
static const stm32_codec_profile_t v1926 = {
	.tag = 0x00561926u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080EB300u, .code_len = 0xF4BAu, .code_crc = 0x10AC40E0u, .tables = v1926_tables, .windows = 1,
	.at = CODEC_ENTRIES,
};

// D19.26 REC
static const codec_desc_window_t d1926_tables[] = { { 0x080DBC1Cu, 0x12672u, 0x7E080ADEu } };
static const stm32_codec_profile_t d1926 = {
	.tag = 0x00441926u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080EE290u, .code_len = 0xF4AAu, .code_crc = 0x3367A6D0u, .tables = d1926_tables, .windows = 1,
	.at = CODEC_ENTRIES,
};

// P19.26 CSV-GPS
static const codec_desc_window_t p1926_tables[] = { { 0x080DB054u, 0x1270Au, 0x06DB6E3Cu } };
static const stm32_codec_profile_t p1926 = {
	.tag = 0x00501926u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080ED760u, .code_len = 0xF4BAu, .code_crc = 0x0AEE6C4Au, .tables = p1926_tables, .windows = 1,
	.at = CODEC_ENTRIES,
};

// S19.26 GPS-REC
static const codec_desc_window_t s1926_tables[] = { { 0x080DE01Cu, 0x12712u, 0xDED111EFu } };
static const stm32_codec_profile_t s1926 = {
	.tag = 0x00531926u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080F0730u, .code_len = 0xF4B6u, .code_crc = 0x11E43041u, .tables = s1926_tables, .windows = 1,
	.at = CODEC_ENTRIES,
};

// V18.11 CSV
static const codec_desc_window_t v1811_tables[] = { { 0x080D5B34u, 0x121EEu, 0x28A30A27u } };
static const stm32_codec_profile_t v1811 = {
	.tag = 0x00561811u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080E7D24u, .code_len = 0xF6DEu, .code_crc = 0x0B1FFBD7u, .tables = v1811_tables, .windows = 1,
	.at = CODEC_ENTRIES,
};

// D18.11 REC
static const codec_desc_window_t d1811_tables[] = { { 0x080D8998u, 0x121F6u, 0x6249AF48u } };
static const stm32_codec_profile_t d1811 = {
	.tag = 0x00441811u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080EAB90u, .code_len = 0xF6D2u, .code_crc = 0x8DB9F023u, .tables = d1811_tables, .windows = 1,
	.at = CODEC_ENTRIES,
};

// P18.11 CSV-GPS
static const codec_desc_window_t p1811_tables[] = { { 0x080D7674u, 0x1228Eu, 0x1AC8D7F3u } };
static const stm32_codec_profile_t p1811 = {
	.tag = 0x00501811u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080E9904u, .code_len = 0xF6E2u, .code_crc = 0x229BC3B4u, .tables = p1811_tables, .windows = 1,
	.at = CODEC_ENTRIES,
};

// S18.11 GPS-REC
static const codec_desc_window_t s1811_tables[] = { { 0x080DA4D8u, 0x12296u, 0x87CA8BC7u } };
static const stm32_codec_profile_t s1811 = {
	.tag = 0x00531811u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080EC770u, .code_len = 0xF6C2u, .code_crc = 0x21EBA180u, .tables = s1811_tables, .windows = 1,
	.at = CODEC_ENTRIES,
};

// P18.16 CSV-GPS
static const codec_desc_window_t p1816_tables[] = { { 0x080D87F4u, 0x1228Eu, 0xD772CFADu } };
static const stm32_codec_profile_t p1816 = {
	.tag = 0x00501816u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080EAA84u, .code_len = 0xF6F6u, .code_crc = 0x9C51AF50u, .tables = p1816_tables, .windows = 1,
	.at = CODEC_ENTRIES,
};

// S18.16 GPS-REC
static const codec_desc_window_t s1816_tables[] = { { 0x080DB618u, 0x12296u, 0x3D1F111Cu } };
static const stm32_codec_profile_t s1816 = {
	.tag = 0x00531816u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080ED8B0u, .code_len = 0xF6D2u, .code_crc = 0x1986B8FFu, .tables = s1816_tables, .windows = 1,
	.at = CODEC_ENTRIES,
};

// V19.06 CSV
static const codec_desc_window_t v1906_tables[] = { { 0x080D70F4u, 0x12376u, 0x22393F1Bu } };
static const stm32_codec_profile_t v1906 = {
	.tag = 0x00561906u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080E946Cu, .code_len = 0xF6D6u, .code_crc = 0x25117C5Eu, .tables = v1906_tables, .windows = 1,
	.at = CODEC_ENTRIES,
};

// P19.06 CSV-GPS
static const codec_desc_window_t p1906_tables[] = { { 0x080D94B4u, 0x12416u, 0x02CB0AB8u } };
static const stm32_codec_profile_t p1906 = {
	.tag = 0x00501906u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080EB8CCu, .code_len = 0xF6DEu, .code_crc = 0xB18F0F57u, .tables = p1906_tables, .windows = 1,
	.at = CODEC_ENTRIES,
};

// V19.74 CSV
static const codec_desc_window_t v1974_tables[] = { { 0x080DA36Cu, 0x1272Eu, 0x605799E9u } };
static const stm32_codec_profile_t v1974 = {
	.tag = 0x00561974u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080ECA9Cu, .code_len = 0xF4A6u, .code_crc = 0x54149419u, .tables = v1974_tables, .windows = 1,
	.at = CODEC_ENTRIES,
};

// D19.74 REC
static const codec_desc_window_t d1974_tables[] = { { 0x080DD330u, 0x12746u, 0xAC72D37Eu } };
static const stm32_codec_profile_t d1974 = {
	.tag = 0x00441974u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080EFA78u, .code_len = 0xF4BAu, .code_crc = 0x1924F763u, .tables = d1974_tables, .windows = 1,
	.at = CODEC_ENTRIES,
};

// P19.74 CSV-GPS
static const codec_desc_window_t p1974_tables[] = { { 0x080DC8ECu, 0x127CEu, 0x6E6FD4E0u } };
static const stm32_codec_profile_t p1974 = {
	.tag = 0x00501974u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080EF0BCu, .code_len = 0xF49Au, .code_crc = 0x97C6E76Bu, .tables = p1974_tables, .windows = 1,
	.at = CODEC_ENTRIES,
};

// S19.74 GPS-REC
static const codec_desc_window_t s1974_tables[] = { { 0x080DF8B0u, 0x127D6u, 0x8414A4A9u } };
static const stm32_codec_profile_t s1974 = {
	.tag = 0x00531974u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080C0000u, .code_len = 0xF48Eu, .code_crc = 0x808DD9EEu, .tables = s1974_tables, .windows = 1,
	.at = CODEC_ENTRIES,
};

// 10 W V19.74 CSV
static const codec_desc_window_t w10_v1974_tables[] = { { 0x080DA2ACu, 0x1272Eu, 0xAC3B0A5Au } };
static const stm32_codec_profile_t w10_v1974 = {
	.tag = 0x40561974u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080EC9DCu, .code_len = 0xF492u, .code_crc = 0xE86E3C65u, .tables = w10_v1974_tables, .windows = 1,
	.at = CODEC_ENTRIES,
};

// 10 W D19.74 REC
static const codec_desc_window_t w10_d1974_tables[] = { { 0x080DD270u, 0x12746u, 0x3F3E347Bu } };
static const stm32_codec_profile_t w10_d1974 = {
	.tag = 0x40441974u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080EF9B8u, .code_len = 0xF4BAu, .code_crc = 0xAA99FD17u, .tables = w10_d1974_tables, .windows = 1,
	.at = CODEC_ENTRIES,
};

// 10 W P19.74 CSV-GPS
static const codec_desc_window_t w10_p1974_tables[] = { { 0x080DC86Cu, 0x127CEu, 0x3CA2A02Du } };
static const stm32_codec_profile_t w10_p1974 = {
	.tag = 0x40501974u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080EF03Cu, .code_len = 0xF4AEu, .code_crc = 0x1B9C0A2Bu, .tables = w10_p1974_tables, .windows = 1,
	.at = CODEC_ENTRIES,
};

// 10 W S19.74 GPS-REC
static const codec_desc_window_t w10_s1974_tables[] = { { 0x080DF870u, 0x127D6u, 0xC39C3679u } };
static const stm32_codec_profile_t w10_s1974 = {
	.tag = 0x40531974u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080C0000u, .code_len = 0xF4C2u, .code_crc = 0x92924D87u, .tables = w10_s1974_tables, .windows = 1,
	.at = CODEC_ENTRIES,
};

// V219.60 CSV AES 255
static const codec_desc_window_t v21960_tables[] = { { 0x080E322Cu, 0x1272Au, 0xE880FF1Bu } };
static const stm32_codec_profile_t v21960 = {
	.tag = 0x80561960u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080C0000u, .code_len = 0xF3BEu, .code_crc = 0x620F4AC5u, .tables = v21960_tables, .windows = 1,
	.at = CODEC_ENTRIES,
};

// P219.60 CSV-GPS AES 255
static const codec_desc_window_t p21960_tables[] = { { 0x080E57ACu, 0x127CAu, 0xC76CFEB9u } };
static const stm32_codec_profile_t p21960 = {
	.tag = 0x80501960u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080C0000u, .code_len = 0xF39Au, .code_crc = 0x307BE6B5u, .tables = p21960_tables, .windows = 1,
	.at = CODEC_ENTRIES,
};

// V219.74 CSV AES 255
static const codec_desc_window_t v21974_tables[] = { { 0x080E396Cu, 0x1278Au, 0xEF3C7105u } };
static const stm32_codec_profile_t v21974 = {
	.tag = 0x80561974u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080C0000u, .code_len = 0xF3B2u, .code_crc = 0x254A2827u, .tables = v21974_tables, .windows = 1,
	.at = CODEC_ENTRIES,
};

// D219.74 REC AES 255
static const codec_desc_window_t d21974_tables[] = { { 0x080E6774u, 0x127A2u, 0xE588E20Fu } };
static const stm32_codec_profile_t d21974 = {
	.tag = 0x80441974u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080C0000u, .code_len = 0xF3A6u, .code_crc = 0x3E5C5BC2u, .tables = d21974_tables, .windows = 1,
	.at = CODEC_ENTRIES,
};

// P219.74 CSV-GPS AES 255
static const codec_desc_window_t p21974_tables[] = { { 0x080E5EECu, 0x1282Au, 0x9418379Du } };
static const stm32_codec_profile_t p21974 = {
	.tag = 0x80501974u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080C0000u, .code_len = 0xF3B2u, .code_crc = 0xBA895AC5u, .tables = p21974_tables, .windows = 1,
	.at = CODEC_ENTRIES,
};

// S219.74 GPS-REC AES 255
static const codec_desc_window_t s21974_tables[] = { { 0x080E8D34u, 0x12832u, 0x0B8910A3u } };
static const stm32_codec_profile_t s21974 = {
	.tag = 0x80531974u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080C0000u, .code_len = 0xF392u, .code_crc = 0x87BCEDF2u, .tables = s21974_tables, .windows = 1,
	.at = CODEC_ENTRIES,
};

// 10 W V219.74 CSV AES 255
static const codec_desc_window_t w10_v21974_tables[] = { { 0x080E38ACu, 0x1278Au, 0x5C092FDDu } };
static const stm32_codec_profile_t w10_v21974 = {
	.tag = 0xC0561974u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080C0000u, .code_len = 0xF3A6u, .code_crc = 0x6A67A328u, .tables = w10_v21974_tables, .windows = 1,
	.at = CODEC_ENTRIES,
};

// 10 W D219.74 REC AES 255
static const codec_desc_window_t w10_d21974_tables[] = { { 0x080E66B4u, 0x127A2u, 0x60ACDF51u } };
static const stm32_codec_profile_t w10_d21974 = {
	.tag = 0xC0441974u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080C0000u, .code_len = 0xF3AAu, .code_crc = 0xD68C85D5u, .tables = w10_d21974_tables, .windows = 1,
	.at = CODEC_ENTRIES,
};

// 10 W P219.74 CSV-GPS AES 255
static const codec_desc_window_t w10_p21974_tables[] = { { 0x080E5E6Cu, 0x1282Au, 0xE429F823u } };
static const stm32_codec_profile_t w10_p21974 = {
	.tag = 0xC0501974u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080C0000u, .code_len = 0xF3BAu, .code_crc = 0x84C3921Cu, .tables = w10_p21974_tables, .windows = 1,
	.at = CODEC_ENTRIES,
};

// 10 W S219.74 GPS-REC AES 255
static const codec_desc_window_t w10_s21974_tables[] = { { 0x080E8CB4u, 0x12832u, 0x64B8EF5Cu } };
static const stm32_codec_profile_t w10_s21974 = {
	.tag = 0xC0531974u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080C0000u, .code_len = 0xF38Eu, .code_crc = 0x4703BB9Fu, .tables = w10_s21974_tables, .windows = 1,
	.at = CODEC_ENTRIES,
};

// 19.26 first: the builds most radios carry
static const stm32_codec_profile_t *const profiles[] = {
	&v1926, &d1926, &p1926, &s1926, &v1811, &d1811, &p1811, &s1811, &p1816, &s1816, &v1906, &p1906, &v1974,
	&d1974, &p1974, &s1974, &w10_v1974, &w10_d1974, &w10_p1974, &w10_s1974, &v21960, &p21960, &v21974, &d21974,
	&p21974, &s21974, &w10_v21974, &w10_d21974, &w10_p21974, &w10_s21974
};
const stm32_codec_profiles_t stm32_codec_profiles = { profiles, (uint8_t)(sizeof profiles / sizeof profiles[0]) };

#endif	// CONFIG_DMR
