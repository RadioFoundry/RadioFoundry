// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Yaesu System Fusion coding (docs/spec/ysf-modem.md 3, from Yaesu's "Amateur Radio Digital Standards" rev 1.02,
// section 4.5 and appendices A and B): the CRC-16, the extended Golay (24,12) code, the rate 1/2 K = 5 convolutional
// code with its soft Viterbi decoder, the dibit interleavers, the PN (9,5) whitening and the V/D mode 2 voice
// channel's repetition code. No RTOS, no board, integer only.
//
// Bits travel most significant first: bit i of a buffer is byte i / 8, bit 7 - i % 8. The standard turns its bit
// series into dibits "starting from the leading bit" (3.2 (2)), so a field it draws left to right is stored with its
// leftmost bit as bit 0, and a byte of a callsign goes on air from its top bit down.
#ifndef RFOUNDRY_YSF_FEC_H
#define RFOUNDRY_YSF_FEC_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

static inline bool ysf_bit(const uint8_t *buf, uint32_t i)
{
	return ((buf[i >> 3] >> (7u - (i & 7u))) & 1u) != 0;
}

static inline void ysf_bit_put(uint8_t *buf, uint32_t i, bool v)
{
	uint8_t m = (uint8_t)(0x80u >> (i & 7u));
	if (v) {
		buf[i >> 3] |= m;
	} else {
		buf[i >> 3] &= (uint8_t)~m;
	}
}

// Soft bits: the receiver's confidence per coded bit, positive for 1, negative for 0, 0 for nothing known, -7..7.
// The receiver keeps them packed four bits each (bit i in the high nibble of byte i / 2 when i is odd), as the
// D-STAR receiver does, so a header's two 360-bit units fit in 360 bytes
#define YSF_SOFT_MAX	7

static inline int8_t ysf_soft_clamp(int32_t s)
{
	return (int8_t)(s > YSF_SOFT_MAX ? YSF_SOFT_MAX : s < -YSF_SOFT_MAX ? -YSF_SOFT_MAX : s);
}

static inline void ysf_soft_put(uint8_t *packed, uint32_t i, int8_t s)
{
	uint8_t n = (uint8_t)ysf_soft_clamp(s) & 0x0Fu;
	uint8_t *b = &packed[i >> 1];
	*b = (i & 1u) ? (uint8_t)((*b & 0x0Fu) | (uint8_t)(n << 4)) : (uint8_t)((*b & 0xF0u) | n);
}

static inline int8_t ysf_soft_get(const uint8_t *packed, uint32_t i)
{
	uint8_t n = (i & 1u) ? (uint8_t)(packed[i >> 1] >> 4) : (uint8_t)(packed[i >> 1] & 0x0Fu);
	return (int8_t)((n & 0x08u) ? (int)n - 16 : (int)n);
}

// --- CRC-16 (4.5 (1) (b), figure 4-25) ---

// x^16 + x^12 + x^5 + 1 over nbits bits of buf (most significant first), the registers from zero, every bit of the
// result inverted. The figure's register is the plain left-shifting kind: the catalog's CRC-16/GSM ("123456789" gives
// 0xCE3C). The 16 bits follow the data top bit first
uint16_t ysf_crc(const uint8_t *buf, uint32_t nbits);

// --- extended Golay (24,12,8) (appendix A) ---

// 12 data bits to the 24-bit code word, d11 first: data in bits 23-12, the parity p11..p0 in bits 11-0
uint32_t ysf_golay_encode(uint16_t data);
// Up to three errors corrected. Returns the number corrected, -1 for four or more (data then holds the received data
// bits as they are)
int ysf_golay_decode(uint32_t cw, uint16_t *data);

// --- convolutional code (4.5 (1) (c), appendix B) ---

// Rate 1/2, K = 5: G1 = x^4 + x^3 + 1 then G2 = x^4 + x^2 + x + 1 for each input bit, from zeroed registers; nbits
// input bits (the caller's data with its four zero tail bits) to 2 nbits output bits
void ysf_conv_encode(const uint8_t *in, uint32_t nbits, uint8_t *out);

#define YSF_VITERBI_MAX	180u	// input bits of the longest unit: 160 data, 16 CRC, 4 tail

// Soft Viterbi decoder over 2 nbits soft values (in coded bit order) to nbits bits, the last four of which are the
// zero tail it ends on. get(ctx, i) returns soft bit i. Returns the coded bits its path disagrees with (a quality
// figure: 0 on a clean signal)
typedef int8_t (*ysf_soft_get_t)(const void *ctx, uint32_t i);
uint16_t ysf_viterbi(ysf_soft_get_t get, const void *ctx, uint32_t nbits, uint8_t *out);

