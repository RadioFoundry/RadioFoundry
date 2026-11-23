// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// P25 data units (p25.h): where each field sits among the status symbols, and its code words. Clause numbers are
// TIA-102.BAAA-A's.

#include "p25.h"

#include <string.h>

#include "../air/air_syms.h"
#include "../fec/bch63.h"
#include "../fec/golay.h"
#include "../fec/hamming.h"
#include "../fec/rs63.h"

#define SS_EVERY	35	// information dibits between two status symbols (8.2: 2 bits after every 70)
#define INFO_AFTER_NID	112	// frame sync 48 + NID 64

static const uint8_t FRAME_SYNC[6] = { 0x55, 0x75, 0xF5, 0xFF, 0x77, 0xFF };	// 8.3

// An LDU's fields, as information bit offsets (figures 8-3, 8-4): voice 1, voice 2, four LC / ES words, voice 3, ...
static const uint16_t VOICE_AT[P25_VOICE_FRAMES] = { 112, 256, 440, 624, 808, 992, 1176, 1360, 1536 };
static const uint16_t WORDS_AT[6] = { 400, 584, 768, 952, 1136, 1320 };
#define LSD_AT		1504

// Information bit i as a bit index into the packed unit: a status symbol follows every 35th dibit
static uint16_t air_bit(uint16_t i)
{
	uint16_t k = i >> 1;

	return (uint16_t)(2 * (k + k / SS_EVERY) + (i & 1));
}

uint32_t p25_bits_get(const uint8_t *unit, uint16_t i, uint8_t n)
{
	uint32_t v = 0;

	while (n--)
		v = (v << 1) | air_bit_get(unit, air_bit(i++));
	return v;
}

void p25_bits_put(uint8_t *unit, uint16_t i, uint8_t n, uint32_t v)
{
	while (n--)
		air_bit_put(unit, air_bit(i++), (uint8_t)((v >> n) & 1));
}

uint16_t p25_unit_syms(uint8_t duid)
{
	switch (duid) {
	case P25_DUID_HDU: return P25_HDU_SYMS;
	case P25_DUID_TDU: return P25_TDU_SYMS;
	case P25_DUID_LDU1:
	case P25_DUID_LDU2: return P25_LDU_SYMS;
	case P25_DUID_TDULC: return P25_TDULC_SYMS;
	default: return 0;
	}
}

void p25_unit_begin(uint8_t *unit, uint16_t nsyms, uint16_t nac, uint8_t duid, uint8_t ss)
{
	uint64_t cw = fec_bch63_encode((uint16_t)(((nac & 0xFFF) << 4) | (duid & 15)));

	memset(unit, 0, (size_t)(nsyms + 3) / 4);
	memcpy(unit, FRAME_SYNC, sizeof(FRAME_SYNC));		// 24 symbols, ahead of the first status symbol
	p25_bits_put(unit, 48, 31, (uint32_t)(cw >> 32));
	p25_bits_put(unit, 79, 32, (uint32_t)cw);
	// The 64th bit, as the last column of the standard's generator matrix has it (8.5.2): set in the rows of the
	// data unit ID's two low bits only, which gives the P column of table 8-4
	p25_bits_put(unit, 111, 1, ((duid >> 1) ^ duid) & 1);
	for (uint16_t a = SS_EVERY; a < nsyms; a += SS_EVERY + 1)
		air_sym_put(unit, a, ss);
}

int p25_nid_get(const uint8_t *unit, uint16_t *nac, uint8_t *duid)
{
	uint64_t cw = ((uint64_t)p25_bits_get(unit, 48, 31) << 32) | p25_bits_get(unit, 79, 32);
	uint16_t data;
	int errs = fec_bch63_decode(cw, &data);

	if (errs < 0)
		return -1;
	*nac = data >> 4;
	*duid = data & 15;
	return errs;
}

uint8_t p25_status_get(const uint8_t *unit, uint16_t k)
{
	return air_sym_get(unit, (uint16_t)(SS_EVERY + k * (SS_EVERY + 1)));
}

