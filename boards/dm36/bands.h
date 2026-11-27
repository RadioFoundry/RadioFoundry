// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// PA select pins are specified; calibration and safe RF output remain unverified.
static const band_t bands[] = {
	{ 136000000, 174000000, true },
	{ 400000000, 480000000, true },
#if CONFIG_AM_RX
	{ 108000000, 136000000, false },
#endif
#if CONFIG_BCAST_FM
	{ 65000000, 108000000, false },
#endif
#if CONFIG_RX_EXT
	{ 16000000, 65000000, false },
	{ 174000000, 400000000, false },
	{ 480000000, 740000000, false },
#endif
};
