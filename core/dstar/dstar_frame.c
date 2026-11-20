// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// D-STAR fields on air (dstar.h): the header and its FEC, voice frames, the scrambler. Written from the JARL standard
// (4.1, appendices 1 and 2).
#include "dstar.h"
#include <string.h>
#include "../air/air_syms.h"

const uint8_t dstar_data_sync[DSTAR_DATA_BYTES] = { 0x55, 0x2D, 0x16 };
const uint8_t dstar_end_bytes[DSTAR_END_SYMS / 8] = { 0x55, 0x55, 0x55, 0x55, 0xC8, 0x7A };

// S(x) = x^7 + x^4 + 1 from all ones: its 127-bit period, the first bit in bit 7 of byte 0
static const uint8_t pn_tab[16] = {
	0x0E, 0xF2, 0xC9, 0x02, 0x26, 0x2E, 0xB6, 0x0C, 0xD4, 0xE7, 0xB4, 0x2A, 0xFA, 0x51, 0xB8, 0xFE,
};

uint8_t dstar_pn(uint16_t n)
{
	n %= 127;
	return (uint8_t)((pn_tab[n >> 3] >> (7 - (n & 7))) & 1);
}

void dstar_data_scramble(uint8_t data[DSTAR_DATA_BYTES])
{
	// the sequence's first 24 bits, each field byte least significant bit first
	data[0] ^= 0x70;
	data[1] ^= 0x4F;
	data[2] ^= 0x93;
}

// ── The header ──
uint16_t dstar_crc(const uint8_t *b, uint16_t n)
{
	uint16_t c = 0xFFFF;

	while (n--) {
		c ^= *b++;
		for (int i = 0; i < 8; i++)
			c = (uint16_t)((c & 1) ? (c >> 1) ^ 0x8408 : c >> 1);
	}
	return (uint16_t)~c;
}

void dstar_header_pack(const dstar_header_t *h, uint8_t raw[DSTAR_HDR_BYTES])
{
	uint16_t c;

	memcpy(raw, h->flag, 3);
	memcpy(raw + 3, h->rpt2, DSTAR_CALL_LEN);
	memcpy(raw + 11, h->rpt1, DSTAR_CALL_LEN);
	memcpy(raw + 19, h->ur, DSTAR_CALL_LEN);
	memcpy(raw + 27, h->my, DSTAR_CALL_LEN);
	memcpy(raw + 35, h->sfx, DSTAR_SUFFIX_LEN);
	c = dstar_crc(raw, DSTAR_HDR_FIELDS);
	raw[39] = (uint8_t)c;
	raw[40] = (uint8_t)(c >> 8);
}

bool dstar_header_unpack(const uint8_t raw[DSTAR_HDR_BYTES], dstar_header_t *h)
{
	uint16_t c = dstar_crc(raw, DSTAR_HDR_FIELDS), got = (uint16_t)(raw[39] | raw[40] << 8);

	// The standard names the polynomial only. Sent from all ones, inverted, low byte first (as X.25's FCS) is what
	// we send; the same not inverted is taken too (docs/spec/dstar.md 2.2)
	if (got != c && got != (uint16_t)~c)
		return false;
	memcpy(h->flag, raw, 3);
	memcpy(h->rpt2, raw + 3, DSTAR_CALL_LEN);
	memcpy(h->rpt1, raw + 11, DSTAR_CALL_LEN);
	memcpy(h->ur, raw + 19, DSTAR_CALL_LEN);
	memcpy(h->my, raw + 27, DSTAR_CALL_LEN);
	memcpy(h->sfx, raw + 35, DSTAR_SUFFIX_LEN);
	return true;
}

void dstar_call_set(char *field, uint8_t len, const char *text)
{
	uint8_t i = 0;

	for (; text != NULL && text[i] != '\0' && i < len; i++) {
		char ch = text[i];

		field[i] = (ch >= 'a' && ch <= 'z') ? (char)(ch - 'a' + 'A') : (ch >= ' ' && ch <= '~') ? ch : ' ';
	}
	for (; i < len; i++)
		field[i] = ' ';
}

// ── Symbols: a 1 is +3 (01), a 0 is -3 (11) ──
void dstar_bits_put(uint8_t *syms, uint16_t at, const uint8_t *bytes, uint16_t nbits)
{
	for (uint16_t i = 0; i < nbits; i++)
		air_sym_put(syms, (uint16_t)(at + i), ((bytes[i >> 3] >> (i & 7)) & 1) ? 1 : 3);
}

void dstar_bits_get(const uint8_t *syms, uint16_t at, uint8_t *bytes, uint16_t nbits)
{
	memset(bytes, 0, (size_t)(nbits + 7) / 8);
	for (uint16_t i = 0; i < nbits; i++)
		if ((air_sym_get(syms, (uint16_t)(at + i)) & 2) == 0)
			bytes[i >> 3] |= (uint8_t)(1u << (i & 7));
}

// The interleave (Ap2.2): code bit k is written down column k / 24, row k % 24 of a 24-row matrix whose last column
// holds 12 bits; the rows are sent in turn, the first 12 with 28 bits, the others with 27
static uint16_t ilv_pos(uint16_t k)
{
	uint16_t r = k % 24, c = k / 24;

	return (uint16_t)((r <= 12 ? 28 * r : 28 * 12 + 27 * (r - 12)) + c);
}

// The convolutional code (Ap2.1): constraint length 3, two outputs per bit, the first 1 + D + D^2, the second
// 1 + D^2. The standard names neither: these are the code's usual generators (docs/spec/dstar.md 2.3)
#define G_SWAP_NONE	0
#define G_SWAP		1