// ── Hex bits: 6-bit symbols of the Reed-Solomon codes, to and from octets, first bit first ──
static void hex_from_bytes(const uint8_t *bytes, uint8_t nhex, uint8_t *hex)
{
	for (uint8_t i = 0; i < nhex; i++) {
		uint8_t v = 0;

		for (uint8_t b = 0; b < 6; b++)
			v = (uint8_t)((v << 1) | air_bit_get(bytes, (uint16_t)(6 * i + b)));
		hex[i] = v;
	}
}

static void hex_to_bytes(const uint8_t *hex, uint8_t nhex, uint8_t *bytes)
{
	for (uint8_t i = 0; i < nhex; i++)
		for (uint8_t b = 0; b < 6; b++)
			air_bit_put(bytes, (uint16_t)(6 * i + b), (uint8_t)((hex[i] >> (5 - b)) & 1));
}

// Reed-Solomon over inner code words that flagged their own failures. The flags go in as erasures while they use up
// no more than half the code's distance: past that a word of noise decodes to something (12 erasures of RS(24,12,13)
// fit any word), and the flags of a badly hit unit are not to be trusted either. Then, and when the flagged decode
// fails, the word is tried as it is
static int rs_get(fec_rs_code_t code, uint8_t *hex, const uint8_t *erasures, uint8_t n)
{
	int r = -1;

	if (n && 2 * n <= fec_rs63_n(code) - fec_rs63_k(code))
		r = fec_rs63_decode(code, hex, erasures, n);
	if (r < 0)
		r = fec_rs63_decode(code, hex, NULL, 0);
	return r;
}

// The 24 shortened Hamming words of an LDU (5.4, 5.5) into hex bits. Words whose Hamming code can't place its error
// are told to the Reed-Solomon decoder as erasures
static uint8_t ldu_words_get(const uint8_t *unit, uint8_t *hex, uint8_t *erasures)
{
	uint8_t n = 0;

	for (uint8_t w = 0; w < 24; w++) {
		uint16_t cw = (uint16_t)p25_bits_get(unit, (uint16_t)(WORDS_AT[w / 4] + 10 * (w % 4)), 10);

		if (!fec_hamming10_correct(&cw))
			erasures[n++] = w;
		hex[w] = (uint8_t)(cw >> 4);
	}
	return n;
}

static void ldu_words_put(uint8_t *unit, const uint8_t *hex)
{
	for (uint8_t w = 0; w < 24; w++)
		p25_bits_put(unit, (uint16_t)(WORDS_AT[w / 4] + 10 * (w % 4)), 10, fec_hamming10_encode(hex[w]));
}

static int ldu_get(const uint8_t *unit, fec_rs_code_t code, uint8_t nhex, uint8_t *out)
{
	uint8_t hex[24], erasures[24];
	uint8_t n = ldu_words_get(unit, hex, erasures);
	int r = rs_get(code, hex, erasures, n);

	if (r < 0)
		return -1;
	hex_to_bytes(hex, nhex, out);
	return r;
}

static void ldu_put(uint8_t *unit, fec_rs_code_t code, uint8_t nhex, const uint8_t *in)
{
	uint8_t hex[24];

	hex_from_bytes(in, nhex, hex);
	fec_rs63_encode(code, hex);
	ldu_words_put(unit, hex);
}

int p25_ldu_lc_get(const uint8_t *unit, uint8_t lc[P25_LC_BYTES])
{
	return ldu_get(unit, FEC_RS_24_12_13, 12, lc);
}

void p25_ldu_lc_put(uint8_t *unit, const uint8_t lc[P25_LC_BYTES])
{
	ldu_put(unit, FEC_RS_24_12_13, 12, lc);
}

int p25_ldu_es_get(const uint8_t *unit, uint8_t es[P25_ES_BYTES])
{
	return ldu_get(unit, FEC_RS_24_16_9, 16, es);
}

void p25_ldu_es_put(uint8_t *unit, const uint8_t es[P25_ES_BYTES])
{
	ldu_put(unit, FEC_RS_24_16_9, 16, es);
}

// Low speed data: one octet in a (16,8,5) shortened cyclic code word, g(x) = x^8 + x^5 + x^4 + x^3 + 1 (5.6)
static uint16_t lsd_encode(uint8_t d)
{
	uint16_t r = (uint16_t)(d << 8);

	for (uint8_t i = 0; i < 8; i++)
		r = (uint16_t)((r & 0x8000) ? ((uint32_t)r << 1) ^ (0x139u << 8) : (uint32_t)r << 1);
	return (uint16_t)((d << 8) | (r >> 8));
}

