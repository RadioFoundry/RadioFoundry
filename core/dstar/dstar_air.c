// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// D-STAR DV as an air interface profile (dstar_air.h)
#include "dstar/dstar_air.h"
#include "dstar/dstar_frame.h"
#include "dstar/dstar_gmsk.h"
#include "../../hal/hal_radio.h"

// Symbols in air order, a bit 1 as +, 0 as - (dibits 01 and 11, hal_air.h): 1 is a positive deviation (dstar_gmsk.h)
const air_profile_t dstar_air_profile = {
	.sym_rate = DSTAR_BAUD,
	.levels = 2,
	.shape = AIR_SHAPE_GAUSS,
	.shape_param = 50,			// BT = 0.5 (JARL 4.1.1, dstar-modem.md 2.1)
	.dev_outer_hz = DSTAR_DEV_HZ,
	.timing = AIR_TIMING_CONTINUOUS,
	.bw = RADIO_BW_12K5,
	// The resync pattern is the one 24-symbol pattern that recurs: once a superframe, 21 frames of 96 bits
	.sync_every_syms = DSTAR_SUPERFRAME * DSTAR_FRAME_BITS,
	.sync_any_syms = 0,
	.idle = 0x77,				// 1010: the preamble's pattern
	.nsyncs = 2,
	.sync = {
		// 1010101010 1101000 1101000 in the data place of frame 0, after its 72 voice bits (3.3); 4 errors as the
		// deframer allows in a call (3.4)
		{ .id = DSTAR_AIR_SYNC_RESYNC, .nsyms = 24, .max_errs = 4, .frame_at = -72,
		  .syms = { 0x77, 0x77, 0x75, 0xDF, 0xD7, 0x7F } },
		// The frame sync, 111011001010000, once, after the preamble: the header follows it; 1 error as the
		// deframer's search allows
		{ .id = DSTAR_AIR_SYNC_FRAME, .nsyms = DSTAR_FRAME_SYNC_BITS, .max_errs = 1, .flags = AIR_SYNC_START_ONLY,
		  .frame_at = DSTAR_FRAME_SYNC_BITS, .syms = { 0x57, 0x5F, 0x77, 0xFC } },
	},
};
