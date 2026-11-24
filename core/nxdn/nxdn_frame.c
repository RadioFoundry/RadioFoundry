// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The NXDN frame's channels (nxdn.h): scrambling, the LICH, SACCH and FACCH1 with their CRC, convolutional code,
// puncturing and interleaving, the VCH, and the VCALL and TX_REL messages. Section numbers are NXDN TS 1-A's.
#include "nxdn.h"

#include <string.h>

#include "../air/air_syms.h"

// ── Scrambling (4.6) ──
// Every symbol behind the FSW has its sign inverted where the PN sequence is 1: X^9 + X^4 + 1, loaded with
// S8..S0 = 0 1 1 1 0 0 1 0 0 at each frame's first LICH symbol, one output per symbol, the first output before the
// first shift. These are its 182 bits, MSB first (nxdnTest.c runs the register against them)
static const uint8_t pn[23] = {
	0x27, 0x2A, 0xC3, 0x7A, 0x6E, 0x45, 0x0A, 0xD3, 0xF6, 0x49, 0x6F, 0xC9,
	0xA9, 0x98, 0x0C, 0x65, 0x1A, 0x5F, 0xD1, 0x63, 0xAC, 0xB3, 0xC4,
};

uint8_t nxdn_pn(uint16_t sym)
{
	if (sym < NXDN_FSW_SYMS || sym >= NXDN_FRAME_SYMS)
		return 0;
	sym = (uint16_t)(sym - NXDN_FSW_SYMS);
	return (uint8_t)((pn[sym >> 3] >> (7 - (sym & 7))) & 1);
}

// Frame bit j (0..383, MSB of each dibit first), descrambled: an inverted sign is the dibit's high bit flipped
static uint8_t fbit_get(const uint8_t *hard, uint16_t j)
{
	uint8_t d = air_sym_get(hard, (uint16_t)(j >> 1));

	return (j & 1) ? (uint8_t)(d & 1) : (uint8_t)((d >> 1) ^ nxdn_pn((uint16_t)(j >> 1)));
}

static void fbit_put(uint8_t *frame, uint16_t j, uint8_t b)
{
	uint16_t s = (uint16_t)(j >> 1);
	uint8_t d = air_sym_get(frame, s);

	if (j & 1)
		d = (uint8_t)((d & 2) | (b & 1));
	else
		d = (uint8_t)(((b ^ nxdn_pn(s)) & 1) << 1 | (d & 1));
	air_sym_put(frame, s, d);
}

// Frame bit j as a soft value (conv.h: positive 1): the high bit from the sign, the low bit from the size (outer
// symbols are 1). Without soft values, every bit the same size (hard decisions)
static int8_t fbit_soft(const uint8_t *hard, const int8_t *soft, uint16_t j)
{
	uint16_t s = (uint16_t)(j >> 1);
	int v;

	if (!soft)
		return fbit_get(hard, j) ? 32 : -32;
	v = soft[s];
	if (j & 1) {
		v = (v < 0 ? -v : v) - 2 * AIR_SOFT_OUTER / 3;
	} else {
		v = nxdn_pn(s) ? v : -v;
	}
	return (int8_t)(v > 127 ? 127 : v < -127 ? -127 : v);
}

static uint8_t bit_get(const uint8_t *p, uint16_t i)
{
	return (uint8_t)((p[i >> 3] >> (7 - (i & 7))) & 1);
}

static void bit_put(uint8_t *p, uint16_t i, uint8_t b)
{
	uint8_t m = (uint8_t)(0x80u >> (i & 7));

	p[i >> 3] = (uint8_t)(b ? (p[i >> 3] | m) : (p[i >> 3] & ~m));
}

// ── CRC (4.5.5): MSB first, the register loaded with ones; its bits follow the data, the highest first ──
uint16_t nxdn_crc(const uint8_t *bits, uint16_t n, uint8_t width, uint16_t poly)
{
	uint16_t mask = (uint16_t)((1u << width) - 1), r = mask;

	for (uint16_t i = 0; i < n; i++) {
		uint8_t fb = (uint8_t)(bit_get(bits, i) ^ ((r >> (width - 1)) & 1));

		r = (uint16_t)((r << 1) & mask);
		if (fb)
			r ^= poly;
	}
	return r;
}

#define CRC6_POLY	0x27	// x6 + x5 + x2 + x + 1 (SACCH)
#define CRC12_POLY	0x80F	// x12 + x11 + x3 + x2 + x + 1 (FACCH1)

// ── The convolutional code (4.5.1): K = 5, G1 = 1 + D3 + D4, G2 = 1 + D + D2 + D4 ──
static const fec_conv_code_t code = { 0x19, 0x17 };

