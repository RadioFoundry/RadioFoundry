// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// MD-2017 vocoder: the radio's own AMBE+2 codec run in place from flash (docs/spec/codec-placement.md, "STM32 radios";
// the codec: docs/spec/codec-md2017.md). Any build listed below can be the donor (TYT 4.05, Retevis RT82 4.15 and
// EXXT_V2); they hold the UV380 family's codec at their own addresses. The merge (rfoundry, tools/codec_extract.py, the
// web flasher) writes the codec's tables at their address in that build (cut to the codec's own data), its code right
// after them, word aligned, and the descriptor at the end of sector 11 (platform/stm32f4/codec_desc.h). The calls and
// the checks are the shared ones (platform/stm32f4/hrc/vocoder.c); this file says which builds and where. Without a
// valid descriptor and codec, present() is false and the radio is FM only.
#include "../uv380_family/uvf.h"
#include "config.h"

#if CONFIG_DMR	// the whole file: DMR only
#include "hrc/vocoder.h"

#define CODEC_FLASH_LO		0x080C0000u	// sectors 10 and 11: the flash boards/md2017/memory.ld leaves to the codec

// The id tools/rfoundry and the release manifests use for the codec (stockfw.py MD2017_CODEC): one for every build
#define CODEC_ID		"md2017-405-s11"

// Entry offsets from the code start: the UV380 family's codec, the same in every build (codec-md2017.md)
#define CODEC_ENTRIES { .encode = 0x130, .decode = 0x5D8, .enc_init = 0x284, .fec_init = 0x4CC, .dec_init = 0x6D0, \
		.enc_params = 0x848, .enc_set = 0x1C0, .dec_set = 0x688 }

// Each build as the merge places it (tools/rfoundry/rfoundry/stockfw.py, "placed"): the descriptor's tag (the build
// letter and version), the code and the tables with the CRC-32 of each after the rewrites. EXXT_V2 holds P4.05's codec
// byte for byte and takes its profile

// V4.05 CSV
static const codec_desc_window_t v405_tables[] = { { 0x080D2AF4u, 0x11F56u, 0x08F19F1Du } };
static const stm32_codec_profile_t v405 = {
	.tag = 0x00560405u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080E4A4Cu, .code_len = 0xF6E2u, .code_crc = 0xA0E05F0Cu, .tables = v405_tables, .windows = 1,
	.at = CODEC_ENTRIES,
};

// D4.05 REC
static const codec_desc_window_t d405_tables[] = { { 0x080D53D8u, 0x11F5Eu, 0x59ED45C3u } };
static const stm32_codec_profile_t d405 = {
	.tag = 0x00440405u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080E7338u, .code_len = 0xF6EAu, .code_crc = 0x1B55116Bu, .tables = d405_tables, .windows = 1,
	.at = CODEC_ENTRIES,
};

// P4.05 CSV-GPS
static const codec_desc_window_t p405_tables[] = { { 0x080D4474u, 0x11FD6u, 0x0DB2A471u } };
static const stm32_codec_profile_t p405 = {
	.tag = 0x00500405u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080E644Cu, .code_len = 0xF706u, .code_crc = 0x835BC708u, .tables = p405_tables, .windows = 1,
	.at = CODEC_ENTRIES,
};

// S4.05 GPS-REC
static const codec_desc_window_t s405_tables[] = { { 0x080D6D18u, 0x11FEEu, 0xE4E5B155u } };
static const stm32_codec_profile_t s405 = {
	.tag = 0x00530405u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080E8D08u, .code_len = 0xF6D6u, .code_crc = 0xF2A3A5FBu, .tables = s405_tables, .windows = 1,
	.at = CODEC_ENTRIES,
};

// V4.15 CSV
static const codec_desc_window_t v415_tables[] = { { 0x080D3BF4u, 0x11F4Au, 0x8DE04C0Bu } };
static const stm32_codec_profile_t v415 = {
	.tag = 0x00560415u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080E5B40u, .code_len = 0xF6DEu, .code_crc = 0x32156970u, .tables = v415_tables, .windows = 1,
	.at = CODEC_ENTRIES,
};

// D4.15 REC
static const codec_desc_window_t d415_tables[] = { { 0x080D6798u, 0x11F52u, 0xC9E8AC09u } };
static const stm32_codec_profile_t d415 = {
	.tag = 0x00440415u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080E86ECu, .code_len = 0xF6D2u, .code_crc = 0x89FF7A8Bu, .tables = d415_tables, .windows = 1,
	.at = CODEC_ENTRIES,
};

// P4.15 CSV-GPS
static const codec_desc_window_t p415_tables[] = { { 0x080D55F4u, 0x11FCAu, 0x473DC5EEu } };
static const stm32_codec_profile_t p415 = {
	.tag = 0x00500415u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080E75C0u, .code_len = 0xF6FAu, .code_crc = 0x79810032u, .tables = p415_tables, .windows = 1,
	.at = CODEC_ENTRIES,
};

// S4.15 GPS-REC
static const codec_desc_window_t s415_tables[] = { { 0x080D8198u, 0x11FE2u, 0xEAB1A997u } };
static const stm32_codec_profile_t s415 = {
	.tag = 0x00530415u, .id = CODEC_ID, .flash_lo = CODEC_FLASH_LO,
	.code = 0x080EA17Cu, .code_len = 0xF6F2u, .code_crc = 0x164FDD7Cu, .tables = s415_tables, .windows = 1,
	.at = CODEC_ENTRIES,
};

static const stm32_codec_profile_t *const profiles[] = {
	&v405, &d405, &p405, &s405, &v415, &d415, &p415, &s415
};
const stm32_codec_profiles_t stm32_codec_profiles = { profiles, (uint8_t)(sizeof profiles / sizeof profiles[0]) };

#endif	// CONFIG_DMR
