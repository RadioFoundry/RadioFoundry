// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// MD-9600 vocoder: the radio's own stock AMBE+2 codec, run from flash (docs/spec/codec-md9600.md). Every TYT and Retevis
// build of the codec that has a map is accepted (the profiles below, one per original firmware file: 03.28, 03.29,
// 04.03, 06.03, 06.09, 06.24, 16.24, 16.45, 26.38, 26.41, 26.42, 26.45 and 26.57, each in the CSV, CSV-GPS, REC and
// GPS-REC variants it exists in); the codec is board-independent, so any of them serves every revision. The calls are
// the other STM32F4 hosts', at the same entry offsets (platform/stm32f4/hrc/vocoder.c).
//
// The merge on the user's machine (rfoundry, tools/codec_extract.py, the web flasher) writes the codec's table window at
// its stock address (sector 9, and a few bytes into sector 10 for the largest builds; the addresses differ, so a profile
// names its own), the code at the start of sector 11 (0x080E0000) and the descriptor at the end of sector 11
// (platform/stm32f4/codec_desc.h). The code reaches its tables through absolute addresses, which don't change. The merge
// rewrites what does: the code's pointers into itself and the calls from the island in the table window back into the
// code (the codec map). None of it is part of this repository or a release. The calls and the checks are the shared
// ones; this file says which build and where. Without a valid descriptor and codec, present() is false and the radio is
// FM only.
#include "md9600.h"
#include "config.h"

#if CONFIG_DMR	// the whole file: DMR only
#include "hrc/vocoder.h"

#define CODEC_FLASH_LO		0x080A0000u	// sectors 9-11: the flash boards/md9600/memory.ld leaves to the codec
#define CODEC_CODE		0x080E0000u	// sector 11
#define CODEC_ID		"md9600-s11"	// one id for every build, as rfoundry and the release manifests use it

// Not used: encode ECC +0x4E8 and FEC decode (+0xB78, +0xB68, +0xB64 or +0xB54 by build). The air frame FEC is ours
// (core/dmr/ambe); every build has the other entries at these offsets
#define ENTRIES { .encode = 0x130, .decode = 0x5D8, .enc_init = 0x284, .fec_init = 0x4CC, .dec_init = 0x6D0, \
		  .enc_params = 0x848, .enc_set = 0x1C0, .dec_set = 0x688 }

// Each build as the merge places it (tools/rfoundry/rfoundry/stockfw.py, "placed"): CRC-32 (IEEE 802.3) of the code and
// of the table window after the rewrites, and the code window's length. The descriptor must carry exactly these. The tag
// is the variant in bits 16-17 (0 CSV, 1 CSV-GPS, 2 REC, 3 GPS-REC) and the version in BCD below (0x1645: 16.45 CSV)
//
// The two builds first accepted: 06.09 CSV (V1-V3 boards) and 16.45 CSV (V4)
static const codec_desc_window_t tables_v0609[1] = { { 0x080AB13Cu, 0x10FE6u, 0x9983906Au } };
static const codec_desc_window_t tables_v1645[1] = { { 0x080AC2BCu, 0x11036u, 0xA8A52935u } };

static const stm32_codec_profile_t v0609 = {
	.tag = 0x0609u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO, .code = CODEC_CODE, .code_len = 0xF214u,
	.code_crc = 0xD7D67AE8u, .tables = tables_v0609, .windows = 1, .at = ENTRIES,
};

static const stm32_codec_profile_t v1645 = {
	.tag = 0x1645u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO, .code = CODEC_CODE, .code_len = 0xF214u,
	.code_crc = 0xD5E2E873u, .tables = tables_v1645, .windows = 1, .at = ENTRIES,
};