// One coded channel's geometry: input bits (data, CRC, 4 tail), CRC width, its puncture pattern (one bit per G2
// output in a period: 0 erased; G1 outputs are never erased here) and interleaver columns
typedef struct {
	uint8_t data_bits, crc_width;
	uint16_t crc_poly;
	uint8_t period, g2_keep;	// bit k (MSB first of `period`): G2 of input k in the period is sent
	uint8_t cols;
	uint16_t sym0;			// its first symbol in the frame
	uint8_t coded;			// bits on air
} chan_t;

// SACCH (4.5.2, figure 4.5-4): 26 + 6 + 4 = 36 -> 72 -> punctured 111111 / 110110 -> 60, 5 x 12
static const chan_t sacch = { 26, 6, CRC6_POLY, 6, 0x36, 12, NXDN_SACCH_SYM, 60 };
// FACCH1 (figure 4.5-5): 80 + 12 + 4 = 96 -> 192 -> punctured 11 / 01 -> 144, 9 x 16
static const chan_t facch1 = { 80, 12, CRC12_POLY, 2, 0x01, 16, NXDN_VCH_SYM(0), 144 };

static bool g2_kept(const chan_t *c, uint16_t i)
{
	return (c->g2_keep >> (c->period - 1 - i % c->period)) & 1;
}

// The interleaver: punctured bits written row by row into rows of `cols`, read column by column. Bit y of the
// punctured stream goes to air position
static uint16_t ileave(const chan_t *c, uint16_t y)
{
	uint8_t rows = (uint8_t)(c->coded / c->cols);

	return (uint16_t)((y % c->cols) * rows + y / c->cols);
}

static void chan_put(const chan_t *c, uint8_t *frame, uint16_t sym0, const uint8_t *data)
{
	uint8_t in[12] = { 0 }, enc[24];
	uint16_t n = (uint16_t)(c->data_bits + c->crc_width + 4), crc, y = 0;

	for (uint16_t i = 0; i < c->data_bits; i++)
		bit_put(in, i, bit_get(data, i));
	crc = nxdn_crc(in, c->data_bits, c->crc_width, c->crc_poly);
	for (uint8_t i = 0; i < c->crc_width; i++)
		bit_put(in, (uint16_t)(c->data_bits + i), (uint8_t)((crc >> (c->crc_width - 1 - i)) & 1));
	fec_conv_encode(&code, in, n, enc);
	for (uint16_t i = 0; i < n; i++) {
		fbit_put(frame, (uint16_t)(2 * sym0 + ileave(c, y++)), bit_get(enc, (uint16_t)(2 * i)));
		if (g2_kept(c, i))
			fbit_put(frame, (uint16_t)(2 * sym0 + ileave(c, y++)), bit_get(enc, (uint16_t)(2 * i + 1)));
	}
}

static int chan_get(const chan_t *c, const uint8_t *hard, const int8_t *soft, uint16_t sym0, nxdn_fec_work_t *w,
		    uint8_t *data)
{
	uint8_t dec[12];
	uint16_t n = (uint16_t)(c->data_bits + c->crc_width + 4), y = 0, crc;
	int errs;

	for (uint16_t i = 0; i < n; i++) {
		w->soft[2 * i] = fbit_soft(hard, soft, (uint16_t)(2 * sym0 + ileave(c, y++)));
		w->soft[2 * i + 1] = g2_kept(c, i) ? fbit_soft(hard, soft, (uint16_t)(2 * sym0 + ileave(c, y++))) : 0;
	}
	errs = fec_conv_decode(&code, w->soft, n, w->dec, dec);
	crc = 0;
	for (uint8_t i = 0; i < c->crc_width; i++)
		crc = (uint16_t)(crc << 1 | bit_get(dec, (uint16_t)(c->data_bits + i)));
	memset(data, 0, (size_t)(c->data_bits + 7) / 8);
	for (uint16_t i = 0; i < c->data_bits; i++)
		bit_put(data, i, bit_get(dec, i));
	return nxdn_crc(dec, c->data_bits, c->crc_width, c->crc_poly) == crc ? errs : -1;
}

void nxdn_sacch_put(uint8_t *frame, const uint8_t in[NXDN_SACCH_BYTES])
{
	chan_put(&sacch, frame, sacch.sym0, in);
}

int nxdn_sacch_get(const uint8_t *hard, const int8_t *soft, nxdn_fec_work_t *w, uint8_t out[NXDN_SACCH_BYTES])
{
	return chan_get(&sacch, hard, soft, sacch.sym0, w, out);
}

// A FACCH1 takes VCH 1-2 or VCH 3-4 (72 symbols)
void nxdn_facch1_put(uint8_t *frame, uint8_t half, const uint8_t in[NXDN_FACCH1_BYTES])
{
	chan_put(&facch1, frame, NXDN_VCH_SYM(half ? 2 : 0), in);
}

