// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The stock AMBE+2 codec run in place from flash, shared by the STM32F4 radios (docs/spec/codec-placement.md, "STM32
// radios"; docs/spec/codec-dm1701.md, codec-md380.md, codec-md9600.md). It is one library in every TYT / Baofeng
// STM32 image, linked at other addresses: the init sequence, the arguments and the state sizes are the same, only the
// entry offsets and the placement differ. A board describes the builds it accepts (stm32_codec_profile_t) and defines
// stm32_codec_profiles; vocoder.c gives the hal_vocoder_t. The merge on the user's machine writes the codec and its
// descriptor (codec_desc.h); none of it is part of this repository or a release.
#ifndef STM32_HRC_VOCODER_H
#define STM32_HRC_VOCODER_H

#include <stdint.h>
#include <stdbool.h>
#include "config.h"
#include "codec_desc.h"
#include "hal_vocoder.h"

// Entry points: offsets from the code start
typedef struct {
	uint16_t encode, decode;
	uint16_t enc_init;	// (E, 1, 1)
	uint16_t fec_init;	// (F, 1): state of the codec's own FEC routines
	uint16_t dec_init;	// (D, 1)
	uint16_t enc_params;	// (E, 10 parameters)
	uint16_t enc_set;	// (E, a, b)
	uint16_t dec_set;	// (D, a, b)
} stm32_codec_entries_t;

// One stock build as the merge places it
typedef struct {
	uint32_t tag;			// the descriptor's tag for the build
	const char *id;			// the profile id the debug link reports (hal_vocoder_t.profile)
	uint32_t flash_lo;		// the flash the board's linker script leaves to the codec, up to the descriptor
	uint32_t code;			// where the merge puts the code; 0: anywhere in the codec flash
	uint32_t code_len;		// the code window's length (always checked)
	uint32_t code_crc;		// its CRC-32 as placed; 0: the descriptor's, checked against the bytes
	// The table windows exactly as placed (address, length, CRC); NULL: the descriptor's, checked against the bytes
	const codec_desc_window_t *tables;
	uint8_t windows;		// entries in tables
	stm32_codec_entries_t at;
} stm32_codec_profile_t;

typedef struct {
	const stm32_codec_profile_t *const *list;	// tried in order; the first whose checks pass is used
	uint8_t count;
} stm32_codec_profiles_t;

// Defined by each board that builds platform/stm32f4/hrc
extern const stm32_codec_profiles_t stm32_codec_profiles;

#if CONFIG_DMR
extern const hal_vocoder_t stm32_vocoder;
#endif

#endif