// The other original firmware files (the older builds are 03.xx to 06.xx, the V4 and V5 ones 16.xx and 26.xx). The
// largest GPS-REC builds' tables run into sector 10, which the board keeps free for it
// MD9600-CSV-GPS-P06.09.bin (P06.09 CSV-GPS)
static const codec_desc_window_t m0609p_tables[1] = { { 0x080AC6FCu, 0x11066u, 0x7293248Au } };
static const stm32_codec_profile_t m0609p = {
	.tag = 0x10609u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO, .code = CODEC_CODE, .code_len = 0xF234u,
	.code_crc = 0x92742CD3u, .tables = m0609p_tables, .windows = 1, .at = ENTRIES,
};

// MD9600-REC-D06.09.bin (D06.09 REC)
static const codec_desc_window_t m0609d_tables[1] = { { 0x080AD660u, 0x10FF6u, 0x678DE46Bu } };
static const stm32_codec_profile_t m0609d = {
	.tag = 0x20609u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO, .code = CODEC_CODE, .code_len = 0xF210u,
	.code_crc = 0x1E46EE7Du, .tables = m0609d_tables, .windows = 1, .at = ENTRIES,
};

// MD9600-GPS-REC-S06.09.bin (S06.09 GPS-REC)
static const codec_desc_window_t m0609s_tables[1] = { { 0x080AEC60u, 0x11076u, 0x16BC466Fu } };
static const stm32_codec_profile_t m0609s = {
	.tag = 0x30609u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO, .code = CODEC_CODE, .code_len = 0xF22Cu,
	.code_crc = 0x8B3C7444u, .tables = m0609s_tables, .windows = 1, .at = ENTRIES,
};

// MD9600-CSV-V06.24.bin (V06.24 CSV)
static const codec_desc_window_t m0624v_tables[1] = { { 0x080AB2BCu, 0x10FE6u, 0x2876271Fu } };
static const stm32_codec_profile_t m0624v = {
	.tag = 0x0624u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO, .code = CODEC_CODE, .code_len = 0xF232u,
	.code_crc = 0x0BD96104u, .tables = m0624v_tables, .windows = 1, .at = ENTRIES,
};

// MD9600-CSV-GPS-P06.24.bin (P06.24 CSV-GPS)
static const codec_desc_window_t m0624p_tables[1] = { { 0x080AC9FCu, 0x11066u, 0x6F4DC5D8u } };
static const stm32_codec_profile_t m0624p = {
	.tag = 0x10624u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO, .code = CODEC_CODE, .code_len = 0xF21Eu,
	.code_crc = 0x59308C6Eu, .tables = m0624p_tables, .windows = 1, .at = ENTRIES,
};

// MD9600-REC-D06.24.bin (D06.24 REC)
static const codec_desc_window_t m0624d_tables[1] = { { 0x080AD7E0u, 0x10FF6u, 0x310E2E1Au } };
static const stm32_codec_profile_t m0624d = {
	.tag = 0x20624u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO, .code = CODEC_CODE, .code_len = 0xF22Eu,
	.code_crc = 0x9F66F4A5u, .tables = m0624d_tables, .windows = 1, .at = ENTRIES,
};

// MD9600-GPS-REC-S06.24.bin (S06.24 GPS-REC)
static const codec_desc_window_t m0624s_tables[1] = { { 0x080AEF60u, 0x11076u, 0x51A89621u } };
static const stm32_codec_profile_t m0624s = {
	.tag = 0x30624u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO, .code = CODEC_CODE, .code_len = 0xF216u,
	.code_crc = 0x2B2D9386u, .tables = m0624s_tables, .windows = 1, .at = ENTRIES,
};

// MD9600-CSV-GPS(V4)-P16.45.bin (P16.45 CSV-GPS)
static const codec_desc_window_t m1645p_tables[1] = { { 0x080ADA7Cu, 0x110B6u, 0xED224602u } };
static const stm32_codec_profile_t m1645p = {
	.tag = 0x11645u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO, .code = CODEC_CODE, .code_len = 0xF238u,
	.code_crc = 0xB39222A8u, .tables = m1645p_tables, .windows = 1, .at = ENTRIES,
};

