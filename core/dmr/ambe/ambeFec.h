// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
/*
 * AMBE+2 3600x2450 air frame (DMR full rate vocoder frame, 20 ms): 49 voice parameter bits <-> 72 bits on the air.
 *
 * Code words, each written most significant bit first:
 *   C0  24 bits  extended Golay(24,12): parameter bits 0-11, 11 Golay(23,12) parity bits, 1 overall parity bit
 *   C1  23 bits  Golay(23,12): parameter bits 12-23 and 11 parity bits, then XORed with a pseudo-random sequence
 *                seeded from C0's 12 data bits (x0 = 16 * data, x' = 173 x + 13849 mod 65536, one bit = bit 15 of
 *                each new x, x1 to x23, to the C1 bits from the most significant down)
 *   C2  11 bits  parameter bits 24-34, unprotected
 *   C3  14 bits  parameter bits 35-48, unprotected
 * Golay generator polynomial x^11 + x^10 + x^6 + x^5 + x^4 + x^2 + 1 (0xC75), systematic (data in the top 12 bits).
 *
 * Interleaving: write C0 C1 C2 C3 one after the other (72 bits S0..S71) and read them out four ways:
 * air bit 4k = S(k), 4k+1 = S(18+k), 4k+2 = S(36+k), 4k+3 = S(54+k), k = 0..17. Air bits are numbered from the most
 * significant bit of byte 0 of the 9-byte frame, the order the modems send them (each dibit is air bits 2i, 2i+1).
 *
 * A DMR voice burst carries three frames: 108 bits before the 48-bit sync / embedded signalling and 108 after
 * (docs/spec/dmr-modem.md 1.2). With the middle 48 bits taken out, as the HR_C6000 voice port and the dmr_modem HAL
 * deliver them, the 27 bytes are simply the three 9-byte frames in a row.
 */
#ifndef ODR_AMBE_FEC_H
#define ODR_AMBE_FEC_H

#include <stdbool.h>
#include <stdint.h>

#define AMBE_FRAME_BYTES	9
#define AMBE_FRAME_BITS		72
#define AMBE_PARAM_BITS		49
#define AMBE_BURST_BYTES	27	// three frames, sync / EMB removed
#define AMBE_FULL_BURST_BYTES	33	// 264-bit burst including the middle 48 bits

typedef struct {
	uint8_t c0_errors;	// bit errors corrected in C0 (0..3)
	uint8_t c1_errors;	// bit errors corrected in C1 (0..3; a perfect code, so more errors decode wrongly)
	bool c0_failed;		// C0 had 4 or more errors (detected, not corrected): treat the frame as lost
	uint8_t errors;		// c0_errors + c1_errors, or 7 when c0_failed
} ambe_fec_result_t;

// 49 parameter bits (one per byte, 0/1, parameter bit 0 first) -> 9-byte air frame
void ambeFecEncode(const uint8_t bits[AMBE_PARAM_BITS], uint8_t frame[AMBE_FRAME_BYTES]);
// 9-byte air frame -> 49 parameter bits with error correction. Returns the error count (as res->errors); res may be NULL
int ambeFecDecode(const uint8_t frame[AMBE_FRAME_BYTES], uint8_t bits[AMBE_PARAM_BITS], ambe_fec_result_t *res);

// Code words <-> air frame (interleaving only). c[0] = C0 (24 bits), c[1] = C1 (23, as sent, still whitened),
// c[2] = C2 (11), c[3] = C3 (14)
void ambeInterleave(const uint32_t c[4], uint8_t frame[AMBE_FRAME_BYTES]);
void ambeDeinterleave(const uint8_t frame[AMBE_FRAME_BYTES], uint32_t c[4]);
// Air bit position (0..71) of code word bit n of the 72-bit sequence S (C0 MSB first = n 0 ... C3 LSB = n 71)
uint8_t ambeAirBitOfSeq(uint8_t n);

// Golay codes as used here: 12 data bits in, code word out (23 bits: data << 11 | parity; 24 bits: that << 1 | overall
// parity, even). Decoders correct up to 3 errors, return the number corrected, -1 if uncorrectable (24-bit only)
uint32_t ambeGolay23Encode(uint16_t data);
uint32_t ambeGolay24Encode(uint16_t data);
int ambeGolay23Decode(uint32_t cw, uint16_t *data);
int ambeGolay24Decode(uint32_t cw, uint16_t *data);
// The 23-bit whitening mask for C1 from C0's 12 data bits
uint32_t ambeC1Mask(uint16_t c0_data);

// Voice burst <-> three frames
void ambeBurstSplit(const uint8_t burst[AMBE_BURST_BYTES], uint8_t frames[3][AMBE_FRAME_BYTES]);
void ambeBurstJoin(const uint8_t frames[3][AMBE_FRAME_BYTES], uint8_t burst[AMBE_BURST_BYTES]);
// The same for a full 264-bit burst: frame bits are burst bits 0-107 and 156-263; the middle 48 bits are left as they are
void ambeFullBurstSplit(const uint8_t burst[AMBE_FULL_BURST_BYTES], uint8_t frames[3][AMBE_FRAME_BYTES]);
void ambeFullBurstJoin(const uint8_t frames[3][AMBE_FRAME_BYTES], uint8_t burst[AMBE_FULL_BURST_BYTES]);

#endif