static uint8_t bits16(uint16_t v)
{
	uint8_t n = 0;

	for (; v; v &= (uint16_t)(v - 1))
		n++;
	return n;
}

int p25_ldu_lsd_get(const uint8_t *unit, uint8_t lsd[2])
{
	uint8_t out[2];
	int errs = 0;

	for (uint8_t i = 0; i < 2; i++) {
		uint16_t cw = (uint16_t)p25_bits_get(unit, (uint16_t)(LSD_AT + 16 * i), 16);
		uint16_t d;

		// Distance 5: two errors at most, so one of the code words that near is the only one
		for (d = 0; d < 256 && bits16((uint16_t)(lsd_encode((uint8_t)d) ^ cw)) > 2; d++)
			;
		if (d == 256)
			return -1;
		errs += bits16((uint16_t)(lsd_encode((uint8_t)d) ^ cw));
		out[i] = (uint8_t)d;
	}
	lsd[0] = out[0];
	lsd[1] = out[1];
	return errs;
}

void p25_ldu_lsd_put(uint8_t *unit, const uint8_t lsd[2])
{
	for (uint8_t i = 0; i < 2; i++)
		p25_bits_put(unit, (uint16_t)(LSD_AT + 16 * i), 16, lsd_encode(lsd[i]));
}

// The header word: 20 hex bits + 16 of RS(36,20,17), each in a (18,6,8) Golay word (5.2)
int p25_hdu_get(const uint8_t *unit, uint8_t hdr[P25_HDR_BYTES])
{
	uint8_t hex[36], erasures[36], n = 0;
	int r;

	for (uint8_t w = 0; w < 36; w++) {
		uint32_t cw = p25_bits_get(unit, (uint16_t)(INFO_AFTER_NID + 18 * w), 18);

		hex[w] = (uint8_t)(cw >> 12);
		if (fec_golay18_decode(cw, &hex[w]) < 0)
			erasures[n++] = w;
	}
	r = rs_get(FEC_RS_36_20_17, hex, erasures, n);
	if (r < 0)
		return -1;
	hex_to_bytes(hex, 20, hdr);
	return r;
}

void p25_hdu_put(uint8_t *unit, const uint8_t hdr[P25_HDR_BYTES])
{
	uint8_t hex[36];

	hex_from_bytes(hdr, 20, hex);
	fec_rs63_encode(FEC_RS_36_20_17, hex);
	for (uint8_t w = 0; w < 36; w++)
		p25_bits_put(unit, (uint16_t)(INFO_AFTER_NID + 18 * w), 18, fec_golay18_encode(hex[w]));
}

// A terminator's link control: the 24 hex bits in pairs, each pair in a (24,12,8) Golay word (5.5.1)
int p25_tdulc_get(const uint8_t *unit, uint8_t lc[P25_LC_BYTES])
{
	uint8_t hex[24], erasures[24], n = 0;
	int r;

	for (uint8_t w = 0; w < 12; w++) {
		uint32_t cw = p25_bits_get(unit, (uint16_t)(INFO_AFTER_NID + 24 * w), 24);
		uint16_t d = (uint16_t)(cw >> 12);

		if (fec_golay24_decode(cw, &d) < 0) {
			erasures[n++] = (uint8_t)(2 * w);
			erasures[n++] = (uint8_t)(2 * w + 1);
		}
		hex[2 * w] = (uint8_t)(d >> 6);
		hex[2 * w + 1] = (uint8_t)(d & 63);
	}
	r = rs_get(FEC_RS_24_12_13, hex, erasures, n);
	if (r < 0)
		return -1;
	hex_to_bytes(hex, 12, lc);
	return r;
}

void p25_tdulc_put(uint8_t *unit, const uint8_t lc[P25_LC_BYTES])
{
	uint8_t hex[24];

	hex_from_bytes(lc, 12, hex);
	fec_rs63_encode(FEC_RS_24_12_13, hex);
	for (uint8_t w = 0; w < 12; w++)
		p25_bits_put(unit, (uint16_t)(INFO_AFTER_NID + 24 * w), 24,
			     fec_golay24_encode((uint16_t)((hex[2 * w] << 6) | hex[2 * w + 1])));
}

