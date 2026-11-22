// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
/*
 * Soft decision decoding of BPTC(196,96) (TS 102 361-1 B.1.1; docs/spec/dmr-modem.md 1.8): an iterative product
 * code decoder whose rows (Hamming(15,11,3)) and columns (Hamming(13,9,3)) are each decoded exactly, soft in and
 * soft out, over their dual codes (16 words each). Where the hard decoder (dmrFec.h) gives up past a few bit errors,
 * this one weighs how sure each bit is: on bursts received through FM noise it decodes most of what the hard
 * decoder can't.
 *
 * Input: one LLR per info bit in burst order (bits 0-97, then 166-263), positive for a 1, in eighths (8 = a natural
 * log ratio of 1), clamped to +-127. The decoder is exact only for true LLRs: a receiver scales its soft values by its
 * own noise estimate. Fixed point (no float, no divide), 0.6 KB of tables (in CPU-only RAM where the platform has
 * some); the caller owns the work memory, best placed RFOUNDRY_CPU_RAM.
 */

#ifndef RFOUNDRY_DMR_SOFT_BPTC_H
#define RFOUNDRY_DMR_SOFT_BPTC_H

#include <stdbool.h>
#include <stdint.h>

#define DMR_SOFT_BPTC_BITS	196
#define DMR_SOFT_BPTC_ITERS	4	// 98 % of what 20 decode, in AWGN

typedef struct {
	int8_t ch[DMR_SOFT_BPTC_BITS];		// the input, in code matrix order
	int8_t row[DMR_SOFT_BPTC_BITS];		// the rows' extrinsic LLRs
	int8_t col[DMR_SOFT_BPTC_BITS];		// the columns'
} dmrSoftBptcWork_t;

// Whether a decoded payload is right (its CRC or Reed-Solomon check, by data type): a candidate the hard decoder
// corrected is often wrong after the first iteration and right after the second
typedef bool (*dmrSoftBptcAccept_t)(const uint8_t *payload, void *ctx);

// Decodes llr into payload (12 bytes, as dmrFecBptcDecode) and the reserved bits R(3)-R(0) into *rbits (NULL: not
// wanted). The decisions go to the hard decoder first, then after each iteration; its payload ends the decoding when
// accept takes it (accept NULL: any it can decode). Returns the iterations it took: 0 when the decisions alone
// decoded, -1 when no iteration up to maxIters gave an accepted payload. Without accept a codeword is not proof:
// check the payload
int dmrSoftBptcDecode(dmrSoftBptcWork_t *w, const int8_t *llr, uint8_t *payload, uint8_t *rbits, int maxIters,
		      dmrSoftBptcAccept_t accept, void *ctx);

#endif
