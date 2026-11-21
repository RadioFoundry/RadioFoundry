// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// dPMR frames and their fields (dpmr.h): the channel codes, the HI, CCH and END fields, and the frames built from and
// read into symbol buffers. Section numbers are ETSI TS 102 490's.
#include "dpmr.h"

#include <string.h>

#define FS24_MASK	0xFFFFFFu

// ── Channel codes (6.1.5, 7.1) ──

// Table 6.1 as its twelve-bit codes (each symbol +3 -> 0, -3 -> 1), in the table's order: 446.103125 MHz first
static const uint16_t cc_a[16] = {
	0x145, 0x1A4, 0x1D4, 0x016, 0x066, 0x087, 0x0F7, 0x303,
	0x373, 0x392, 0x3E2, 0x220, 0x250, 0x2B1, 0x2C1, 0x529,
};
static const uint16_t cc_b[16] = {
	0xD11, 0xD61, 0xD80, 0xDF0, 0xC32, 0xC42, 0xCA3, 0xCD3,
	0xF27, 0xF57, 0xFB6, 0xFC6, 0xE04, 0xE74, 0xE95, 0xEE5,
};

#define CC_LO_HZ	446103125u
#define CC_STEP_HZ	6250u

uint32_t dpmr_cc_air(uint16_t cc)
{
	uint32_t air = 0;

	for (uint32_t i = 0; i < 12u; i++)
		air = (air << 2) | (((cc >> (11u - i)) & 1u) ? 3u : 1u);
	return air;
}

uint16_t dpmr_cc_from_air(uint32_t air, uint8_t *bad)
{
	uint16_t cc = 0;
	uint8_t n = 0;

	for (uint32_t i = 0; i < 12u; i++) {
		uint32_t d = (air >> (22u - 2u * i)) & 3u;

		cc = (uint16_t)((cc << 1) | (d >> 1));
		n += (d & 1u) == 0;	// 00 or 10: an inner symbol where a code has only outer ones
	}
	if (bad != NULL)
		*bad = n;
	return cc;
}

uint16_t dpmr_cc_group(bool group_b, uint8_t n)
{
	return (group_b ? cc_b : cc_a)[n & 15u];
}

uint8_t dpmr_cc_index(uint16_t cc)
{
	for (uint8_t i = 0; i < 16u; i++) {
		if (cc_a[i] == cc)
			return (uint8_t)(i + 1u);
		if (cc_b[i] == cc)
			return (uint8_t)(i + 17u);
	}
	return 0;
}

uint16_t dpmr_cc_for_hz(uint32_t hz)
{
	uint32_t half = CC_STEP_HZ / 2u;

	if (hz + half < CC_LO_HZ || hz >= CC_LO_HZ + 16u * CC_STEP_HZ - half)
		return cc_a[0];
	return cc_a[(hz + half - CC_LO_HZ) / CC_STEP_HZ];
}

// ── The words the screens show ──
// Written by hand rather than with snprintf, which would pull a formatter of several kilobytes into an image that may
// have none, and without the UI's sbuf, which this layer doesn't depend on
static size_t put_str(char *out, size_t n, size_t at, const char *s)
{
	while (*s != '\0' && at + 1u < n)
		out[at++] = *s++;
	return at;
}

static size_t put_num(char *out, size_t n, size_t at, uint32_t v, uint32_t base, uint32_t digits)
{
	char d[10];
	uint32_t k = 0;

	do {
		d[k++] = "0123456789ABCDEF"[v % base];
		v /= base;
	} while ((v != 0 || k < digits) && k < sizeof d);
	while (k > 0 && at + 1u < n)
		out[at++] = d[--k];
	return at;
}

void dpmr_id_text(uint32_t id, char *out, size_t n)
{
	size_t at = 0;

	if (n == 0)
		return;
	if (id == DPMR_ID_ALL) {
		at = put_str(out, n, at, "All");
	} else if (dpmr_id_is_common(id)) {
		at = put_str(out, n, at, "CID ");
		at = put_num(out, n, at, id >> 16, 10u, 1u);
	} else {
		at = put_num(out, n, at, id & 0xFFFFFFu, 10u, 1u);
	}
	out[at] = '\0';
}

void dpmr_cc_text(uint16_t cc, char *out, size_t n)
{
	size_t at = 0;
	uint8_t i = dpmr_cc_index(cc);

	if (n == 0)
		return;
	if (i == 0) {
		at = put_num(out, n, at, cc & 0xFFFu, 16u, 3u);
	} else {
		at = put_str(out, n, at, i > 16u ? "B" : "A");
		at = put_num(out, n, at, i > 16u ? i - 16u : i, 10u, 1u);
	}
	out[at] = '\0';
}

