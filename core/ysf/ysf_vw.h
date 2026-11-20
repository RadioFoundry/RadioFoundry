// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Yaesu System Fusion Voice FR ("VW", docs/spec/ysf-modem.md 4.1): the full-rate voice frame and where VW puts it.
//
// A VW communication frame's five 144-bit subframes are five full-rate vocoder frames (20 ms each), coded exactly as
// P25 Phase 1 codes its voice (TIA-102.BAAA 5.3): the 88 information bits u0..u7 in four Golay (23,12) words, three
// Hamming (15,11) words and seven bare bits, c1..c6 scrambled by a sequence seeded from u0, the 144 bits in P25's voice
// interleave. The one exception is the first communication frame after the header (FN 0 of the FT 1 pair that follows
// it): its subframes 0-2 carry CSD3 as a 160-bit DCH unit (360 coded bits) and 72 zero bits, and only subframes 3-4
// are voice. Yaesu's standard doesn't give this layout; it was read off a real radio's VW transmission, every frame's
// FEC checking (4.1).
//
// The codec itself (core/mbe) stays outside, as DN's does: frames go to and come from the caller as the 88 bits.
// No RTOS, no board, integer only.
#ifndef RFOUNDRY_YSF_VW_H
#define RFOUNDRY_YSF_VW_H

#include <stdint.h>
#include <stdbool.h>
#include "ysf/ysf_frame.h"

#define YSF_VW_FRAME_BITS	144u	// one subframe
#define YSF_VW_CSD3_SUBS	3u	// subframes the CSD3 unit and its pad take in the first communication frame
#define YSF_VW_CSD3_PAD		72u	// zero bits after the 360-bit unit

// What a frame's codes corrected, packed as core/mbe's IMBE_ERRS wants it (imbe.h): the bits corrected in all seven
// code words (0..15), in the first (u0's Golay word, 0..3) and in the fifth (u4's Hamming word, 0..1). Spelled out here
// so core/ysf needs nothing from the codec; ysfVwTest checks the two agree
#define YSF_VW_ERRS(total, c0, c4)	((uint8_t)((total) | ((c0) << 4) | ((c4) << 6)))
#define YSF_VW_ERRS_TOTAL(e)		((e) & 15u)

// The 88 bits to a frame's 144 coded bits, written at bit `at` of buf (most significant first, ysf_bit_put)
void ysf_vw_frame_encode(const uint8_t imbe[YSF_VW_IMBE_BYTES], uint8_t *buf, uint32_t at);
// 144 hard bits read from bit `at` of buf back to the 88 bits. Returns the corrections, YSF_VW_ERRS. The Golay code is
// perfect, so four or more errors in a word decode to a wrong word; the codec's rule (IMBE_ERRS_REPEAT) catches a
// frame whose u0 went wrong, which also unscrambled the rest wrongly
uint8_t ysf_vw_frame_decode(const uint8_t *buf, uint32_t at, uint8_t imbe[YSF_VW_IMBE_BYTES]);

// Whether a frame of these fields carries CSD3 in its subframes 0-2
static inline bool ysf_vw_has_csd3(const ysf_fich_t *f)
{
	return f->fi == YSF_FI_COMM && f->ft == 1u && f->fn == 0u;
}

// The transmitter's FN and FT for its n-th communication frame (0 first): the FT 1 pair FN 0, FN 1 after the header,
// then FN 0 of FT 0 for the rest, as the radio this was read from sends them
static inline void ysf_vw_tx_fn(uint32_t n, uint8_t *fn, uint8_t *ft)
{
	*fn = n == 1u ? 1u : 0u;
	*ft = n < 2u ? 1u : 0u;
}

// The frame after f in a VW transmission, for a receiver that lost a FICH: after the header FN 0 FT 1 (CSD3), then FN 1
// FT 1, then FN 0 FT 0 for good. ysf_frame.c's rolling count would go back to FN 0 FT 1 and read voice as CSD3
static inline void ysf_vw_rx_next(ysf_fich_t *f)
{
	bool header = f->fi != YSF_FI_COMM;
	f->fi = YSF_FI_COMM;
	f->fn = (uint8_t)(!header && f->fn == 0u && f->ft == 1u ? 1u : 0u);
	f->ft = (uint8_t)(header || f->fn == 1u ? 1u : 0u);
}

// The receiver's and the transmitter's VW parts, called by ysf_frame.c for frames with DT = Voice FR. The receiver's:
// one communication frame's payload bit (b: 0..719) with its soft value, and the payload's end (CSD3 into r->call)
void ysf_vw_rx_bit(ysf_rx_t *r, uint32_t b, int8_t s);
void ysf_vw_rx_done(ysf_rx_t *r);
// The transmitter's: a communication frame's data part (CSD3 in the first; the caller has cleared the frame and put the
// sync and FICH), and the voice of subframe sub as it is reached
void ysf_vw_tx_comm(ysf_tx_t *t);
void ysf_vw_tx_voice(ysf_tx_t *t, uint32_t sub);
// The first voice subframe of the transmitter's current frame
static inline uint32_t ysf_vw_tx_first_voice(const ysf_tx_t *t)
{
	return t->fn == 0u ? YSF_VW_CSD3_SUBS : 0u;
}

#endif
