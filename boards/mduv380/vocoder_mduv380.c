// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// MD-UV380 vocoder: the radio's own AMBE+2 codec run in place from flash (docs/spec/codec-placement.md, "STM32 radios";
// the codec: docs/spec/codec-mduv380.md). Any of the four TYT 19.26 builds can be the donor; they hold the same codec at
// their own addresses. The merge on the user's machine (rfoundry, tools/codec_extract.py, the web flasher) writes the
// codec's tables at their address in that build (cut to the codec's own data), its code right after them, word
// aligned, and the descriptor at the end of sector 11 (platform/stm32f4/codec_desc.h). The code reaches its tables
// through absolute addresses, which don't change; the merge rewrites one pointer into the code and the 12 calls from
// the code kept in the table window back into the code (the codec map). The calls and the checks are the shared ones
// (platform/stm32f4/hrc/vocoder.c); this file says which builds and where. Without a valid descriptor and codec,
// present() is false and the radio is FM only.
#include "../uv380_family/uvf.h"
#include "config.h"

#if CONFIG_DMR	// the whole file: DMR only
#include "hrc/vocoder.h"

#define CODEC_FLASH_LO		0x080C0000u	// sectors 10 and 11: the flash boards/mduv380/memory.ld leaves to the codec

// The id tools/rfoundry and the release manifests use for the codec (stockfw.py MDUV380_CODEC): one for all four
// builds, since it is the same codec
#define CODEC_ID		"mduv380-1926-s11"

// Entry offsets from the code start: the UV380 family's codec (codec-mduv380.md). Not used: encode ECC +0x4E8 and
// FEC decode +0xB54. The air frame FEC is ours (core/dmr/ambe) and matches the radio's own
#define CODEC_ENTRIES { .encode = 0x130, .decode = 0x5D8, .enc_init = 0x284, .fec_init = 0x4CC, .dec_init = 0x6D0, \
		.enc_params = 0x848, .enc_set = 0x1C0, .dec_set = 0x688 }

// Each build as the merge places it (tools/rfoundry/rfoundry/stockfw.py, "placed"): the descriptor's tag (the build
// letter and 1926), the code and the tables with the CRC-32 (IEEE 802.3) of each after the rewrites. The descriptor
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

static const stm32_codec_profile_t *const profiles[] = { &v1926, &d1926, &p1926, &s1926 };
const stm32_codec_profiles_t stm32_codec_profiles = { profiles, 4 };

#endif	// CONFIG_DMR
