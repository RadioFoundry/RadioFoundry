// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The YSF frame's channels (ysf.h, docs/spec/ysf.md 2-3): the FICH, the long channels (CSD), the DCH and
// the VCH, with their CRC, Golay and convolutional codes, whitening and interleaving.
#include "ysf.h"

#include <string.h>

#include "../air/air_syms.h"
#include "../fec/conv.h"
#include "../fec/crc16.h"

const uint8_t ysf_sync[5] = { 0xD4, 0x71, 0xC9, 0x63, 0x4D };

// ── Frame bits and their soft values ──
// A frame bit's soft value (conv.h: positive says 1). A dibit's first bit is its sign (1 for -1 and -3), the second
// its size (1 for the outer levels). Without soft values, every bit the same size
static int8_t fbit_soft(const uint8_t *hard, const int8_t *soft, uint16_t j)
{
	int v;

	if (!soft)
		return air_bit_get(hard, j) ? 32 : -32;
	v = soft[j >> 1];
	if (j & 1)
		v = (v < 0 ? -v : v) - 2 * AIR_SOFT_OUTER / 3;
	else
		v = -v;
	return (int8_t)(v > 127 ? 127 : v < -127 ? -127 : v);
}

// ── Whitening (ysf.md 3.3): x^9 + x^5 + 1, each output the XOR of the outputs 5 and 9 before it; the first nine
// are 1 0 0 1 0 0 1 1 1 ──
#define PN_SEED		0x127u	// the first nine outputs, the first in bit 8

static uint8_t pn_next(uint16_t *r)
{
	uint8_t out = (uint8_t)((*r >> 8) & 1);
	uint8_t in = (uint8_t)(((*r >> 4) ^ (*r >> 8)) & 1);

	*r = (uint16_t)(((*r << 1) | in) & 0x1FF);
	return out;
}

uint8_t ysf_pn(uint16_t i)
{
	uint16_t r = PN_SEED;
	uint8_t b = 0;

	for (uint16_t k = 0; k <= i; k++)
		b = pn_next(&r);
	return b;
}

static void whiten(uint8_t *p, uint16_t nbits)
{
	uint16_t r = PN_SEED;

	for (uint16_t i = 0; i < nbits; i++)
		if (pn_next(&r))
			p[i >> 3] ^= (uint8_t)(0x80u >> (i & 7));
}

// ── CRC-16 (ysf.md 3.2): x^16 + x^12 + x^5 + 1, register cleared, the result inverted, sent high byte first ──

static void crc_append(uint8_t *p, uint16_t len)
{
	uint16_t c = fec_crc_ccitt(p, len);

	p[len] = (uint8_t)(c >> 8);
	p[len + 1] = (uint8_t)c;
}

static bool crc_ok(const uint8_t *p, uint16_t len)
{
	uint16_t c = fec_crc_ccitt(p, len);

	return p[len] == (uint8_t)(c >> 8) && p[len + 1] == (uint8_t)c;
}

// ── Coded channels: K = 5 rate 1/2 (G1 = 1 + D3 + D4, G2 = 1 + D + D2 + D4), four tail bits, the coded dibits
// written 20 to a column. Coded dibit i goes to channel position (i mod cols) x 20 + i / cols ──
static const fec_conv_code_t code = { 0x19, 0x17 };

typedef struct {
	uint8_t cols;		// 20 x cols input bits (the last four the tail), as many coded dibits
	uint8_t at;		// first symbol: in the frame (FICH), or in each payload block
	bool blocks;		// spread over the five payload blocks, 4 x cols symbols in each
} chan_t;

static const chan_t fich_ch = { 5, YSF_FICH_SYM, false };
static const chan_t dch_ch = { 5, 0, true };
static const chan_t csd_ch[2] = { { 9, 0, true }, { 9, 36, true } };

// The frame symbol of channel position p
static uint16_t chan_sym(const chan_t *c, uint16_t p)
{
	uint16_t per = (uint16_t)(4 * c->cols);

	if (!c->blocks)
		return (uint16_t)(c->at + p);
	return (uint16_t)(YSF_BLOCK_SYM(p / per) + c->at + p % per);
}

static uint16_t chan_pos(const chan_t *c, uint16_t i)
{
	return (uint16_t)((i % c->cols) * 20 + i / c->cols);
}

// in: 20 x cols input bits, tail included (zero)
static void chan_put(const chan_t *c, uint8_t *frame, const uint8_t *in)
{
	uint8_t enc[45] = { 0 };
	uint16_t n = (uint16_t)(20 * c->cols);

	fec_conv_encode(&code, in, (uint16_t)(20 * c->cols), enc);
	for (uint16_t i = 0; i < n; i++) {
		uint8_t d = (uint8_t)(air_bit_get(enc, (uint16_t)(2 * i)) << 1 | air_bit_get(enc, (uint16_t)(2 * i + 1)));

		air_sym_put(frame, chan_sym(c, chan_pos(c, i)), d);
	}
}

