// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// MD-9600 vocoder: the radio's own stock AMBE+2 codec, run from flash (docs/spec/codec-md9600.md). Two builds are
// accepted, 06.09 CSV (V1-V3 boards) and 16.45 CSV (V4); the codec is board-independent, so either serves every
// revision. The calls are the DM-1701's, at the same entry offsets (platform/stm32f4/hrc/vocoder.c).
//
// The merge on the user's machine (odrflash, tools/codec_extract.py, the web flasher) writes the codec's table window at
// its stock address (sector 9; the two builds' addresses differ, so a profile names its own), the code at the start of
// sector 11 (0x080E0000) and the descriptor at the end of sector 11 (platform/stm32f4/codec_desc.h). The code reaches its
// tables through absolute addresses, which don't change. The merge rewrites what does: the code's pointers into itself
// and the calls from the island in the table window back into the code (the codec map). None of it is part of this
// repository or a release. The calls and the checks are the shared ones; this file says which build and where. Without
// a valid descriptor and codec, present() is false and the radio is FM only.
#include "md9600.h"
#include "config.h"

#if CONFIG_DMR	// the whole file: DMR only
#include "hrc/vocoder.h"

#define CODEC_FLASH_LO		0x08080000u	// sectors 8-11: the flash md9600.ld leaves to the codec
#define CODEC_CODE		0x080E0000u	// sector 11
#define CODE_LEN		0xF214u		// both builds' code window

// Not used: encode ECC +0x4E8 and FEC decode (+0xB78 in both builds). The air frame FEC is ours (core/dmr/ambe)
#define ENTRIES { .encode = 0x130, .decode = 0x5D8, .enc_init = 0x284, .fec_init = 0x4CC, .dec_init = 0x6D0, \
		  .enc_params = 0x848, .enc_set = 0x1C0, .dec_set = 0x688 }

// Each build as the merge places it (tools/odrflash/odrflash/stockfw.py, "placed"): CRC-32 (IEEE 802.3) of the code and
// of the table window after the rewrites. The descriptor must carry exactly these
static const codec_desc_window_t tables_v0609[1] = { { 0x080AB13Cu, 0x10FE6u, 0x9983906Au } };
static const codec_desc_window_t tables_v1645[1] = { { 0x080AC2BCu, 0x11036u, 0xA8A52935u } };

// One id for both builds, as odrflash and the release manifests use it (the tag tells the builds apart). A build from
// before the move would report another id
static const stm32_codec_profile_t v0609 = {
	.tag = 0x0609u, .id = "md9600-s11", .flash_lo = CODEC_FLASH_LO, .code = CODEC_CODE, .code_len = CODE_LEN,
	.code_crc = 0xD7D67AE8u, .tables = tables_v0609, .windows = 1, .at = ENTRIES,
};

static const stm32_codec_profile_t v1645 = {
	.tag = 0x1645u, .id = "md9600-s11", .flash_lo = CODEC_FLASH_LO, .code = CODEC_CODE, .code_len = CODE_LEN,
	.code_crc = 0xD5E2E873u, .tables = tables_v1645, .windows = 1, .at = ENTRIES,
};

static const stm32_codec_profile_t *const profiles[] = { &v0609, &v1645 };
const stm32_codec_profiles_t stm32_codec_profiles = { profiles, 2 };

#endif	// CONFIG_DMR