int nxdn_facch1_get(const uint8_t *hard, const int8_t *soft, uint8_t half, nxdn_fec_work_t *w,
		    uint8_t out[NXDN_FACCH1_BYTES])
{
	return chan_get(&facch1, hard, soft, NXDN_VCH_SYM(half ? 2 : 0), w, out);
}

// ── VCH (4.4, 7.2.1): 72 bits in order, scrambled like the rest ──
void nxdn_vch_put(uint8_t *frame, uint8_t n, const uint8_t in[NXDN_EHR_BYTES])
{
	for (uint16_t i = 0; i < 72; i++)
		fbit_put(frame, (uint16_t)(2 * NXDN_VCH_SYM(n) + i), bit_get(in, i));
}

void nxdn_vch_get(const uint8_t *hard, uint8_t n, uint8_t out[NXDN_EHR_BYTES])
{
	for (uint16_t i = 0; i < 72; i++)
		bit_put(out, i, fbit_get(hard, (uint16_t)(2 * NXDN_VCH_SYM(n) + i)));
}

// ── FSW (4.4.4) and LICH (4.5.3) ──
static const uint8_t fsw[3] = { 0xCD, 0xF5, 0x90 };	// -3 +1 -3 +3 -3 -3 +3 +3 -1 +3

// The deviation pattern +3 +3 -3 -3 (3.6) three times, then the preamble of table 4.4-1
const uint8_t nxdn_preamble[NXDN_PREAMBLE_SYMS / 4] = { 0x5F, 0x5F, 0x5F, 0x57, 0x75, 0xFD };

static uint8_t lich_parity(uint8_t lich)
{
	return (uint8_t)(((lich >> 6) ^ (lich >> 5) ^ (lich >> 4) ^ (lich >> 3)) & 1);
}

void nxdn_frame_begin(uint8_t *frame, uint8_t lich)
{
	uint8_t l8 = (uint8_t)((lich & 0x7F) << 1 | lich_parity(lich));

	air_syms_copy(frame, 0, fsw, 0, NXDN_FSW_SYMS);
	// Each bit is a dibit: 0 = 01 (+3), 1 = 11 (-3)
	for (uint8_t i = 0; i < 8; i++) {
		fbit_put(frame, (uint16_t)(2 * (NXDN_LICH_SYM + i)), (uint8_t)((l8 >> (7 - i)) & 1));
		fbit_put(frame, (uint16_t)(2 * (NXDN_LICH_SYM + i) + 1), 1);
	}
}

int nxdn_lich_get(const uint8_t *hard, const int8_t *soft)
{
	uint8_t l8 = 0;

	(void)soft;		// the sign is in the hard symbol
	for (uint8_t i = 0; i < 8; i++)
		l8 = (uint8_t)(l8 << 1 | fbit_get(hard, (uint16_t)(2 * (NXDN_LICH_SYM + i))));
	if (lich_parity((uint8_t)(l8 >> 1)) != (l8 & 1))
		return -1;
	return l8 >> 1;
}

// ── VCALL and TX_REL (6.4.1.1, 6.4.1.3) ──
void nxdn_vcall_build(const nxdn_vcall_t *v, uint8_t *out, uint8_t len)
{
	memset(out, 0, len);
	out[0] = v->msg & 0x3F;
	out[1] = v->cc_opt;
	out[2] = (uint8_t)((v->call_type & 7) << 5 | (v->msg == NXDN_MSG_VCALL ? v->voice_opt & 0x1F : 0));
	out[3] = (uint8_t)(v->src >> 8);
	out[4] = (uint8_t)v->src;
	out[5] = (uint8_t)(v->dst >> 8);
	out[6] = (uint8_t)v->dst;
	if (v->msg == NXDN_MSG_VCALL)
		out[7] = (uint8_t)((v->cipher & 3) << 6 | (v->key_id & 0x3F));
}

bool nxdn_vcall_parse(const uint8_t *msg, nxdn_vcall_t *v)
{
	uint8_t t = nxdn_msg_type(msg);

	if (t != NXDN_MSG_VCALL && t != NXDN_MSG_TX_REL)
		return false;
	memset(v, 0, sizeof *v);
	v->msg = t;
	v->cc_opt = msg[1];
	v->call_type = msg[2] >> 5;
	v->src = (uint16_t)(msg[3] << 8 | msg[4]);
	v->dst = (uint16_t)(msg[5] << 8 | msg[6]);
	if (t == NXDN_MSG_VCALL) {
		v->voice_opt = msg[2] & 0x1F;
		v->cipher = msg[7] >> 6;
		v->key_id = msg[7] & 0x3F;
	}
	return true;
}