static void conv_bits(uint8_t b, uint8_t s, uint8_t *o1, uint8_t *o2)
{
	uint8_t x1 = (uint8_t)((s >> 1) & 1), x2 = (uint8_t)(s & 1);

	*o1 = (uint8_t)(b ^ x1 ^ x2);
	*o2 = (uint8_t)(b ^ x2);
}

void dstar_header_encode(const uint8_t raw[DSTAR_HDR_BYTES], uint8_t *syms, uint16_t at)
{
	uint8_t s = 0;

	for (uint16_t i = 0; i < DSTAR_HDR_SYMS / 2; i++) {
		uint8_t b = i < DSTAR_HDR_BYTES * 8 ? (uint8_t)((raw[i >> 3] >> (i & 7)) & 1) : 0, o1, o2;
		uint16_t k = (uint16_t)(2 * i);

		conv_bits(b, s, &o1, &o2);
		s = (uint8_t)((b << 1) | (s >> 1));
		air_sym_put(syms, (uint16_t)(at + ilv_pos(k)), (o1 ^ dstar_pn(ilv_pos(k))) ? 1 : 3);
		air_sym_put(syms, (uint16_t)(at + ilv_pos(k + 1)), (o2 ^ dstar_pn(ilv_pos(k + 1))) ? 1 : 3);
	}
}

// Code bit k of the header as received, descrambled, as a correlation: + for a 1
typedef struct {
	const uint8_t *hard;
	const int8_t *soft;
	uint16_t at;
} rx_bits_t;

static int8_t code_bit(const rx_bits_t *r, uint16_t k)
{
	uint16_t pos = ilv_pos(k);
	int8_t v = r->soft != NULL ? r->soft[r->at + pos] : ((air_sym_get(r->hard, (uint16_t)(r->at + pos)) & 2) ? -64 : 64);

	if (v == -128)
		v = -127;
	return dstar_pn(pos) ? (int8_t)-v : v;
}

// Viterbi over the four states (X1 X2), the soft values as correlations; swap: the two outputs the other way round.
// Returns the code bits the best path disagrees with
static int viterbi(const rx_bits_t *r, dstar_fec_work_t *w, uint8_t raw[DSTAR_HDR_BYTES], int swap)
{
	int32_t m[4] = { 0, -100000, -100000, -100000 }, nm[4];
	int errs = 0;
	uint8_t s;

	for (uint16_t t = 0; t < DSTAR_HDR_SYMS / 2; t++) {
		int8_t a = code_bit(r, (uint16_t)(2 * t + swap)), b = code_bit(r, (uint16_t)(2 * t + 1 - swap));
		uint8_t d = 0;

		for (uint8_t ns = 0; ns < 4; ns++) {
			uint8_t bit = ns >> 1, x1 = ns & 1;
			int32_t best = INT32_MIN;
			uint8_t pick = 0;

			for (uint8_t x2 = 0; x2 < 2; x2++) {
				uint8_t ps = (uint8_t)((x1 << 1) | x2), o1, o2;
				int32_t v;

				conv_bits(bit, ps, &o1, &o2);
				v = m[ps] + (o1 ? a : -a) + (o2 ? b : -b);
				if (v > best) {
					best = v;
					pick = x2;
				}
			}
			nm[ns] = best;
			d |= (uint8_t)(pick << ns);
		}
		w->dec[t] = d;
		memcpy(m, nm, sizeof m);
	}
	// trace back from state 0: the two tail bits are zeros
	memset(raw, 0, DSTAR_HDR_BYTES);
	s = 0;
	for (int t = DSTAR_HDR_SYMS / 2 - 1; t >= 0; t--) {
		uint8_t bit = s >> 1, x1 = s & 1, x2 = (uint8_t)((w->dec[t] >> s) & 1), ps = (uint8_t)((x1 << 1) | x2), o1, o2;

		conv_bits(bit, ps, &o1, &o2);
		errs += (o1 != (code_bit(r, (uint16_t)(2 * t + swap)) > 0)) + (o2 != (code_bit(r, (uint16_t)(2 * t + 1 - swap)) > 0));
		if (t < DSTAR_HDR_BYTES * 8 && bit)
			raw[t >> 3] |= (uint8_t)(1u << (t & 7));
		s = ps;
	}
	return errs;
}

int dstar_header_decode(const uint8_t *hard, const int8_t *soft, uint16_t at, dstar_fec_work_t *w,
			uint8_t raw[DSTAR_HDR_BYTES])
{
	rx_bits_t r = { hard, soft, at };
	dstar_header_t h;

	for (int swap = G_SWAP_NONE; swap <= G_SWAP; swap++) {
		int errs = viterbi(&r, w, raw, swap);

		if (dstar_header_unpack(raw, &h))
			return errs;
	}
	return -1;
}

// ── Voice frames ──
void dstar_frame_put(uint8_t *syms, uint16_t at, const uint8_t voice[DSTAR_VOICE_BYTES], const uint8_t data[DSTAR_DATA_BYTES])
{
	dstar_bits_put(syms, at, voice, DSTAR_VOICE_BYTES * 8);
	dstar_bits_put(syms, (uint16_t)(at + DSTAR_VOICE_BYTES * 8), data, DSTAR_DATA_BYTES * 8);
}

void dstar_frame_get(const uint8_t *syms, uint16_t at, uint8_t voice[DSTAR_VOICE_BYTES], uint8_t data[DSTAR_DATA_BYTES])
{
	dstar_bits_get(syms, at, voice, DSTAR_VOICE_BYTES * 8);
	dstar_bits_get(syms, (uint16_t)(at + DSTAR_VOICE_BYTES * 8), data, DSTAR_DATA_BYTES * 8);
}