// ── The information fields ──

void dpmr_hi_pack(const dpmr_hi_t *h, uint8_t data[DPMR_HI_BYTES])
{
	memset(data, 0, DPMR_HI_BYTES);
	dpmr_put(data, 0, h->ht, 4u);
	dpmr_put(data, 4, h->called, 24u);
	dpmr_put(data, 28, h->own, 24u);
	dpmr_put(data, 52, h->mode, 3u);
	dpmr_put(data, 55, h->version, 2u);
	dpmr_put(data, 57, h->format, 2u);
	dpmr_put(data, 59, 0u, 2u);		// reserved
	dpmr_put(data, 61, h->ci, 11u);
}

void dpmr_hi_unpack(const uint8_t data[DPMR_HI_BYTES], dpmr_hi_t *h)
{
	h->ht = (uint8_t)dpmr_get(data, 0, 4u);
	h->called = dpmr_get(data, 4, 24u);
	h->own = dpmr_get(data, 28, 24u);
	h->mode = (uint8_t)dpmr_get(data, 52, 3u);
	h->version = (uint8_t)dpmr_get(data, 55, 2u);
	h->format = (uint8_t)dpmr_get(data, 57, 2u);
	h->ci = (uint16_t)dpmr_get(data, 61, 11u);
}

void dpmr_cch_pack(const dpmr_cch_t *c, uint8_t data[DPMR_CCH_BYTES])
{
	memset(data, 0, DPMR_CCH_BYTES);
	dpmr_put(data, 0, c->fn, 2u);
	dpmr_put(data, 2, c->id12, 12u);
	dpmr_put(data, 14, c->mode, 3u);
	dpmr_put(data, 17, c->version, 2u);
	dpmr_put(data, 19, c->format, 2u);
	dpmr_put(data, 21, 0u, 2u);		// reserved
	dpmr_put(data, 23, c->sld, 18u);
}

void dpmr_cch_unpack(const uint8_t data[DPMR_CCH_BYTES], dpmr_cch_t *c)
{
	c->fn = (uint8_t)dpmr_get(data, 0, 2u);
	c->id12 = (uint16_t)dpmr_get(data, 2, 12u);
	c->mode = (uint8_t)dpmr_get(data, 14, 3u);
	c->version = (uint8_t)dpmr_get(data, 17, 2u);
	c->format = (uint8_t)dpmr_get(data, 19, 2u);
	c->sld = dpmr_get(data, 23, 18u);
}

uint16_t dpmr_id_quarter(const dpmr_hi_t *h, uint8_t fn)
{
	uint32_t id = (fn & 3u) < 2u ? h->called : h->own;

	return (uint16_t)((fn & 1u) ? id & 0xFFFu : (id >> 12) & 0xFFFu);
}

void dpmr_end_pack(const dpmr_end_info_t *e, uint8_t data[DPMR_END_BYTES])
{
	memset(data, 0, DPMR_END_BYTES);
	dpmr_put(data, 0, e->et, 2u);
	dpmr_put(data, 2, e->arq, 2u);
	dpmr_put(data, 4, e->wait, 4u);
	dpmr_put(data, 8, e->status, 5u);
	dpmr_put(data, 13, 0u, 4u);		// reserved
}

void dpmr_end_unpack(const uint8_t data[DPMR_END_BYTES], dpmr_end_info_t *e)
{
	e->et = (uint8_t)dpmr_get(data, 0, 2u);
	e->arq = (uint8_t)dpmr_get(data, 2, 2u);
	e->wait = (uint8_t)dpmr_get(data, 4, 4u);
	e->status = (uint8_t)dpmr_get(data, 8, 5u);
}

// ── Frames as symbols: a symbol buffer is its bits in air order, so the fields go in with dpmr_put ──

static void copy_in(uint8_t *dst, uint32_t at, const uint8_t *src, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++)
		dpmr_bit_put(dst, at + i, dpmr_bit(src, i));
}

static void copy_out(const uint8_t *src, uint32_t at, uint8_t *dst, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++)
		dpmr_bit_put(dst, i, dpmr_bit(src, at + i));
}

