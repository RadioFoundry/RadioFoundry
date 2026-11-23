// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// D-STAR DV coding (dstar_fec.h)
#include <string.h>
#include "dstar/dstar_fec.h"
#include "fec/conv.h"

// Its own copy, not core/aprs's ax25_fcs (the same CRC): D-STAR builds without APRS, and ten lines aren't worth a
// shared module between two protocol layers that are switched separately
uint16_t dstar_crc(const uint8_t *p, size_t n)
{
	uint16_t crc = 0xFFFFu;
	while (n--) {
		crc ^= *p++;
		for (int b = 0; b < 8; b++) {
			crc = (crc & 1u) ? (uint16_t)((crc >> 1) ^ 0x8408u) : (uint16_t)(crc >> 1);
		}
	}
	return (uint16_t)~crc;
}

void dstar_header_crc_set(uint8_t hdr[DSTAR_HEADER_BYTES])
{
	uint16_t crc = dstar_crc(hdr, DSTAR_HEADER_BYTES - 2u);
	hdr[39] = (uint8_t)crc;
	hdr[40] = (uint8_t)(crc >> 8);
}

bool dstar_header_crc_ok(const uint8_t hdr[DSTAR_HEADER_BYTES])
{
	uint16_t crc = dstar_crc(hdr, DSTAR_HEADER_BYTES - 2u);
	return hdr[39] == (uint8_t)crc && hdr[40] == (uint8_t)(crc >> 8);
}

// The standard's figure (appendix 1.1): seven stages shifting one way, the feedback the XOR of the last stage and the
// fourth, fed back into the first stage and XORed with the data. As a sequence: s[n] = s[n - 4] ^ s[n - 7], the seven
// bits before s[0] all ones. Bit 0 of the state below is s[n - 1]
static uint8_t pn_next(uint8_t *st)
{
	uint8_t b = (uint8_t)(((*st >> 3) ^ (*st >> 6)) & 1u);
	*st = (uint8_t)(((*st << 1) | b) & 0x7Fu);
	return b;
}

bool dstar_pn_bit(uint32_t i)
{
	uint8_t st = 0x7Fu, b = 0;
	for (uint32_t k = 0; k <= i % 127u; k++) {
		b = pn_next(&st);
	}
	return b != 0;
}

// The interleaver (appendix 2.2): the coded bits fill a 24-row matrix column by column (coded bit 24 c + r in row r,
// column c), 28 columns in rows 0-11 and 27 in rows 12-23 (660 = 12 x 28 + 12 x 27), and go on air row by row.
// The encoder walks the air positions in order; the decoder maps a coded index back to its air position
#define IL_ROWS		24u
#define IL_LONG_ROWS	12u

// The convolutional code (appendix 2.1, the older translation names the polynomials): G1 = 1 + D + D^2 first, then
// G2 = 1 + D^2, from zeroed registers, the header least significant bit first, then two zero bits. Coded and decoded
// by the code every mode shares (core/fec/conv.c), so a fix to the Viterbi decoder reaches D-STAR too; what is D-STAR's
// own is the bit order around it, the interleaver and the scrambler
static const fec_conv_code_t header_code = { 0x07u, 0x05u };
#define CONV_BITS	(DSTAR_HEADER_BITS / 2u)	// 328 header bits and the two tail bits
#define CONV_BYTES	((CONV_BITS + 7u) / 8u)

// The shared code reads and writes its bits most significant first; D-STAR's air order is least significant first
static uint8_t reverse8(uint8_t b)
{
	b = (uint8_t)((b & 0xF0u) >> 4 | (b & 0x0Fu) << 4);
	b = (uint8_t)((b & 0xCCu) >> 2 | (b & 0x33u) << 2);
	return (uint8_t)((b & 0xAAu) >> 1 | (b & 0x55u) << 1);
}

static bool msb_bit(const uint8_t *buf, uint32_t i)
{
	return ((buf[i >> 3] >> (7u - (i & 7u))) & 1u) != 0;
}

void dstar_header_encode(const uint8_t hdr[DSTAR_HEADER_BYTES], uint8_t air[DSTAR_HEADER_AIR_BYTES])
{
	uint8_t in[CONV_BYTES], coded[DSTAR_HEADER_AIR_BYTES];
	for (uint32_t i = 0; i < DSTAR_HEADER_BYTES; i++) {
		in[i] = reverse8(hdr[i]);
	}
	in[DSTAR_HEADER_BYTES] = 0;	// the tail bits
	memset(coded, 0, sizeof(coded));
	fec_conv_encode(&header_code, in, CONV_BITS, coded);
	memset(air, 0, DSTAR_HEADER_AIR_BYTES);
	uint8_t st = 0x7Fu;
	uint32_t j = 0;
	for (uint32_t r = 0; r < IL_ROWS; r++) {
		uint32_t cols = r < IL_LONG_ROWS ? 28u : 27u;
		for (uint32_t c = 0; c < cols; c++, j++) {
			bool b = msb_bit(coded, IL_ROWS * c + r) ^ (pn_next(&st) != 0);
			dstar_bit_put(air, j, b);
		}
	}
}

// The decoder's view of the received header: coded bit c's soft value read through the interleaver and the
// scrambler, straight out of the air-order buffer, with the scrambler's 127-bit period in a 16-byte table, so the
// decoder needs no second 330-byte copy on the stack of whichever task runs it
typedef struct {
	const uint8_t *soft;
	uint8_t pn[16];
} header_rx_t;

static int8_t coded_soft(const void *ctx, uint16_t c)
{
	const header_rx_t *h = ctx;
	uint32_t col = c / IL_ROWS, r = c % IL_ROWS;
	uint32_t j = r < IL_LONG_ROWS ? r * 28u + col : IL_LONG_ROWS * 28u + (r - IL_LONG_ROWS) * 27u + col;
	int8_t s = dstar_soft_get(h->soft, j);
	return dstar_bit(h->pn, j % 127u) ? (int8_t)-s : s;
}

dstar_header_result_t dstar_header_decode(const uint8_t soft[DSTAR_HEADER_SOFT_BYTES], uint8_t hdr[DSTAR_HEADER_BYTES])
{
	header_rx_t h = { .soft = soft };
	uint8_t st = 0x7Fu;
	for (uint32_t i = 0; i < 127u; i++) {
		dstar_bit_put(h.pn, i, pn_next(&st) != 0);
	}
	uint8_t dec[FEC_CONV_DEC_BYTES(3, CONV_BITS)], out[CONV_BYTES];
	int corrected = fec_conv_decode_fn(&header_code, coded_soft, &h, CONV_BITS, dec, out);
	for (uint32_t i = 0; i < DSTAR_HEADER_BYTES; i++) {
		hdr[i] = reverse8(out[i]);
	}
	dstar_header_result_t res = { dstar_header_crc_ok(hdr), (uint16_t)corrected };
	return res;
}

void dstar_data_scramble(uint8_t d[3])
{
	uint8_t st = 0x7Fu;
	for (uint32_t i = 0; i < DSTAR_DATA_BITS; i++) {
		if (pn_next(&st)) {
			d[i >> 3] ^= (uint8_t)(1u << (i & 7u));
		}
	}
}