// --- whitening (4.5 (2) (b), figure 4-28) ---

// Bit i of the PN (9,5) sequence from the figure's initial value (S08..S00 = 111001001): the sequence starts
// 93 D7 51 21 9C 2F 6C D0 when packed most significant first. It restarts for every unit
bool ysf_pn_bit(uint32_t i);
// XOR the first nbits bits of buf with the sequence (it whitens and unwhitens)
void ysf_whiten(uint8_t *buf, uint32_t nbits);

// --- interleavers ---

// The FICH and the DCH units (figures 4-26, 4-30, 4-31): the coded bits as dibits S0..S(n-1), written into rows of
// width dibits and read out by columns, 20 rows. The air's k-th dibit is S((k % 20) x width + k / 20)
static inline uint32_t ysf_dibit_il(uint32_t k, uint32_t width)
{
	return (k % 20u) * width + k / 20u;
}

// The V/D mode 2 voice (figure 4-32): 104 bits written into four rows of 26 and read out by columns. The air's k-th
// bit is S((k % 4) x 26 + k / 4)
static inline uint32_t ysf_vch_il(uint32_t k)
{
	return (k % 4u) * 26u + k / 4u;
}

// --- coded units ---

// A unit's CRC is said to follow the whitening (figure 4-27), but the detailed figures 4-30 and 4-31 leave the
// whitening out. The receiver accepts either order and says which it found; the transmitter uses the one asked for
typedef enum {
	YSF_WHITEN_DATA = 0,	// whiten the data, then the CRC over the whitened data (figure 4-27)
	YSF_WHITEN_ALL = 1,	// the CRC over the data, then whiten data and CRC together
} ysf_whiten_t;

typedef struct {
	bool crc_ok;
	uint8_t order;		// ysf_whiten_t the CRC matched with (when crc_ok)
	uint16_t corrected;	// coded bits the decoder disagreed with
} ysf_unit_result_t;

#define YSF_DCH_BYTES_LONG	20u	// a 160-bit unit: CSD1, CSD2, CSD3 in a header or a V/D mode 1 frame
#define YSF_DCH_BYTES_SHORT	10u	// an 80-bit unit: a V/D mode 2 frame's DCH
#define YSF_DCH_BITS_LONG	360u	// coded
#define YSF_DCH_BITS_SHORT	200u

// A DCH unit of n bytes (10 or 20) to its coded bits in air order (360 or 200, most significant first)
void ysf_dch_encode(const uint8_t *data, uint32_t n, ysf_whiten_t order, uint8_t *air);
// The soft bits of a unit in air order (packed, ysf_soft_put) back to its n bytes, whatever the CRC says
ysf_unit_result_t ysf_dch_decode(const uint8_t *soft, uint32_t n, uint8_t *data);

// The FICH's 32 bits (figure 4-9, bit 31 first) to 200 coded bits in air order: CRC, four Golay words, the
// convolutional code, the interleaver. Not whitened
#define YSF_FICH_BITS		200u
void ysf_fich_encode(uint32_t fich, uint8_t air[YSF_FICH_BITS / 8u]);
typedef struct {
	bool crc_ok;
	uint8_t golay;		// bit errors the Golay words corrected (0xFF: one failed)
	uint16_t corrected;	// coded bits the Viterbi decoder disagreed with
} ysf_fich_result_t;
ysf_fich_result_t ysf_fich_decode(const uint8_t *soft, uint32_t *fich);

// The V/D mode 2 voice channel (4.5 (3)): 49 codec bits (one per byte, 0/1, as hal_vocoder.h gives them) to the 104
// bits of VCH and VeCH in air order: the first 27 bits three times each, the other 22 once, a zero, whitened,
// interleaved
#define YSF_VD2_VOICE_BITS	104u
#define YSF_VOICE_BITS		49u
void ysf_vd2_voice_encode(const uint8_t bits[YSF_VOICE_BITS], uint8_t air[YSF_VD2_VOICE_BITS / 8u]);
// The soft bits back to 49 codec bits: each of the first 27 by the sum of its three copies' soft values (a majority
// vote weighted by confidence), the other 22 by sign. Returns the copies that disagreed with their vote and the
// unprotected bits sent with no confidence, a quality figure
uint8_t ysf_vd2_voice_decode(const uint8_t *soft, uint8_t bits[YSF_VOICE_BITS]);

#endif