// MD9600-REC(V4)-D16.45.bin (D16.45 REC)
static const codec_desc_window_t m1645d_tables[1] = { { 0x080AE820u, 0x11046u, 0xDB4CA8C2u } };
static const stm32_codec_profile_t m1645d = {
	.tag = 0x21645u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO, .code = CODEC_CODE, .code_len = 0xF234u,
	.code_crc = 0x8838CEB2u, .tables = m1645d_tables, .windows = 1, .at = ENTRIES,
};

// MD9600-GPS-REC(V4)-S16.45.bin (S16.45 GPS-REC)
static const codec_desc_window_t m1645s_tables[1] = { { 0x080AFFE0u, 0x110C6u, 0x08804876u } };
static const stm32_codec_profile_t m1645s = {
	.tag = 0x31645u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO, .code = CODEC_CODE, .code_len = 0xF1FCu,
	.code_crc = 0x3E27241Au, .tables = m1645s_tables, .windows = 1, .at = ENTRIES,
};

// MD9600-CSV(V4)-V16.24.bin (V16.24 CSV)
static const codec_desc_window_t m1624v_tables[1] = { { 0x080AB5FCu, 0x10FE6u, 0x1A2262A1u } };
static const stm32_codec_profile_t m1624v = {
	.tag = 0x1624u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO, .code = CODEC_CODE, .code_len = 0xF23Au,
	.code_crc = 0x3E488686u, .tables = m1624v_tables, .windows = 1, .at = ENTRIES,
};

// MD9600-CSV-GPS(V4)-P16.24.bin (P16.24 CSV-GPS)
static const codec_desc_window_t m1624p_tables[1] = { { 0x080ACD3Cu, 0x11066u, 0xFB1BA990u } };
static const stm32_codec_profile_t m1624p = {
	.tag = 0x11624u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO, .code = CODEC_CODE, .code_len = 0xF226u,
	.code_crc = 0x56A6DF63u, .tables = m1624p_tables, .windows = 1, .at = ENTRIES,
};

// MD9600-REC(V4)-D16.24.bin (D16.24 REC)
static const codec_desc_window_t m1624d_tables[1] = { { 0x080ADB20u, 0x10FF6u, 0x5CB7686Au } };
static const stm32_codec_profile_t m1624d = {
	.tag = 0x21624u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO, .code = CODEC_CODE, .code_len = 0xF236u,
	.code_crc = 0x474FE24Eu, .tables = m1624d_tables, .windows = 1, .at = ENTRIES,
};

// MD9600-GPS-REC(V4)-S16.24.bin (S16.24 GPS-REC)
static const codec_desc_window_t m1624s_tables[1] = { { 0x080AF2A0u, 0x11076u, 0x0F3948D1u } };
static const stm32_codec_profile_t m1624s = {
	.tag = 0x31624u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO, .code = CODEC_CODE, .code_len = 0xF21Eu,
	.code_crc = 0x0C0CE848u, .tables = m1624s_tables, .windows = 1, .at = ENTRIES,
};

// MD9600-CSV(2571V5)-V26.45.bin (V26.45 CSV)
static const codec_desc_window_t m2645v_tables[1] = { { 0x080AB9BCu, 0x11036u, 0x45AEA030u } };
static const stm32_codec_profile_t m2645v = {
	.tag = 0x2645u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO, .code = CODEC_CODE, .code_len = 0xF22Eu,
	.code_crc = 0x6C1C3CDAu, .tables = m2645v_tables, .windows = 1, .at = ENTRIES,
};

// MD9600-CSV-GPS(2571V5)-P26.45.bin (P26.45 CSV-GPS)
static const codec_desc_window_t m2645p_tables[1] = { { 0x080AD13Cu, 0x110B6u, 0xF3525F5Au } };
static const stm32_codec_profile_t m2645p = {
	.tag = 0x12645u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO, .code = CODEC_CODE, .code_len = 0xF212u,
	.code_crc = 0xA161DDD6u, .tables = m2645p_tables, .windows = 1, .at = ENTRIES,
};