static int chan_get(const chan_t *c, const uint8_t *hard, const int8_t *soft, ysf_fec_work_t *w, uint8_t *out)
{
	uint16_t n = (uint16_t)(20 * c->cols);

	for (uint16_t i = 0; i < n; i++) {
		uint16_t s = chan_sym(c, chan_pos(c, i));

		w->soft[2 * i] = fbit_soft(hard, soft, (uint16_t)(2 * s));
		w->soft[2 * i + 1] = fbit_soft(hard, soft, (uint16_t)(2 * s + 1));
	}
	return fec_conv_decode(&code, w->soft, (uint16_t)(20 * c->cols), w->dec, out);
}

// ── Sync ──
void ysf_frame_begin(uint8_t *frame)
{
	memcpy(frame, ysf_sync, sizeof ysf_sync);
}

unsigned ysf_sync_errs(const uint8_t *hard)
{
	unsigned n = 0;

	for (uint8_t i = 0; i < sizeof ysf_sync; i++)
		for (uint8_t x = (uint8_t)(hard[i] ^ ysf_sync[i]); x; x &= (uint8_t)(x - 1))
			n++;
	return n;
}

// ── FICH (ysf.md 3.1): four octets of fields and the CRC, as four Golay(24,12) words, coded and interleaved ──
static void fich_pack(const ysf_fich_t *f, uint8_t b[4])
{
	b[0] = (uint8_t)((f->fi & 3) << 6 | (f->cs & 3) << 4 | (f->cm & 3) << 2 | (f->bn & 3));
	b[1] = (uint8_t)((f->bt & 3) << 6 | (f->fn & 7) << 3 | (f->ft & 7));
	b[2] = (uint8_t)((f->dev ? 0x40 : 0) | (f->mr & 7) << 3 | (f->voip ? 0x04 : 0) | (f->dt & 3));
	b[3] = (uint8_t)((f->sql ? 0x80 : 0) | (f->dgid & 0x7F));
}

static void fich_unpack(const uint8_t b[4], ysf_fich_t *f)
{
	f->fi = b[0] >> 6;
	f->cs = (b[0] >> 4) & 3;
	f->cm = (b[0] >> 2) & 3;
	f->bn = b[0] & 3;
	f->bt = b[1] >> 6;
	f->fn = (b[1] >> 3) & 7;
	f->ft = b[1] & 7;
	f->dev = (b[2] & 0x40) != 0;
	f->mr = (b[2] >> 3) & 7;
	f->voip = (b[2] & 0x04) != 0;
	f->dt = b[2] & 3;
	f->sql = (b[3] & 0x80) != 0;
	f->dgid = b[3] & 0x7F;
}

void ysf_fich_put(uint8_t *frame, const ysf_fich_t *f)
{
	uint8_t b[6], in[13] = { 0 };

	fich_pack(f, b);
	crc_append(b, 4);
	// 48 bits as four 12-bit words, each to 24 bits: 96 bits and four tail bits
	for (uint8_t k = 0; k < 4; k++) {
		uint16_t d = 0;
		uint32_t cw;

		for (uint8_t i = 0; i < 12; i++)
			d = (uint16_t)(d << 1 | air_bit_get(b, (uint16_t)(12 * k + i)));
		cw = ambeGolay24Encode(d);
		for (uint8_t i = 0; i < 24; i++)
			air_bit_put(in, (uint16_t)(24 * k + i), (uint8_t)((cw >> (23 - i)) & 1));
	}
	chan_put(&fich_ch, frame, in);
}

int ysf_fich_get(const uint8_t *hard, const int8_t *soft, ysf_fec_work_t *w, ysf_fich_t *f)
{
	uint8_t in[13] = { 0 }, b[6] = { 0 };
	int errs = chan_get(&fich_ch, hard, soft, w, in);

	for (uint8_t k = 0; k < 4; k++) {
		uint32_t cw = 0;
		uint16_t d;
		int e;

		for (uint8_t i = 0; i < 24; i++)
			cw = cw << 1 | air_bit_get(in, (uint16_t)(24 * k + i));
		e = ambeGolay24Decode(cw, &d);
		if (e < 0)
			return -1;
		errs += e;
		for (uint8_t i = 0; i < 12; i++)
			air_bit_put(b, (uint16_t)(12 * k + i), (uint8_t)((d >> (11 - i)) & 1));
	}
	if (!crc_ok(b, 4))
		return -1;
	fich_unpack(b, f);
	return errs;
}

