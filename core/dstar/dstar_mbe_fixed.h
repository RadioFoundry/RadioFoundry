// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// SPDX-FileCopyrightText: 2026 mbe contributors
/*
 * Fixed-point helpers and tables shared by the D-STAR codec's MBE analyzer and synthesizer (docs/spec/dstar-codec.md).
 *
 * The radios' smallest core (C-SKY CK803S) has a 32x32->32 multiplier, a hardware divider and count-leading-zeros, but
 * no 32x32->64 multiply: a 64-bit product is a libgcc call. So every inner loop multiplies 16-bit quantities into 32
 * bits, and 64-bit products stay out of anything done per sample.
 *
 * Phases are uint32 with 2^32 = one cycle, so they wrap exactly; mbelib's float phases grow without bound and lose
 * precision over a long transmission.
 */
#ifndef RFOUNDRY_DSTAR_MBE_FIXED_H
#define RFOUNDRY_DSTAR_MBE_FIXED_H

#include <stdint.h>

_Static_assert((-1 >> 1) == -1, "arithmetic right shift required");

// log2(256 x sum of the squared spectral window), Q12: the analyzer's band power scale
#define DSTAR_MBE_SW_LOG2_NW2_Q12	59188

// dstar_mbe_tables.inc
extern const int16_t dstar_mbe_cos_q15[1025];
extern const int16_t dstar_mbe_spec_w_q15[111];
extern const int16_t dstar_mbe_pitch_lpf_q15[21];
extern const int16_t dstar_mbe_win_ft_q12[449];
extern const uint32_t dstar_mbe_f0_q24[126];
extern const uint32_t dstar_mbe_f0_mid_q24[125];
extern const uint8_t dstar_mbe_L[126];
extern const uint16_t dstar_mbe_ws_ramp_q15[51];
extern const uint16_t dstar_mbe_log2_tab_q16[33];
extern const uint32_t dstar_mbe_pow2_tab_q28[33];

// cos of a uint32 phase, Q15 (|v| <= 32767). 4096 steps a cycle, no interpolation: worst error 7.7e-4 (-62 dB), well
// under what the 16-bit output and the MBE model itself can show, and it costs one load
static inline int32_t dstar_mbe_cos(uint32_t ph)
{
	uint32_t i = ph >> 20, j = i & 1023u, q = i >> 10;
	int32_t v = dstar_mbe_cos_q15[(q & 1u) ? 1024u - j : j];
	return ((q + 1u) & 2u) ? -v : v;
}

static inline int32_t dstar_mbe_sin(uint32_t ph)
{
	return dstar_mbe_cos(ph - 0x40000000u);
}

static inline int dstar_mbe_clz(uint32_t x)
{
	return x ? __builtin_clz(x) : 32;
}

// log2(x) in Q12 for x > 0 (-32 << 12 for x = 0)
int32_t dstar_mbe_log2_q12(uint32_t x);

// round(2^(x / 4096)), saturating at 0x7fffffff; 0 when it rounds to 0
uint32_t dstar_mbe_pow2_q12(int32_t x);


#endif