// MD9600-REC(2571V5)-D26.45.bin (D26.45 REC)
static const codec_desc_window_t m2645d_tables[1] = { { 0x080ADEE0u, 0x11046u, 0x23760EF0u } };
static const stm32_codec_profile_t m2645d = {
	.tag = 0x22645u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO, .code = CODEC_CODE, .code_len = 0xF20Eu,
	.code_crc = 0xCC9B0015u, .tables = m2645d_tables, .windows = 1, .at = ENTRIES,
};

// MD9600-GPS-REC(2571V5)-S26.45.bin (S26.45 GPS-REC)
static const codec_desc_window_t m2645s_tables[1] = { { 0x080AF6E0u, 0x110C6u, 0xEFEACF83u } };
static const stm32_codec_profile_t m2645s = {
	.tag = 0x32645u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO, .code = CODEC_CODE, .code_len = 0xF216u,
	.code_crc = 0xC1FD5D9Bu, .tables = m2645s_tables, .windows = 1, .at = ENTRIES,
};

// MD9600-REC(2571V4)-D26.57.bin (D26.57 REC)
static const codec_desc_window_t m2657d_tables[1] = { { 0x080AE5E0u, 0x11032u, 0x503BFE08u } };
static const stm32_codec_profile_t m2657d = {
	.tag = 0x22657u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO, .code = CODEC_CODE, .code_len = 0xF1EEu,
	.code_crc = 0x4ADE21EAu, .tables = m2657d_tables, .windows = 1, .at = ENTRIES,
};

// MD9600-CSV-V06.03.bin (V06.03 CSV)
static const codec_desc_window_t m0603v_tables[1] = { { 0x080AACFCu, 0x10FE6u, 0x886A04C5u } };
static const stm32_codec_profile_t m0603v = {
	.tag = 0x0603u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO, .code = CODEC_CODE, .code_len = 0xF23Au,
	.code_crc = 0xAC0A6C89u, .tables = m0603v_tables, .windows = 1, .at = ENTRIES,
};

// MD9600-CSV-GPS-P06.03.bin (P06.03 CSV-GPS)
static const codec_desc_window_t m0603p_tables[1] = { { 0x080AC2BCu, 0x11066u, 0xDF5D6B0Cu } };
static const stm32_codec_profile_t m0603p = {
	.tag = 0x10603u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO, .code = CODEC_CODE, .code_len = 0xF234u,
	.code_crc = 0xA8BC5920u, .tables = m0603p_tables, .windows = 1, .at = ENTRIES,
};

// MD9600-REC-D06.03.bin (D06.03 REC)
static const codec_desc_window_t m0603d_tables[1] = { { 0x080AD1E0u, 0x10FF6u, 0x41B67AF0u } };
static const stm32_codec_profile_t m0603d = {
	.tag = 0x20603u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO, .code = CODEC_CODE, .code_len = 0xF210u,
	.code_crc = 0x2463B336u, .tables = m0603d_tables, .windows = 1, .at = ENTRIES,
};

// MD9600-GPS-REC-S06.03.bin (S06.03 GPS-REC)
static const codec_desc_window_t m0603s_tables[1] = { { 0x080AE820u, 0x11076u, 0x229CF445u } };
static const stm32_codec_profile_t m0603s = {
	.tag = 0x30603u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO, .code = CODEC_CODE, .code_len = 0xF22Eu,
	.code_crc = 0x28F39772u, .tables = m0603s_tables, .windows = 1, .at = ENTRIES,
};

// MD9600-REC-D03.28.bin (D03.28 REC)
static const codec_desc_window_t m0328d_tables[1] = { { 0x080A9F5Cu, 0x10C8Eu, 0x7B2C05F7u } };
static const stm32_codec_profile_t m0328d = {
	.tag = 0x20328u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO, .code = CODEC_CODE, .code_len = 0x102F6u,
	.code_crc = 0xF1CF88D9u, .tables = m0328d_tables, .windows = 1, .at = ENTRIES,
};

