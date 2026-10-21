// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// MD-9600 vocoder: the radio's own stock AMBE+2 codec, run from flash (docs/spec/codec-md9600.md). Two builds are
// accepted, 06.09 CSV (V1-V3 boards) and 16.45 CSV (V4); the codec is board-independent, so either serves every
// revision. The calls are the DM-1701's, at the same entry offsets (platform/stm32f4/hrc/vocoder.c).
//
// Where the merge puts the codec is not settled (codec-md9600.md, "Hosting"): its stock code window starts in sector 7,
// which md9600.ld gives to the application, so the code has to move into sectors 8-11 (with the rewrites a move needs,
// as on the DM-1701) or the application has to end below it. Until then a profile pins only the build's tag and code
// length, and the flash the linker script leaves to the codec (sectors 8-11); the code's address and every window's
// CRC are the descriptor's, which present() checks against the flash. None of the codec is part of this repository or
// a release. Without a valid descriptor and codec, present() is false and the radio is FM only.
#include "md9600.h"
#include "config.h"

#if CONFIG_DMR	// the whole file: DMR only
#include "hrc/vocoder.h"

#define CODEC_FLASH_LO		0x08080000u	// sectors 8-11: the flash md9600.ld leaves to the codec
#define CODE_LEN		0xF214u		// both builds' code window

// Not used: encode ECC +0x4E8 and FEC decode (+0xB78 in both builds). The air frame FEC is ours (core/dmr/ambe)
#define ENTRIES { .encode = 0x130, .decode = 0x5D8, .enc_init = 0x284, .fec_init = 0x4CC, .dec_init = 0x6D0, \
		  .enc_params = 0x848, .enc_set = 0x1C0, .dec_set = 0x688 }

// Ids for tools/odrflash and the release manifests (proposed in codec-md9600.md)
static const stm32_codec_profile_t v0609 = {
	.tag = 0x0609u, .id = "md9600-v0609-csv", .flash_lo = CODEC_FLASH_LO, .code_len = CODE_LEN, .at = ENTRIES,
};

static const stm32_codec_profile_t v1645 = {
	.tag = 0x1645u, .id = "md9600-v1645-csv", .flash_lo = CODEC_FLASH_LO, .code_len = CODE_LEN, .at = ENTRIES,
};

static const stm32_codec_profile_t *const profiles[] = { &v0609, &v1645 };
const stm32_codec_profiles_t stm32_codec_profiles = { profiles, 2 };

#endif	// CONFIG_DMR
