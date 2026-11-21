// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// dPMR coding (dpmr_fec.h)
#include <string.h>
#include "dpmr_fec.h"

void dpmr_put(uint8_t *buf, uint32_t at, uint32_t v, uint32_t nbits)
{
	for (uint32_t i = 0; i < nbits; i++) {
		dpmr_bit_put(buf, at + i, ((v >> (nbits - 1u - i)) & 1u) != 0);
	}
}

uint32_t dpmr_get(const uint8_t *buf, uint32_t at, uint32_t nbits)
{
	uint32_t v = 0;
	for (uint32_t i = 0; i < nbits; i++) {
		v = (v << 1) | (dpmr_bit(buf, at + i) ? 1u : 0u);
	}
	return v;
}

static uint32_t crc(const uint8_t *buf, uint32_t nbits, uint32_t poly, uint32_t width)
{
	uint32_t top = 1u << (width - 1u), mask = (1u << width) - 1u, reg = 0;
	for (uint32_t i = 0; i < nbits; i++) {
		bool fb = ((reg & top) != 0) != dpmr_bit(buf, i);
		reg = (reg << 1) & mask;
		if (fb) {
			reg ^= poly;
		}
	}
	return reg;
}

uint8_t dpmr_crc7(const uint8_t *buf, uint32_t nbits)
{
	return (uint8_t)crc(buf, nbits, 0x09u, 7u);
}

uint8_t dpmr_crc8(const uint8_t *buf, uint32_t nbits)
{
	return (uint8_t)crc(buf, nbits, 0x07u, 8u);
}

// The parity each data bit contributes, X7 first: the generator matrix of table 7.1, columns C3..C0. Every row has two
// bits or more and no two are alike, so a single error's syndrome names its bit: a data bit by its row, a parity bit
// by its one set bit
static const uint8_t ham_rows[8] = { 0xEu, 0x7u, 0xAu, 0x5u, 0xBu, 0xCu, 0x6u, 0x3u };

static uint8_t ham_parity(uint8_t data)
{
	uint8_t p = 0;
	for (uint32_t i = 0; i < 8u; i++) {
		if (data & (0x80u >> i)) {
			p ^= ham_rows[i];
		}
	}
	return p;
}

uint16_t dpmr_ham_encode(uint8_t data)
{
	return (uint16_t)(((uint16_t)data << 4) | ham_parity(data));
}

int dpmr_ham_decode(uint16_t word, uint8_t *data)
{
	uint8_t d = (uint8_t)(word >> 4);
	uint8_t s = (uint8_t)(ham_parity(d) ^ (word & 0xFu));
	*data = d;
	if (s == 0) {
		return 0;
	}
	if ((s & (s - 1u)) == 0) {
		return 1;	// a parity bit: the data is right as it is
	}
	for (uint32_t i = 0; i < 8u; i++) {
		if (ham_rows[i] == s) {
			*data = (uint8_t)(d ^ (0x80u >> i));
			return 1;
		}
	}
	return 2;
}

void dpmr_scramble(uint8_t *buf, uint32_t at, uint32_t nbits)
{
	uint32_t reg = 0x1FFu;	// S1 (bit 0) .. S9 (bit 8), all ones
	for (uint32_t i = 0; i < nbits; i++) {
		uint32_t out = (reg >> 8) & 1u;
		uint32_t fb = ((reg >> 8) ^ (reg >> 4)) & 1u;
		reg = ((reg << 1) | fb) & 0x1FFu;
		if (out) {
			dpmr_bit_put(buf, at + i, !dpmr_bit(buf, at + i));
		}
	}
}

void dpmr_interleave(const uint8_t *in, uint8_t *out, uint32_t cols)
{
	uint32_t n = 12u * cols;
	for (uint32_t k = 0; k < n; k++) {
		dpmr_bit_put(out, k, dpmr_bit(in, (k % cols) * 12u + k / cols));
	}
}

void dpmr_deinterleave(const uint8_t *in, uint8_t *out, uint32_t cols)
{
	uint32_t n = 12u * cols;
	for (uint32_t k = 0; k < n; k++) {
		dpmr_bit_put(out, (k % cols) * 12u + k / cols, dpmr_bit(in, k));
	}
}