// ── Voice (5.3) ──
// Where each bit of the 72 dibits comes from (table 5-1): code word in bits 7-5, its bit number in bits 4-0. Two
// entries per symbol: bit 1, then bit 0
#define V(c, i)		(uint8_t)(((c) << 5) | (i))
static const uint8_t VOICE_MAP[144] = {
	V(0, 22), V(1, 21), V(2, 20), V(3, 19), V(4, 10), V(5, 1), V(1, 20), V(0, 21), V(3, 18), V(2, 19), V(5, 0), V(4, 9),
	V(0, 20), V(1, 19), V(2, 18), V(3, 17), V(4, 8), V(6, 14), V(1, 18), V(0, 19), V(3, 16), V(2, 17), V(6, 13), V(4, 7),
	V(0, 18), V(1, 17), V(2, 16), V(3, 15), V(4, 6), V(6, 12), V(1, 16), V(0, 17), V(3, 14), V(2, 15), V(6, 11), V(4, 5),
	V(0, 16), V(1, 15), V(2, 14), V(3, 13), V(4, 4), V(6, 10), V(1, 14), V(0, 15), V(3, 12), V(2, 13), V(6, 9), V(4, 3),
	V(0, 14), V(1, 13), V(2, 12), V(3, 11), V(4, 2), V(6, 8), V(1, 12), V(0, 13), V(3, 10), V(2, 11), V(6, 7), V(4, 1),
	V(0, 12), V(1, 11), V(2, 10), V(3, 9), V(4, 0), V(6, 6), V(1, 10), V(0, 11), V(3, 8), V(2, 9), V(6, 5), V(5, 14),
	V(0, 10), V(1, 9), V(2, 8), V(3, 7), V(5, 13), V(6, 4), V(1, 8), V(0, 9), V(3, 6), V(2, 7), V(6, 3), V(5, 12),
	V(0, 8), V(1, 7), V(2, 6), V(3, 5), V(5, 11), V(6, 2), V(1, 6), V(0, 7), V(3, 4), V(2, 5), V(6, 1), V(5, 10),
	V(0, 6), V(1, 5), V(2, 4), V(3, 3), V(5, 9), V(6, 0), V(1, 4), V(0, 5), V(3, 2), V(2, 3), V(7, 6), V(5, 8),
	V(0, 4), V(1, 3), V(2, 2), V(3, 1), V(5, 7), V(7, 5), V(1, 2), V(0, 3), V(3, 0), V(2, 1), V(7, 4), V(5, 6),
	V(0, 2), V(1, 1), V(2, 0), V(4, 14), V(5, 5), V(7, 3), V(1, 0), V(0, 1), V(4, 13), V(3, 22), V(7, 2), V(5, 4),
	V(0, 0), V(2, 22), V(3, 21), V(4, 12), V(5, 3), V(7, 1), V(2, 21), V(1, 22), V(4, 11), V(3, 20), V(7, 0), V(5, 2),
};

// The PN masks m_1..m_6 from u_0 (5.3): 114 values of p = 173 p + 13849 mod 65536 from p0 = 16 u_0, bit 15 of each
static void voice_masks(uint16_t u0, uint32_t m[8])
{
	static const uint8_t LEN[8] = { 0, 23, 23, 23, 15, 15, 15, 0 };
	uint16_t p = (uint16_t)(16 * u0);

	for (uint8_t c = 0; c < 8; c++) {
		m[c] = 0;
		for (uint8_t i = 0; i < LEN[c]; i++) {
			p = (uint16_t)(173u * p + 13849u);
			m[c] = (m[c] << 1) | (p >> 15);
		}
	}
}

uint16_t p25_ldu_voice_sym(uint8_t n)
{
	uint16_t k;

	if (n >= P25_VOICE_FRAMES)
		return P25_LDU_SYMS;
	k = VOICE_AT[n] >> 1;
	return (uint16_t)(k + k / SS_EVERY);
}

