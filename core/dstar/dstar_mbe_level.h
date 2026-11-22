// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// SPDX-FileCopyrightText: 2026 mbe contributors
/*
 * Speech level control for the D-STAR encoder (docs/spec/dstar-codec.md 2): a leveler between the analysis
 * (dstar_mbe_analyze) and the quantizer, working on the frame's log2 magnitudes, so whatever the microphone's level the
 * frames go out at one level in the speech band a handheld's speaker reproduces (below 500 Hz weighted down). The evidence and the
 * constants are in dstar_mbe_level.c.
 *
 * Integer only, no heap, no globals, 16 bytes of state.
 */
#ifndef RFOUNDRY_DSTAR_MBE_LEVEL_H
#define RFOUNDRY_DSTAR_MBE_LEVEL_H

#include <stdint.h>
#include "dstar/dstar_ambe_quant.h"

typedef struct {
	int32_t speech;		// speech level estimate, log2 amplitude Q12
	int32_t floor;		// background level estimate, log2 amplitude Q12
	int32_t gain;		// the gain applied to speech, log2 Q12
	uint8_t off;		// 1: frames pass unchanged, for tests of the codec as a transparent coder (its quality
				// against the research's figures, which were measured without the leveler). Init clears it
} dstar_mbe_level_t;

void dstar_mbe_level_init(dstar_mbe_level_t *s);

// The frame's level as a DVSI decoder plays it and a small speaker reproduces it (harmonics below 500 Hz weighted by
// (f / 500)^2), log2 amplitude Q12, or INT32_MIN for the analysis's silent frame
int32_t dstar_mbe_level_of(const dstar_ambe_params_t *p);

// Measures the frame, updates the state and adds the gain to every magnitude. Returns the gain applied, log2 Q12
int32_t dstar_mbe_level(dstar_mbe_level_t *s, dstar_ambe_params_t *p);

#endif
