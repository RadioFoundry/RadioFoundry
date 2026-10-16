// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The encode flags word of the AMBE+2 codec family the MD-380, DM-1701, DM-1801 Pro and DM-32UV hosts run
// (docs/spec/codec-md380.md "Encode flags"): hal_vocoder_t options to the word passed on every encode call. One value
// for every call of a transmission; the codec takes it as a set of feature bits, so a value that changes from call to
// call is not a "first frame" marker.
#ifndef ODR_AMBE2_FLAGS_H
#define ODR_AMBE2_FLAGS_H

#include <stdint.h>
#include "hal_vocoder.h"

#define AMBE2_ENC_NOISE_GATE	0x0800u	// always set
#define AMBE2_ENC_NOISE_SUPPR	0x0040u	// VOCODER_OPT_NOISE_REDUCTION
#define AMBE2_ENC_TONE_FRAMES	0x1000u	// VOCODER_OPT_TONE_FRAMES
// Never set: 0x0100 turns the input DC filter off, 0x2000 the input AGC (not measured on air), 0x4000 a test mode
// that sends one fixed frame

// What these hosts offer through hal_vocoder_t (the AGC is not offered until it has been tested on air)
#define AMBE2_OPTIONS		(VOCODER_OPT_NOISE_REDUCTION | VOCODER_OPT_TONE_FRAMES)

static inline uint32_t ambe2_enc_flags(uint32_t opts)
{
	uint32_t f = AMBE2_ENC_NOISE_GATE;
	if (opts & VOCODER_OPT_NOISE_REDUCTION) {
		f |= AMBE2_ENC_NOISE_SUPPR;
	}
	if (opts & VOCODER_OPT_TONE_FRAMES) {
		f |= AMBE2_ENC_TONE_FRAMES;
	}
	return f;
}

#endif