void dpmr_header_put(uint8_t *frame, const dpmr_hi_t *h, uint16_t cc)
{
	uint8_t data[DPMR_HI_BYTES], hi[DPMR_HI_CODED_BITS / 8u];
	uint32_t at = 2u * DPMR_PREAMBLE_SYMS;

	dpmr_hi_pack(h, data);
	dpmr_hi_encode(data, hi);
	memset(frame, DPMR_PREAMBLE_BYTE, 2u * DPMR_PREAMBLE_SYMS / 8u);
	dpmr_put(frame, at, (uint32_t)(DPMR_FS1 >> 24), 24u);
	dpmr_put(frame, at + 24u, (uint32_t)(DPMR_FS1 & FS24_MASK), 24u);
	at += 2u * DPMR_FS1_SYMS;
	copy_in(frame, at, hi, DPMR_HI_CODED_BITS);
	at += DPMR_HI_CODED_BITS;
	dpmr_put(frame, at, dpmr_cc_air(cc), 24u);
	at += 24u;
	copy_in(frame, at, hi, DPMR_HI_CODED_BITS);
}

bool dpmr_header_get(const uint8_t *hdr, dpmr_hi_t *h, dpmr_block_result_t *res)
{
	static const uint16_t at[2] = { 2u * DPMR_HDR_HI0_SYM, 2u * DPMR_HDR_HI1_SYM };
	uint8_t coded[DPMR_HI_CODED_BITS / 8u], data[DPMR_HI_BYTES];
	uint8_t fixed = 0, bad = 0;

	// The two copies are the same block: the first whose CRC holds is the header, a fade on one costs nothing
	for (uint32_t k = 0; k < 2u; k++) {
		copy_out(hdr, at[k], coded, DPMR_HI_CODED_BITS);
		dpmr_hi_decode(coded, data, res);
		fixed = (uint8_t)(fixed + res->fixed);
		bad = (uint8_t)(bad + res->bad);
		if (res->crc_ok)
			break;
	}
	res->fixed = fixed;
	res->bad = bad;
	if (!res->crc_ok)
		return false;
	dpmr_hi_unpack(data, h);
	return true;
}

void dpmr_payload_put(uint8_t *frame, const dpmr_cch_t *c, uint16_t cc)
{
	uint8_t data[DPMR_CCH_BYTES], cch[DPMR_CCH_CODED_BITS / 8u];

	dpmr_put(frame, 0, (c->fn & 1u) ? dpmr_cc_air(cc) : DPMR_FS2, 24u);
	dpmr_cch_pack(c, data);
	dpmr_cch_encode(data, cch);
	copy_in(frame, 2u * DPMR_CCH_SYM, cch, DPMR_CCH_CODED_BITS);
}

bool dpmr_cch_get(const uint8_t *frame, dpmr_cch_t *c, dpmr_block_result_t *res)
{
	uint8_t coded[DPMR_CCH_CODED_BITS / 8u], data[DPMR_CCH_BYTES];

	copy_out(frame, 2u * DPMR_CCH_SYM, coded, DPMR_CCH_CODED_BITS);
	dpmr_cch_decode(coded, data, res);
	if (!res->crc_ok)
		return false;
	dpmr_cch_unpack(data, c);
	return true;
}

void dpmr_tch_put(uint8_t *frame, uint8_t n, const uint8_t ambe[DPMR_AMBE_BYTES])
{
	copy_in(frame, 2u * DPMR_TCH_SYM(n & 3u), ambe, 72u);
}

void dpmr_tch_get(const uint8_t *frame, uint8_t n, uint8_t ambe[DPMR_AMBE_BYTES])
{
	copy_out(frame, 2u * DPMR_TCH_SYM(n & 3u), ambe, 72u);
}

void dpmr_end_put(uint8_t *frame, const dpmr_end_info_t *e)
{
	uint8_t data[DPMR_END_BYTES], end[DPMR_END_CODED_BITS / 8u];

	dpmr_end_pack(e, data);
	dpmr_end_encode(data, end);
	dpmr_put(frame, 0, DPMR_FS3, 24u);
	copy_in(frame, 24u, end, DPMR_END_CODED_BITS);
}

bool dpmr_end_get(const uint8_t *frame, dpmr_end_info_t *e, dpmr_block_result_t *res)
{
	uint8_t coded[DPMR_END_CODED_BITS / 8u], data[DPMR_END_BYTES];

	copy_out(frame, 24u, coded, DPMR_END_CODED_BITS);
	dpmr_end_decode(coded, data, res);
	if (!res->crc_ok)
		return false;
	dpmr_end_unpack(data, e);
	return true;
}

uint32_t dpmr_sync_field(const uint8_t *frame)
{
	return dpmr_get(frame, 0, 24u);
}