// MD9600-REC-D03.29.bin (D03.29 REC)
static const codec_desc_window_t m0329d_tables[1] = { { 0x080A9F5Cu, 0x10C8Eu, 0xCEFD3850u } };
static const stm32_codec_profile_t m0329d = {
	.tag = 0x20329u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO, .code = CODEC_CODE, .code_len = 0x102EAu,
	.code_crc = 0x30EBC7BCu, .tables = m0329d_tables, .windows = 1, .at = ENTRIES,
};

// MD9600-REC-D04.03.bin (D04.03 REC)
static const codec_desc_window_t m0403d_tables[1] = { { 0x080AA25Cu, 0x10C7Eu, 0xF12A738Eu } };
static const stm32_codec_profile_t m0403d = {
	.tag = 0x20403u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO, .code = CODEC_CODE, .code_len = 0x102DEu,
	.code_crc = 0x079D54DEu, .tables = m0403d_tables, .windows = 1, .at = ENTRIES,
};

// RT90-CSV(2571V5)-V26.41.bin (V26.41 CSV)
static const codec_desc_window_t m2641v_tables[1] = { { 0x080AB7FCu, 0x11036u, 0xB2F17F8Fu } };
static const stm32_codec_profile_t m2641v = {
	.tag = 0x2641u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO, .code = CODEC_CODE, .code_len = 0xF214u,
	.code_crc = 0x60C83344u, .tables = m2641v_tables, .windows = 1, .at = ENTRIES,
};

// RT90-CSV-GPS(2571V5)-P26.41.bin (P26.41 CSV-GPS)
static const codec_desc_window_t m2641p_tables[1] = { { 0x080ACFBCu, 0x110B6u, 0xF8C5439Du } };
static const stm32_codec_profile_t m2641p = {
	.tag = 0x12641u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO, .code = CODEC_CODE, .code_len = 0xF234u,
	.code_crc = 0x509B33F4u, .tables = m2641p_tables, .windows = 1, .at = ENTRIES,
};

// RT90-REC(2571V5)-D26.38.bin (D26.38 REC)
static const codec_desc_window_t m2638d_tables[1] = { { 0x080ADB60u, 0x1100Eu, 0xEF601161u } };
static const stm32_codec_profile_t m2638d = {
	.tag = 0x22638u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO, .code = CODEC_CODE, .code_len = 0xF210u,
	.code_crc = 0x1EB20C9Au, .tables = m2638d_tables, .windows = 1, .at = ENTRIES,
};

// RT90-GPS-REC(2571V5)-S26.42.bin (S26.42 GPS-REC)
static const codec_desc_window_t m2642s_tables[1] = { { 0x080AF5E0u, 0x110C6u, 0xDD55386Au } };
static const stm32_codec_profile_t m2642s = {
	.tag = 0x32642u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO, .code = CODEC_CODE, .code_len = 0xF22Cu,
	.code_crc = 0x3BDF7D22u, .tables = m2642s_tables, .windows = 1, .at = ENTRIES,
};


static const stm32_codec_profile_t *const profiles[] = {
	&v0609, &v1645, &m0609p, &m0609d, &m0609s, &m0624v,
	&m0624p, &m0624d, &m0624s, &m1645p, &m1645d, &m1645s,
	&m1624v, &m1624p, &m1624d, &m1624s, &m2645v, &m2645p,
	&m2645d, &m2645s, &m2657d, &m0603v, &m0603p, &m0603d,
	&m0603s, &m0328d, &m0329d, &m0403d, &m2641v, &m2641p, &m2638d, &m2642s,
};
const stm32_codec_profiles_t stm32_codec_profiles = { profiles, 32 };

#endif	// CONFIG_DMR