// ── Data channels (ysf.md 3.2): the content whitened, the CRC over what is sent, coded and spread ──
static void data_put(const chan_t *c, uint8_t *frame, const uint8_t *content, uint8_t len)
{
	uint8_t in[23] = { 0 };		// content, CRC, the tail bits' octet

	memcpy(in, content, len);
	whiten(in, (uint16_t)(8 * len));
	crc_append(in, len);
	chan_put(c, frame, in);
}

static int data_get(const chan_t *c, const uint8_t *hard, const int8_t *soft, ysf_fec_work_t *w, uint8_t *out,
		    uint8_t len)
{
	uint8_t in[23] = { 0 };
	int errs = chan_get(c, hard, soft, w, in);

	if (!crc_ok(in, len))
		return -1;
	whiten(in, (uint16_t)(8 * len));
	memcpy(out, in, len);
	return errs;
}

void ysf_csd_put(uint8_t *frame, const uint8_t csd1[YSF_CSD_BYTES], const uint8_t csd2[YSF_CSD_BYTES])
{
	data_put(&csd_ch[0], frame, csd1, YSF_CSD_BYTES);
	data_put(&csd_ch[1], frame, csd2, YSF_CSD_BYTES);
}

int ysf_csd_get(const uint8_t *hard, const int8_t *soft, uint8_t which, ysf_fec_work_t *w, uint8_t out[YSF_CSD_BYTES])
{
	return data_get(&csd_ch[which & 1], hard, soft, w, out, YSF_CSD_BYTES);
}

void ysf_dch_put(uint8_t *frame, const uint8_t in[YSF_DCH_BYTES])
{
	data_put(&dch_ch, frame, in, YSF_DCH_BYTES);
}

int ysf_dch_get(const uint8_t *hard, const int8_t *soft, ysf_fec_work_t *w, uint8_t out[YSF_DCH_BYTES])
{
	return data_get(&dch_ch, hard, soft, w, out, YSF_DCH_BYTES);
}

// ── VCH (ysf.md 3.4): parameter bits 0-26 sent three times in a row, 27-48 once, a zero bit; whitened; written 4 to
// a column of 26 rows: bit i at (i mod 26) x 4 + i / 26 ──
#define VCH_BITS	104
#define VCH_TRIPLED	27

static uint16_t vch_bit(uint8_t n, uint16_t i)
{
	return (uint16_t)(2 * (YSF_BLOCK_SYM(n) + 20) + (i % 26) * 4 + i / 26);
}

void ysf_vch_put(uint8_t *frame, uint8_t n, const uint8_t bits[AMBE_PARAM_BITS])
{
	uint8_t raw[VCH_BITS / 8] = { 0 };
	uint16_t j = 0;

	for (uint8_t i = 0; i < AMBE_PARAM_BITS; i++)
		for (uint8_t r = i < VCH_TRIPLED ? 3 : 1; r; r--)
			air_bit_put(raw, j++, bits[i] & 1);
	whiten(raw, VCH_BITS);
	for (uint16_t i = 0; i < VCH_BITS; i++) {
		uint16_t b = vch_bit(n, i);
		uint8_t d = air_sym_get(frame, (uint16_t)(b >> 1));

		d = (b & 1) ? (uint8_t)((d & 2) | air_bit_get(raw, i)) : (uint8_t)(air_bit_get(raw, i) << 1 | (d & 1));
		air_sym_put(frame, (uint16_t)(b >> 1), d);
	}
}

int ysf_vch_get(const uint8_t *hard, const int8_t *soft, uint8_t n, uint8_t bits[AMBE_PARAM_BITS])
{
	uint16_t r = PN_SEED, j = 0;
	int fixed = 0;
	int8_t v[VCH_BITS];

	// Soft values in sent order, the whitening undone by turning the sign
	for (uint16_t i = 0; i < VCH_BITS; i++) {
		int s = fbit_soft(hard, soft, vch_bit(n, i));

		v[i] = (int8_t)(pn_next(&r) ? -s : s);
	}
	for (uint8_t i = 0; i < AMBE_PARAM_BITS; i++) {
		if (i < VCH_TRIPLED) {
			int a = v[j], b = v[j + 1], c = v[j + 2];

			bits[i] = (uint8_t)(a + b + c > 0);
			fixed += ((a > 0) != (b > 0)) || ((a > 0) != (c > 0));
			j += 3;
		} else {
			bits[i] = (uint8_t)(v[j++] > 0);
		}
	}
	return fixed;
}