int p25_ldu_voice_get(const uint8_t *unit, uint8_t n, uint8_t imbe[P25_IMBE_BYTES])
{
	uint32_t c[8] = { 0 }, m[8];
	uint16_t u[8], at = VOICE_AT[n], o = 0;
	int errs, e0, e4 = 0;

	for (uint8_t i = 0; i < 144; i++)
		c[VOICE_MAP[i] >> 5] |= p25_bits_get(unit, (uint16_t)(at + i), 1) << (VOICE_MAP[i] & 31);
	// u_0 first: the masks of the other words come from it
	errs = e0 = fec_golay23_decode(c[0], &u[0]);
	voice_masks(u[0], m);
	for (uint8_t k = 1; k < 4; k++)
		errs += fec_golay23_decode(c[k] ^ m[k], &u[k]);
	for (uint8_t k = 4; k < 7; k++) {
		uint16_t cw = (uint16_t)(c[k] ^ m[k]), was = cw;

		fec_hamming15_correct(&cw);
		errs += cw != was;
		if (k == 4)
			e4 = cw != was;
		u[k] = cw >> 4;
	}
	u[7] = (uint16_t)c[7];
	memset(imbe, 0, P25_IMBE_BYTES);
	for (uint8_t k = 0; k < 8; k++) {
		uint8_t len = k < 4 ? 12 : k < 7 ? 11 : 7;

		for (uint8_t b = 0; b < len; b++)
			air_bit_put(imbe, o++, (uint8_t)((u[k] >> (len - 1 - b)) & 1));
	}
	return P25_VOICE_ERRS(errs, e0, e4);
}

void p25_ldu_voice_put(uint8_t *unit, uint8_t n, const uint8_t imbe[P25_IMBE_BYTES])
{
	uint32_t c[8], m[8];
	uint16_t u[8], at = VOICE_AT[n], o = 0;

	for (uint8_t k = 0; k < 8; k++) {
		uint8_t len = k < 4 ? 12 : k < 7 ? 11 : 7;

		u[k] = 0;
		for (uint8_t b = 0; b < len; b++)
			u[k] = (uint16_t)((u[k] << 1) | air_bit_get(imbe, o++));
	}
	voice_masks(u[0], m);
	c[0] = fec_golay23_encode(u[0]);
	for (uint8_t k = 1; k < 4; k++)
		c[k] = fec_golay23_encode(u[k]) ^ m[k];
	for (uint8_t k = 4; k < 7; k++)
		c[k] = fec_hamming15_encode(u[k]) ^ m[k];
	c[7] = u[7];
	for (uint8_t i = 0; i < 144; i++)
		p25_bits_put(unit, (uint16_t)(at + i), 1, (c[VOICE_MAP[i] >> 5] >> (VOICE_MAP[i] & 31)) & 1);
}

// ── Link control words (5.5, figure 5-6) ──
bool p25_lc_parse(const uint8_t raw[P25_LC_BYTES], p25_lc_t *lc)
{
	memset(lc, 0, sizeof(*lc));
	lc->lcf = raw[0];
	lc->mfid = raw[1];
	lc->src = ((uint32_t)raw[6] << 16) | ((uint32_t)raw[7] << 8) | raw[8];
	if (lc->mfid != 0)
		return false;		// a manufacturer's own format
	switch (lc->lcf) {
	case P25_LCF_GROUP:
		lc->emergency = (raw[2] & 0x80) != 0;
		lc->tgid = (uint16_t)((raw[4] << 8) | raw[5]);
		return true;
	case P25_LCF_UNIT:
		lc->dst = ((uint32_t)raw[3] << 16) | ((uint32_t)raw[4] << 8) | raw[5];
		return true;
	default:
		return false;
	}
}

void p25_lc_build(const p25_lc_t *lc, uint8_t raw[P25_LC_BYTES])
{
	memset(raw, 0, P25_LC_BYTES);
	raw[0] = lc->lcf;
	raw[1] = lc->mfid;
	if (lc->lcf == P25_LCF_UNIT) {
		raw[3] = (uint8_t)(lc->dst >> 16);
		raw[4] = (uint8_t)(lc->dst >> 8);
		raw[5] = (uint8_t)lc->dst;
	} else {
		raw[2] = lc->emergency ? 0x80 : 0;
		raw[4] = (uint8_t)(lc->tgid >> 8);
		raw[5] = (uint8_t)lc->tgid;
	}
	raw[6] = (uint8_t)(lc->src >> 16);
	raw[7] = (uint8_t)(lc->src >> 8);
	raw[8] = (uint8_t)lc->src;
}
