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
#include "hrc/vocoder.h"

#define CODEC_TAG		0x0206u		// the descriptor's tag for stock V02.06 CSV
#define CODEC_FLASH_LO		0x080C0000u	// sectors 10 and 11: the flash boards/dm1701/memory.ld leaves to the codec

// The V02.06 CSV codec as the merge places it (tools/odrflash/odrflash/stockfw.py, "placed"): CRC-32 (IEEE 802.3) of
// the code and tables after the rewrites. The descriptor must carry exactly these
#define CODEC_CODE		0x080E3A74u
#define CODEC_CODE_LEN		0x1021Cu
#define CODEC_CRC		0x485BFC13u
#define CODEC_TABLES		0x080D19F0u
#define CODEC_TABLES_LEN	0x12082u
#define CODEC_TABLES_CRC	0xC1EEFA8Bu

static const codec_desc_window_t tables[] = { { CODEC_TABLES, CODEC_TABLES_LEN, CODEC_TABLES_CRC } };

// The id tools/odrflash and the release manifests use for this codec: V02.06 CSV placed as above (the descriptor and
// both CRCs matched). A build from before the move reports "dm1701-v0206-csv", which tells odrflash that the codec
// isn't where this layout keeps it
static const stm32_codec_profile_t v0206 = {
	.tag = CODEC_TAG, .id = "dm1701-v0206-csv-s11", .flash_lo = CODEC_FLASH_LO,
	.code = CODEC_CODE, .code_len = CODEC_CODE_LEN, .code_crc = CODEC_CRC, .tables = tables, .windows = 1,
	// Not used: encode ECC +0x4E8 and FEC decode +0xB54. The air frame FEC is ours (core/dmr/ambe) and matches stock's
	.at = { .encode = 0x130, .decode = 0x5D8, .enc_init = 0x284, .fec_init = 0x4CC, .dec_init = 0x6D0,
		.enc_params = 0x848, .enc_set = 0x1C0, .dec_set = 0x688 },
};

static const stm32_codec_profile_t *const profiles[] = { &v0206 };
const stm32_codec_profiles_t stm32_codec_profiles = { profiles, 1 };

#endif	// CONFIG_DMR