#define CODED_MAX	(12u * DPMR_HI_BYTES / 8u)

void dpmr_block_encode(const uint8_t *data, uint32_t n, uint8_t *out)
{
	uint8_t words[CODED_MAX];
	memset(words, 0, sizeof words);
	for (uint32_t i = 0; i < n; i++) {
		dpmr_put(words, 12u * i, dpmr_ham_encode(data[i]), 12u);
	}
	memset(out, 0, 12u * n / 8u);
	dpmr_interleave(words, out, n);
	dpmr_scramble(out, 0, 12u * n);
}

void dpmr_block_decode(const uint8_t *coded, uint32_t n, uint8_t *data, dpmr_block_result_t *res)
{
	uint8_t s[CODED_MAX], words[CODED_MAX];
	memcpy(s, coded, 12u * n / 8u);
	dpmr_scramble(s, 0, 12u * n);
	memset(words, 0, sizeof words);
	dpmr_deinterleave(s, words, n);
	memset(res, 0, sizeof *res);
	for (uint32_t i = 0; i < n; i++) {
		int e = dpmr_ham_decode((uint16_t)dpmr_get(words, 12u * i, 12u), &data[i]);
		if (e == 1) {
			res->fixed++;
		} else if (e == 2) {
			res->bad++;
		}
	}
}

void dpmr_hi_encode(uint8_t data[DPMR_HI_BYTES], uint8_t out[DPMR_HI_CODED_BITS / 8u])
{
	data[9] = dpmr_crc8(data, 72u);
	dpmr_block_encode(data, DPMR_HI_BYTES, out);
}

void dpmr_hi_decode(const uint8_t coded[DPMR_HI_CODED_BITS / 8u], uint8_t data[DPMR_HI_BYTES], dpmr_block_result_t *res)
{
	dpmr_block_decode(coded, DPMR_HI_BYTES, data, res);
	res->crc_ok = dpmr_crc8(data, 72u) == data[9];
}

void dpmr_cch_encode(uint8_t data[DPMR_CCH_BYTES], uint8_t out[DPMR_CCH_CODED_BITS / 8u])
{
	dpmr_put(data, 41u, dpmr_crc7(data, 41u), 7u);
	dpmr_block_encode(data, DPMR_CCH_BYTES, out);
}

void dpmr_cch_decode(const uint8_t coded[DPMR_CCH_CODED_BITS / 8u], uint8_t data[DPMR_CCH_BYTES],
		     dpmr_block_result_t *res)
{
	dpmr_block_decode(coded, DPMR_CCH_BYTES, data, res);
	res->crc_ok = dpmr_crc7(data, 41u) == dpmr_get(data, 41u, 7u);
}

void dpmr_end_encode(uint8_t data[DPMR_END_BYTES], uint8_t out[DPMR_END_CODED_BITS / 8u])
{
	dpmr_put(data, 17u, dpmr_crc7(data, 17u), 7u);
	memset(out, 0, DPMR_END_CODED_BITS / 8u);
	for (uint32_t c = 0; c < 2u; c++) {
		for (uint32_t i = 0; i < DPMR_END_BYTES; i++) {
			dpmr_put(out, 36u * c + 12u * i, dpmr_ham_encode(data[i]), 12u);
		}
	}
	dpmr_scramble(out, 0, DPMR_END_CODED_BITS);
}

void dpmr_end_decode(const uint8_t coded[DPMR_END_CODED_BITS / 8u], uint8_t data[DPMR_END_BYTES],
		     dpmr_block_result_t *res)
{
	uint8_t s[DPMR_END_CODED_BITS / 8u];
	memcpy(s, coded, sizeof s);
	dpmr_scramble(s, 0, DPMR_END_CODED_BITS);
	memset(res, 0, sizeof *res);
	for (uint32_t c = 0; c < 2u && !res->crc_ok; c++) {
		res->fixed = res->bad = 0;
		for (uint32_t i = 0; i < DPMR_END_BYTES; i++) {
			int e = dpmr_ham_decode((uint16_t)dpmr_get(s, 36u * c + 12u * i, 12u), &data[i]);
			res->fixed += e == 1;
			res->bad += e == 2;
		}
		res->crc_ok = dpmr_crc7(data, 17u) == dpmr_get(data, 17u, 7u);
	}
}
