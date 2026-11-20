// SPDX-License-Identifier: MIT AND ISC
// Copyright (c) 2026 The RadioFoundry Authors
// SPDX-FileCopyrightText: 2010 mbelib Author (the parameter bit map, ISC license, see LICENSES/ISC.txt)
/*
 * The D-STAR AMBE 3600x2400 voice frame (docs/spec/dstar-ambe-frame.md): the quantizer's nine fields b0..b8 (48 speech
 * bits) <-> one 72-bit voice frame in core/dstar's air order (dstar_voice.h: byte 0 first, each byte least
 * significant bit first).
 *
 * Three 24-bit words (spec 1):
 *   W0 (C0)  extended Golay(24,12) of parameter bits d0..d11
 *   W1 (C1)  extended Golay(24,12) of d12..d23, XORed with 24 steps of the whitening sequence seeded from W0's data
 *   W2       d25..d34 (C2) and d35..d48 (C3), unprotected
 * d0..d48 is mbelib's 49-bit parameter vector, whose d24 is W1's overall parity bit rather than a speech bit; the map
 * from it to b0..b8 is mbelib's decoder's. The air interleave was derived from off-air frames of DVSI encoders (spec 5).
 */
#ifndef RFOUNDRY_DSTAR_AMBE_FRAME_H
#define RFOUNDRY_DSTAR_AMBE_FRAME_H

#include <stdbool.h>
#include <stdint.h>
#include "dstar/dstar_voice.h"
#include "dstar/dstar_ambe_quant.h"

typedef struct {
	uint8_t c0_errors;	// bit errors corrected in C0 (0..3)
	uint8_t c1_errors;	// in C1 (0..3)
	bool c0_failed;		// C0 had 4 or more errors (detected, not corrected): the frame is lost
	bool c1_failed;		// the same for C1, after C0's whitening was taken off
} dstar_ambe_fec_t;

// Nine fields (b8's low bit is not sent) -> one frame
void dstar_ambe_pack(const uint16_t b[DSTAR_AMBE_FIELDS], uint8_t frame[DSTAR_VOICE_BYTES]);

// One frame -> nine fields with error correction (b8's low bit is 0). fec may be NULL. C0, when it failed, is passed on
// as received; C1, when it failed, as its 23-bit Golay word decodes without the parity bit
void dstar_ambe_unpack(const uint8_t frame[DSTAR_VOICE_BYTES], uint16_t b[DSTAR_AMBE_FIELDS], dstar_ambe_fec_t *fec);

#endif
