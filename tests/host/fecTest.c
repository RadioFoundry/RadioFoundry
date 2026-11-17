// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors

// Host test for the P25 FEC library in core/fec: GF(64), RS(63,k) shortened codes, Golay,
// Hamming, BCH(63,16,23), the data trellis and the K=5 convolutional code with its Viterbi decoder. The reference matrices below are the numeric
// code definitions of TIA-102.BAAA-A (clauses 5.7-5.9, 7, 8.5); the codecs are checked
// against them and against brute-force or exhaustive references.

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "fec/gf64.h"
#include "fec/rs63.h"
#include "fec/golay.h"
#include "fec/hamming.h"
#include "fec/bch63.h"
#include "fec/trellis.h"
#include "fec/conv.h"

static int g_checks;
static int g_fail;

#define CHECK(cond) do { \
	g_checks++; \
	if (!(cond)) { \
		g_fail++; \
		if (g_fail < 40) { \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		} \
	} \
} while (0)

static uint64_t rng_state = 0x9E3779B97F4A7C15ULL;

static uint64_t rnd(void)
{
	uint64_t x = rng_state;
	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	rng_state = x;
	return x * 0x2545F4914F6CDD1DULL;
}

static unsigned rndn(unsigned n)
{
	return (unsigned)((rnd() >> 11) % n);
}

static unsigned pop64(uint64_t v)
{
	return (unsigned)__builtin_popcountll(v);
}

// Parity parts of the systematic generator matrices, TIA-102.BAAA-A 5.9 (octal in the standard, octal literals here).

static const uint8_t mat_lc[12][12] = {
	{ 062, 044, 03, 025, 014, 016, 027, 03, 053, 04, 036, 047 },
	{ 011, 012, 011, 011, 016, 064, 067, 055, 01, 076, 026, 073 },
	{ 03, 01, 05, 075, 014, 06, 020, 044, 066, 06, 070, 066 },
	{ 021, 070, 027, 045, 016, 067, 023, 064, 073, 033, 044, 021 },
	{ 030, 022, 03, 075, 015, 015, 033, 015, 051, 03, 053, 050 },
	{ 01, 041, 027, 056, 076, 064, 021, 053, 04, 025, 01, 012 },
	{ 061, 076, 021, 055, 076, 01, 063, 035, 030, 013, 064, 070 },
	{ 024, 022, 071, 056, 021, 035, 073, 042, 057, 074, 043, 076 },
	{ 072, 042, 05, 020, 043, 047, 033, 056, 01, 016, 013, 076 },
	{ 072, 014, 065, 054, 035, 025, 041, 016, 015, 040, 071, 026 },
	{ 073, 065, 036, 061, 042, 022, 017, 04, 044, 020, 025, 05 },
	{ 071, 05, 055, 03, 071, 034, 060, 011, 074, 02, 041, 050 },
};
static const uint8_t mat_es[16][8] = {
	{ 051, 045, 067, 015, 064, 067, 052, 012 },
	{ 057, 025, 063, 073, 071, 022, 040, 015 },
	{ 05, 01, 031, 04, 016, 054, 025, 076 },
	{ 073, 07, 047, 014, 041, 077, 047, 011 },
	{ 075, 015, 051, 051, 017, 067, 017, 057 },
	{ 020, 032, 014, 042, 075, 042, 070, 054 },
	{ 02, 075, 043, 05, 01, 040, 012, 064 },
	{ 024, 074, 015, 072, 024, 026, 074, 061 },
	{ 042, 064, 07, 022, 061, 020, 040, 065 },
	{ 032, 032, 055, 041, 057, 066, 021, 077 },
	{ 065, 036, 025, 07, 050, 016, 040, 051 },
	{ 064, 06, 054, 032, 076, 046, 014, 036 },
	{ 062, 063, 074, 070, 05, 027, 037, 046 },
	{ 055, 043, 034, 071, 057, 076, 050, 064 },
	{ 024, 023, 023, 05, 050, 070, 042, 023 },
	{ 067, 075, 045, 060, 057, 024, 06, 026 },
};
static const uint8_t mat_hdr[20][16] = {
	{ 074, 037, 034, 06, 02, 07, 044, 064, 026, 014, 026, 044, 054, 013, 077, 05 },
	{ 04, 017, 050, 024, 011, 05, 030, 057, 033, 03, 02, 02, 015, 016, 025, 026 },
	{ 07, 023, 037, 046, 056, 075, 043, 045, 055, 021, 050, 031, 045, 027, 071, 062 },
	{ 026, 05, 07, 063, 063, 027, 063, 040, 06, 04, 040, 045, 047, 030, 075, 07 },
	{ 023, 073, 073, 041, 072, 034, 021, 051, 067, 016, 031, 074, 011, 021, 012, 021 },
	{ 024, 051, 025, 023, 022, 041, 074, 066, 074, 065, 070, 036, 067, 045, 064, 01 },
	{ 052, 033, 014, 02, 020, 06, 014, 025, 052, 023, 035, 074, 075, 075, 043, 027 },
	{ 055, 062, 056, 025, 073, 060, 015, 030, 013, 017, 020, 02, 070, 055, 014, 047 },
	{ 054, 051, 032, 065, 077, 012, 054, 013, 035, 032, 056, 012, 075, 01, 072, 063 },
	{ 074, 041, 030, 041, 043, 022, 051, 06, 064, 033, 03, 047, 027, 012, 055, 047 },
	{ 054, 070, 011, 03, 013, 022, 016, 057, 03, 045, 072, 031, 030, 056, 035, 022 },
	{ 051, 07, 072, 030, 065, 054, 06, 021, 036, 063, 050, 061, 064, 052, 01, 060 },
	{ 01, 065, 032, 070, 013, 044, 073, 024, 012, 052, 021, 055, 012, 035, 014, 072 },
	{ 011, 070, 05, 010, 065, 024, 015, 077, 022, 024, 024, 074, 07, 044, 07, 046 },
	{ 06, 02, 065, 011, 041, 020, 045, 042, 046, 054, 035, 012, 040, 064, 065, 033 },
	{ 034, 031, 01, 015, 044, 064, 016, 024, 052, 016, 06, 062, 020, 013, 055, 057 },
	{ 063, 043, 025, 044, 077, 063, 017, 017, 064, 014, 040, 074, 031, 072, 054, 06 },
	{ 071, 021, 070, 044, 056, 04, 030, 074, 04, 023, 071, 070, 063, 045, 056, 043 },
	{ 02, 01, 053, 074, 02, 014, 052, 074, 012, 057, 024, 063, 015, 042, 052, 033 },
	{ 034, 035, 02, 023, 021, 027, 022, 033, 064, 042, 05, 073, 051, 046, 073, 060 },
};

// BCH(63,16,23) parity part of each row (TIA-102.BAAA-A 8.5.2), overall parity bit removed.
static const uint64_t mat_bch[16] = {
	0x66C985EE9D95ULL,
	0x55AD4719D35FULL,
	0x4C1F2662743AULL,
	0x260F93313A1DULL,
	0x75CE4C76009BULL,
	0x5C2EA3D59DD8ULL,
	0x2E1751EACEECULL,
	0x170BA8F56776ULL,
	0x0B85D47AB3BBULL,
	0x630B6FD3C448ULL,
	0x3185B7E9E224ULL,
	0x18C2DBF4F112ULL,
	0x0C616DFA7889ULL,
	0x60F93313A1D1ULL,
	0x56B51C674D7DULL,
	0x4D930BDD3B2BULL,
};

// alpha^e for e = 0..63 (index 63 repeats alpha^0), octal table of 5.9.
static const uint8_t std_exp[64] = { 1, 2, 4, 8, 16, 32, 3, 6, 12, 24, 48, 35, 5, 10, 20, 40, 19, 38, 15, 30, 60, 59, 53, 41, 17, 34, 7, 14, 28, 56, 51, 37, 9, 18, 36, 11, 22, 44, 27, 54, 47, 29, 58, 55, 45, 25, 50, 39, 13, 26, 52, 43, 21, 42, 23, 46, 31, 62, 63, 61, 57, 49, 33, 1 };

// Generator polynomials as listed in 5.9, coefficient of x^0 first, octal.
static const uint8_t gpoly_lc[13] = { 050, 041, 002, 074, 011, 060, 034, 071, 003, 055, 005, 071, 001 };
static const uint8_t gpoly_es[9] = { 026, 006, 024, 057, 060, 045, 075, 067, 001 };
static const uint8_t gpoly_hdr[17] = {
	060, 073, 046, 051, 073, 005, 042, 064, 033, 022, 027, 021, 023, 002, 035, 034, 001,
};

// Table 5-3: parity halves of the (24,12,8) rows and the (18,6,8) rows (data, parity).
static const uint16_t mat_golay24[12] = {
	06165, 03073, 07550, 03664, 01732, 06631, 03315, 01547, 06706, 05227, 04476, 04353,
};
static const uint16_t mat_golay18[6][2] = {
	{ 040, 03315 }, { 020, 01547 }, { 010, 06706 }, { 004, 05227 }, { 002, 04476 }, { 001, 04353 },
};

// Table 5-4 rows as printed (identity part then parity part).
static const char *const mat_ham15[11] = {
	"100000000001111", "010000000001110", "001000000001101", "000100000001100",
	"000010000001011", "000001000001010", "000000100001001", "000000010000111",
	"000000001000110", "000000000100101", "000000000010011",
};
static const char *const mat_ham10[6] = {
	"1000001110", "0100001101", "0010001011", "0001000111", "0000100011", "0000011100",
};

static unsigned bitstr(const char *s)
{
	unsigned v = 0;
	while (*s) {
		v = (v << 1) | (unsigned)(*s++ - '0');
	}
	return v;
}

static void test_gf(void)
{
	for (unsigned e = 0; e < 63; e++) {
		CHECK(fec_gf64_alpha(e) == std_exp[e]);
		CHECK(fec_gf64_log(std_exp[e]) == e);
	}
	CHECK(std_exp[63] == 1);
	CHECK(fec_gf64_alpha(63 + 5) == fec_gf64_alpha(5));
	for (unsigned a = 1; a < 64; a++) {
		CHECK(fec_gf64_mul((uint8_t)a, fec_gf64_inv((uint8_t)a)) == 1);
		CHECK(fec_gf64_mul((uint8_t)a, 0) == 0);
		CHECK(fec_gf64_mul((uint8_t)a, 1) == a);
		for (unsigned b = 1; b < 64; b++) {
			CHECK(fec_gf64_mul((uint8_t)a, (uint8_t)b) == fec_gf64_mul((uint8_t)b, (uint8_t)a));
			// x^6 = x + 1 reduction checked independently by shift-and-add
			unsigned p = 0;
			for (unsigned i = 0; i < 6; i++) {
				if (b & (1u << i)) {
					p ^= a << i;
				}
			}
			for (int i = 10; i >= 6; i--) {
				if (p & (1u << i)) {
					p ^= 0x43u << (i - 6);
				}
			}
			CHECK(fec_gf64_mul((uint8_t)a, (uint8_t)b) == p);
		}
	}
	CHECK(fec_gf64_inv(0) == 0);
	printf("gf64: exp/log vs standard table, mul vs shift-and-add, inverses\n");
}

// ---------------------------------------------------------------- Reed-Solomon

static int rs_is_codeword(fec_rs_code_t code, const uint8_t *cw)
{
	uint8_t t[FEC_RS_MAX_N];
	unsigned n = fec_rs63_n(code);
	memcpy(t, cw, n);
	fec_rs63_encode(code, t);
	return memcmp(t, cw, n) == 0;
}

// c(x) mod g(x) == 0 with the standard's printed generator polynomial.
static int rs_divisible(const uint8_t *cw, unsigned n, const uint8_t *g, unsigned r)
{
	uint8_t w[FEC_RS_MAX_N];
	memcpy(w, cw, n);
	for (unsigned i = 0; i + r < n; i++) {
		uint8_t q = w[i];
		for (unsigned j = 0; j <= r; j++) {
			w[i + j] ^= fec_gf64_mul(q, g[r - j]);
		}
	}
	for (unsigned i = n - r; i < n; i++) {
		if (w[i]) {
			return 0;
		}
	}
	return 1;
}

static void rs_matrix_check(fec_rs_code_t code, const uint8_t *mat, unsigned k, unsigned r, const uint8_t *g)
{
	unsigned n = k + r;
	uint8_t cw[FEC_RS_MAX_N];

	CHECK(fec_rs63_n(code) == n && fec_rs63_k(code) == k);
	// every generator-matrix row: unit data symbol gives exactly the printed parity
	for (unsigned i = 0; i < k; i++) {
		memset(cw, 0, sizeof cw);
		cw[i] = 1;
		fec_rs63_encode(code, cw);
		for (unsigned j = 0; j < k; j++) {
			CHECK(cw[j] == (j == i));
		}
		for (unsigned j = 0; j < r; j++) {
			CHECK(cw[k + j] == mat[i * r + j]);
		}
	}
	// linear combination of all rows for random data
	for (unsigned it = 0; it < 300; it++) {
		uint8_t exp[FEC_RS_MAX_N] = { 0 };
		for (unsigned i = 0; i < k; i++) {
			cw[i] = (uint8_t)rndn(64);
			exp[i] = cw[i];
		}
		for (unsigned i = 0; i < k; i++) {
			for (unsigned j = 0; j < r; j++) {
				exp[k + j] ^= fec_gf64_mul(cw[i], mat[i * r + j]);
			}
		}
		fec_rs63_encode(code, cw);
		CHECK(memcmp(cw, exp, n) == 0);
		CHECK(rs_divisible(cw, n, g, r));
	}
}

static void rs_flat(uint8_t *dst, const uint8_t *rows, unsigned count, unsigned width)
{
	memcpy(dst, rows, count * width);
}

// Pick cnt distinct indexes below n.
static void pick(uint8_t *idx, unsigned cnt, unsigned n)
{
	uint8_t pool[FEC_RS_MAX_N];
	for (unsigned i = 0; i < n; i++) {
		pool[i] = (uint8_t)i;
	}
	for (unsigned i = 0; i < cnt; i++) {
		unsigned j = i + rndn(n - i);
		uint8_t t = pool[i];
		pool[i] = pool[j];
		pool[j] = t;
		idx[i] = pool[i];
	}
}

static unsigned g_rs_ok, g_rs_beyond_fail, g_rs_beyond_mis;

static void rs_decode_trials(fec_rs_code_t code, unsigned trials, unsigned e, unsigned f, bool beyond)
{
	unsigned n = fec_rs63_n(code);
	unsigned k = fec_rs63_k(code);
	uint8_t orig[FEC_RS_MAX_N], rx[FEC_RS_MAX_N], keep[FEC_RS_MAX_N];
	uint8_t idx[FEC_RS_MAX_N];

	for (unsigned it = 0; it < trials; it++) {
		for (unsigned i = 0; i < k; i++) {
			orig[i] = (uint8_t)rndn(64);
		}
		fec_rs63_encode(code, orig);
		memcpy(rx, orig, n);
		pick(idx, e + f, n);
		for (unsigned i = 0; i < e; i++) {
			rx[idx[i]] ^= (uint8_t)(1 + rndn(63));
		}
		for (unsigned i = e; i < e + f; i++) {
			rx[idx[i]] = (uint8_t)rndn(64);	// may happen to be right: an erasure costs nothing then
		}
		memcpy(keep, rx, n);
		int got = fec_rs63_decode(code, rx, f ? idx + e : NULL, (uint8_t)f);
		if (!beyond) {
			CHECK(got >= 0);
			CHECK(memcmp(rx, orig, n) == 0);
			unsigned wrong = 0;
			for (unsigned i = 0; i < n; i++) {
				wrong += keep[i] != orig[i];
			}
			CHECK(got == (int)wrong);
			g_rs_ok++;
		} else if (got < 0) {
			CHECK(memcmp(rx, keep, n) == 0);
			g_rs_beyond_fail++;
		} else {
			// miscorrection to another code word is allowed, anything else is not
			CHECK(rs_is_codeword(code, rx));
			g_rs_beyond_mis++;
		}
	}
}

static void test_rs(void)
{
	uint8_t m[20 * 16];
	static const struct {
		fec_rs_code_t code;
		const char *name;
	} codes[3] = { { FEC_RS_24_12_13, "RS(24,12,13)" }, { FEC_RS_24_16_9, "RS(24,16,9)" },
		       { FEC_RS_36_20_17, "RS(36,20,17)" } };

	rs_flat(m, &mat_lc[0][0], 12, 12);
	rs_matrix_check(FEC_RS_24_12_13, m, 12, 12, gpoly_lc);
	rs_flat(m, &mat_es[0][0], 16, 8);
	rs_matrix_check(FEC_RS_24_16_9, m, 16, 8, gpoly_es);
	rs_flat(m, &mat_hdr[0][0], 20, 16);
	rs_matrix_check(FEC_RS_36_20_17, m, 20, 16, gpoly_hdr);

	for (unsigned c = 0; c < 3; c++) {
		fec_rs_code_t code = codes[c].code;
		unsigned n = fec_rs63_n(code);
		unsigned r = n - fec_rs63_k(code);
		uint8_t cw[FEC_RS_MAX_N];
		unsigned before_ok = g_rs_ok, before_f = g_rs_beyond_fail, before_m = g_rs_beyond_mis;

		// clean word and every single-symbol error, every value
		for (unsigned i = 0; i < fec_rs63_k(code); i++) {
			cw[i] = (uint8_t)rndn(64);
		}
		fec_rs63_encode(code, cw);
		uint8_t orig[FEC_RS_MAX_N];
		memcpy(orig, cw, n);
		CHECK(fec_rs63_decode(code, cw, NULL, 0) == 0);
		for (unsigned pos = 0; pos < n; pos++) {
			for (unsigned v = 1; v < 64; v++) {
				memcpy(cw, orig, n);
				cw[pos] ^= (uint8_t)v;
				CHECK(fec_rs63_decode(code, cw, NULL, 0) == 1);
				CHECK(memcmp(cw, orig, n) == 0);
			}
		}
		// errors-and-erasures: all (e, f) with 2e + f <= r
		for (unsigned e = 0; 2 * e <= r; e++) {
			for (unsigned f = 0; 2 * e + f <= r; f++) {
				rs_decode_trials(code, 120, e, f, false);
			}
		}
		// beyond capability: 2e + f from r+1 up
		for (unsigned e = 0; e <= r; e++) {
			for (unsigned f = 0; e + f <= n && f <= r; f++) {
				if (2 * e + f > r && 2 * e + f <= r + 6) {
					rs_decode_trials(code, 60, e, f, true);
				}
			}
		}
		// erasure argument validation
		memcpy(cw, orig, n);
		uint8_t bad = (uint8_t)n;
		CHECK(fec_rs63_decode(code, cw, &bad, 1) == -1);
		uint8_t many[FEC_RS_MAX_N + 2] = { 0 };
		CHECK(fec_rs63_decode(code, cw, many, (uint8_t)(r + 1)) == -1);
		// repeated erasure index is tolerated
		uint8_t dup[2] = { 3, 3 };
		cw[3] ^= 5;
		CHECK(fec_rs63_decode(code, cw, dup, 2) == 1);
		CHECK(memcmp(cw, orig, n) == 0);
		// all parity symbols erased
		for (unsigned i = 0; i < r; i++) {
			many[i] = (uint8_t)(n - r + i);
			cw[n - r + i] = (uint8_t)rndn(64);
		}
		CHECK(fec_rs63_decode(code, cw, many, (uint8_t)r) >= 0);
		CHECK(memcmp(cw, orig, n) == 0);
		printf("%s: matrix rows + random combos vs 5.9, g(x) divisibility, 63 values at every position,\n"
		       "  %u errors/erasures decodes ok, beyond capability: %u rejected, %u miscorrected to a code word\n",
		       codes[c].name, g_rs_ok - before_ok, g_rs_beyond_fail - before_f, g_rs_beyond_mis - before_m);
	}
}

// ---------------------------------------------------------------- Golay

static uint32_t golay_ref24(unsigned d)
{
	uint32_t p = 0;
	for (unsigned j = 0; j < 12; j++) {
		if (d & (0x800u >> j)) {
			p ^= mat_golay24[j];
		}
	}
	return ((uint32_t)d << 12) | p;
}

// remainder of a 23-bit word mod g(x) = 6165 octal
static unsigned golay_rem(uint32_t v)
{
	for (int i = 22; i >= 11; i--) {
		if (v & (1u << i)) {
			v ^= 06165u << (i - 11);
		}
	}
	return v;
}

static void test_golay(void)
{
	unsigned fixed = 0;

	// encoders against the printed matrices, all 4096 data words
	for (unsigned d = 0; d < 4096; d++) {
		CHECK(fec_golay24_encode((uint16_t)d) == golay_ref24(d));
		CHECK(fec_golay23_encode((uint16_t)d) == golay_ref24(d) >> 1);
		CHECK(golay_rem(fec_golay23_encode((uint16_t)d)) == 0);
		CHECK((pop64(fec_golay24_encode((uint16_t)d)) & 1) == 0);
	}
	for (unsigned d = 0; d < 64; d++) {
		uint32_t p = 0;
		for (unsigned j = 0; j < 6; j++) {
			if (d & (0x20u >> j)) {
				p ^= mat_golay18[j][1];
			}
		}
		CHECK(fec_golay18_encode((uint8_t)d) == (((uint32_t)d << 12) | p));
		CHECK(fec_golay18_encode((uint8_t)d) == (fec_golay24_encode((uint16_t)d) & 0x3FFFF));
	}
	for (unsigned j = 0; j < 6; j++) {
		CHECK(fec_golay18_encode((uint8_t)mat_golay18[j][0]) == (((uint32_t)mat_golay18[j][0] << 12) | mat_golay18[j][1]));
	}
	for (unsigned j = 0; j < 12; j++) {
		CHECK(fec_golay24_encode((uint16_t)(0x800 >> j)) == ((0x800u >> j) << 12 | mat_golay24[j]));
	}

	// (23,12,7): perfect code, every pattern of weight <= 3 for 24 data words
	for (unsigned it = 0; it < 24; it++) {
		uint16_t d = it < 2 ? (uint16_t)(it * 0xFFF) : (uint16_t)rndn(4096);
		uint32_t cw = fec_golay23_encode(d);
		for (uint32_t a = 0; a < 24; a++) {
			for (uint32_t b = a; b < 24; b++) {
				for (uint32_t c = b; c < 24; c++) {
					// a == 23 means "unused"; equal indexes cancel so cover weights 0..3
					uint32_t e = (a < 23 ? 1u << a : 0) ^ (b < 23 ? 1u << b : 0) ^ (c < 23 ? 1u << c : 0);
					if ((a == b && a < 23) || (b == c && b < 23)) {
						continue;
					}
					uint16_t out = 0xFFFF;
					int n = fec_golay23_decode(cw ^ e, &out);
					CHECK(out == d);
					CHECK(n == (int)pop64(e));
					fixed++;
				}
			}
		}
	}
	// 4 errors: never fails, lands on a valid word (within 3 of the received word)
	for (unsigned it = 0; it < 20000; it++) {
		uint16_t d = (uint16_t)rndn(4096);
		uint32_t e = 0;
		unsigned w = 4 + rndn(10);
		while (pop64(e) < w) {
			e |= 1u << rndn(23);
		}
		uint16_t out;
		int n = fec_golay23_decode(fec_golay23_encode(d) ^ e, &out);
		CHECK(n >= 0 && n <= 3);
		CHECK(pop64(fec_golay23_encode(out) ^ fec_golay23_encode(d) ^ e) == (unsigned)n);
	}

	// (24,12,8): <= 3 corrected, all weight-4 patterns detected, heavier ones never crash
	unsigned w4 = 0, miss5 = 0;
	static uint32_t masks24[13000];
	unsigned nm24 = 0;
	for (uint32_t m = 0; m < (1u << 24); m++) {
		if (pop64(m) <= 4) {
			masks24[nm24++] = m;
		}
	}
	CHECK(nm24 == 1 + 24 + 276 + 2024 + 10626);
	for (unsigned it = 0; it < 3; it++) {
		uint16_t d = it ? (uint16_t)rndn(4096) : 0;
		uint32_t cw = fec_golay24_encode(d);
		for (unsigned i = 0; i < nm24; i++) {
			unsigned w = pop64(masks24[i]);
			uint16_t out = 0xFFFF;
			int n = fec_golay24_decode(cw ^ masks24[i], &out);
			if (w <= 3) {
				CHECK(n == (int)w && out == d);
				fixed++;
			} else {
				CHECK(n == -1 && out == 0xFFFF);
				w4++;
			}
		}
	}
	for (unsigned it = 0; it < 20000; it++) {
		uint16_t d = (uint16_t)rndn(4096);
		uint32_t m = 0;
		unsigned w = 5 + rndn(12);
		while (pop64(m) < w) {
			m |= 1u << rndn(24);
		}
		uint16_t out = 0xFFFF;
		int n = fec_golay24_decode(fec_golay24_encode(d) ^ m, &out);
		if (n >= 0) {
			CHECK(n <= 3);
			CHECK(pop64(fec_golay24_encode(out) ^ fec_golay24_encode(d) ^ m) == (unsigned)n);
			miss5++;
		}
	}

	// (18,6,8): <= 3 errors corrected (all patterns), 4-5 rejected or a valid word
	unsigned rej18 = 0, mis18 = 0;
	for (unsigned d = 0; d < 64; d++) {
		uint32_t cw = fec_golay18_encode((uint8_t)d);
		for (uint32_t m = 0; m < (1u << 18); m++) {
			unsigned w = pop64(m);
			if (w > 5) {
				continue;
			}
			uint8_t out = 0xFF;
			int n = fec_golay18_decode(cw ^ m, &out);
			if (w <= 3) {
				CHECK(n == (int)w && out == d);
				fixed++;
			} else if (n < 0) {
				CHECK(out == 0xFF);
				rej18++;
			} else {
				CHECK(n <= 3);
				CHECK(pop64(fec_golay18_encode(out) ^ cw ^ m) == (unsigned)n);
				mis18++;
			}
		}
	}
	printf("golay: 4096 words vs Table 5-3 (23/24/18 bit), g(x)=6165 divisibility; %u exhaustive/patterned corrections,\n"
	       "  24: %u weight-4 detected, %u weight 5+ miscorrected of 20000; 18: %u rejected, %u miscorrected (weight 4-5)\n",
	       fixed, w4, miss5, rej18, mis18);
}

// ---------------------------------------------------------------- Hamming

static void test_hamming(void)
{
	unsigned det10 = 0, tot10 = 0, mis15 = 0;

	for (unsigned d = 0; d < 2048; d++) {
		unsigned par = 0;
		for (unsigned j = 0; j < 11; j++) {
			if (d & (0x400u >> j)) {
				par ^= bitstr(mat_ham15[j]) & 0xF;
			}
		}
		uint16_t cw = fec_hamming15_encode((uint16_t)d);
		CHECK(cw == ((d << 4) | par));
		uint16_t t = cw;
		CHECK(fec_hamming15_correct(&t) && t == cw);
		for (unsigned b = 0; b < 15; b++) {
			t = (uint16_t)(cw ^ (1u << b));
			CHECK(fec_hamming15_correct(&t) && t == cw);
		}
		for (unsigned b = 0; b < 15; b++) {
			for (unsigned c = b + 1; c < 15; c++) {
				t = (uint16_t)(cw ^ (1u << b) ^ (1u << c));
				CHECK(fec_hamming15_correct(&t));	// perfect: always "corrects", to a wrong word
				CHECK(t != cw && fec_hamming15_encode(t >> 4) == t);
				mis15++;
			}
		}
	}
	for (unsigned j = 0; j < 11; j++) {
		CHECK(fec_hamming15_encode((uint16_t)(0x400 >> j)) == bitstr(mat_ham15[j]));
	}
	for (unsigned d = 0; d < 64; d++) {
		unsigned par = 0;
		for (unsigned j = 0; j < 6; j++) {
			if (d & (0x20u >> j)) {
				par ^= bitstr(mat_ham10[j]) & 0xF;
			}
		}
		uint16_t cw = fec_hamming10_encode((uint8_t)d);
		CHECK(cw == ((d << 4) | par));
		uint16_t t = cw;
		CHECK(fec_hamming10_correct(&t) && t == cw);
		for (unsigned b = 0; b < 10; b++) {
			t = (uint16_t)(cw ^ (1u << b));
			CHECK(fec_hamming10_correct(&t) && t == cw);
		}
		for (unsigned b = 0; b < 10; b++) {
			for (unsigned c = b + 1; c < 10; c++) {
				uint16_t bad = (uint16_t)(cw ^ (1u << b) ^ (1u << c));
				t = bad;
				bool ok = fec_hamming10_correct(&t);
				tot10++;
				if (!ok) {
					CHECK(t == bad);
					det10++;
				} else {
					CHECK(t != cw && fec_hamming10_encode(t >> 4) == t);
				}
			}
		}
	}
	for (unsigned j = 0; j < 6; j++) {
		CHECK(fec_hamming10_encode((uint8_t)(0x20 >> j)) == bitstr(mat_ham10[j]));
	}
	printf("hamming: all 2048 / 64 data words vs Table 5-4, all single errors corrected;\n"
	       "  (15,11) %u double errors miscorrected (perfect code), (10,6) %u of %u double errors detected\n",
	       mis15, det10, tot10);
}

// ---------------------------------------------------------------- BCH

static uint64_t bch_cw[65536];

// Brute-force bounded-distance decoder: the nearest code word, if within 11.
static int bch_ref(uint64_t r, uint16_t *data)
{
	unsigned best = 99;
	uint32_t at = 0;

	for (uint32_t d = 0; d < 65536; d++) {
		unsigned w = pop64(r ^ bch_cw[d]);
		if (w < best) {
			best = w;
			at = d;
		}
	}
	if (best > 11) {
		return -1;
	}
	*data = (uint16_t)at;
	return (int)best;
}

static uint64_t flip_bits(uint64_t cw, unsigned w)
{
	uint64_t e = 0;
	while (pop64(e) < w) {
		e |= 1ULL << rndn(63);
	}
	return cw ^ e;
}

static void test_bch(void)
{
	unsigned within = 0, beyond_rej = 0, beyond_mis = 0;

	for (uint32_t d = 0; d < 65536; d++) {
		bch_cw[d] = fec_bch63_encode((uint16_t)d);
	}
	// every row of the printed generator matrix, then linearity over all data words
	for (unsigned i = 0; i < 16; i++) {
		CHECK(fec_bch63_encode((uint16_t)(0x8000 >> i)) == ((1ULL << (62 - i)) | mat_bch[i]));
	}
	for (uint32_t d = 0; d < 65536; d++) {
		uint64_t par = 0;
		for (unsigned i = 0; i < 16; i++) {
			if (d & (0x8000u >> i)) {
				par ^= mat_bch[i];
			}
		}
		CHECK(bch_cw[d] == (((uint64_t)d << 47) | par));
		CHECK((bch_cw[d] >> 63) == 0);
	}
	// minimum distance of the code is 23: check weight of the sparse combinations
	unsigned minw = 99;
	for (uint32_t d = 1; d < 65536; d++) {
		unsigned w = pop64(bch_cw[d]);
		if (w < minw) {
			minw = w;
		}
	}
	CHECK(minw == 23);

	// all 0, 1, 2 error patterns, exhaustive on three data words
	for (unsigned it = 0; it < 3; it++) {
		uint16_t d = (uint16_t)rndn(65536);
		uint64_t cw = bch_cw[d];
		uint16_t out = 0;
		CHECK(fec_bch63_decode(cw, &out) == 0 && out == d);
		for (unsigned a = 0; a < 63; a++) {
			out = 0;
			CHECK(fec_bch63_decode(cw ^ (1ULL << a), &out) == 1 && out == d);
			for (unsigned b = a + 1; b < 63; b++) {
				out = 0;
				CHECK(fec_bch63_decode(cw ^ (1ULL << a) ^ (1ULL << b), &out) == 2 && out == d);
			}
		}
	}
	// weight 0..11 against the brute-force reference
	for (unsigned it = 0; it < 24000; it++) {
		uint16_t d = (uint16_t)rndn(65536);
		unsigned w = it % 12;
		uint64_t rx = flip_bits(bch_cw[d], w);
		uint16_t out = 0, ref = 0;
		int n = fec_bch63_decode(rx, &out);
		int rn = bch_ref(rx, &ref);
		CHECK(n == (int)w && out == d);
		CHECK(rn == n && ref == out);
		within++;
	}
	// weight 12 and more: decoder must agree with the reference (reject, or the same far word)
	for (unsigned it = 0; it < 3000; it++) {
		uint16_t d = (uint16_t)rndn(65536);
		unsigned w = 12 + it % 20;
		uint64_t rx = flip_bits(bch_cw[d], w);
		uint16_t out = 0xFFFF, ref = 0;
		int n = fec_bch63_decode(rx, &out);
		int rn = bch_ref(rx, &ref);
		CHECK(n == rn);
		if (n < 0) {
			CHECK(out == 0xFFFF);
			beyond_rej++;
		} else {
			CHECK(out == ref && n >= 0 && n <= 11);
			CHECK(w >= 12 && out != d);	// a wrong-word miscorrection, never a wrong count
			beyond_mis++;
		}
	}
	for (unsigned it = 0; it < 400; it++) {
		uint64_t rx = rnd() & 0x7FFFFFFFFFFFFFFFULL;
		uint16_t out = 0xFFFF, ref = 0;
		CHECK(fec_bch63_decode(rx, &out) == bch_ref(rx, &ref));
	}
	// bit 63 (the standard's extra parity bit position) is ignored
	uint16_t out = 0;
	CHECK(fec_bch63_decode(bch_cw[0x1234] | (1ULL << 63), &out) == 0 && out == 0x1234);
	printf("bch63: 16 rows + all 65536 words vs 8.5.2 matrix, min distance 23, exhaustive 0-2 errors,\n"
	       "  %u patterns of weight 0-11 equal to brute force, weight 12-31: %u rejected, %u miscorrected (== brute force)\n",
	       within, beyond_rej, beyond_mis);
}

// ---------------------------------------------------------------- trellis

// Independent copy of Tables 7-2, 7-3 (amplitudes) and 9-1 for the reference encoder.
static const uint8_t t_half[4][4] = { { 0, 15, 12, 3 }, { 4, 11, 8, 7 }, { 13, 2, 1, 14 }, { 9, 6, 5, 10 } };
static const uint8_t t_34[8][8] = {
	{ 0, 8, 4, 12, 2, 10, 6, 14 }, { 4, 12, 2, 10, 6, 14, 0, 8 }, { 1, 9, 5, 13, 3, 11, 7, 15 },
	{ 5, 13, 3, 11, 7, 15, 1, 9 }, { 3, 11, 7, 15, 1, 9, 5, 13 }, { 7, 15, 1, 9, 5, 13, 3, 11 },
	{ 2, 10, 6, 14, 0, 8, 4, 12 }, { 6, 14, 0, 8, 4, 12, 2, 10 },
};
static const int8_t amp[16][2] = {
	{ 1, -1 }, { -1, -1 }, { 3, -3 }, { -3, -3 }, { -3, -1 }, { 3, -1 }, { -1, -3 }, { 1, -3 },
	{ -3, 3 }, { 3, 3 }, { -1, 1 }, { 1, 1 }, { 1, 3 }, { -1, 3 }, { 3, 1 }, { -3, 1 },
};

static unsigned amp_to_dibit(int a)
{
	switch (a) {
	case 3:
		return 1;
	case 1:
		return 0;
	case -1:
		return 2;
	default:
		return 3;
	}
}

// The interleave rows legible in Table 7-4 (output index, input index).
static const uint8_t il_rows[][2] = {
	{ 0, 0 }, { 1, 1 }, { 2, 8 }, { 3, 9 }, { 4, 16 }, { 5, 17 }, { 18, 72 }, { 19, 73 }, { 20, 80 },
	{ 21, 81 }, { 22, 88 }, { 23, 89 }, { 24, 96 }, { 25, 97 }, { 26, 2 }, { 27, 3 }, { 28, 10 },
	{ 29, 11 }, { 30, 18 }, { 31, 19 }, { 44, 74 }, { 45, 75 }, { 46, 82 }, { 47, 83 }, { 48, 90 },
	{ 49, 91 }, { 50, 4 }, { 51, 5 }, { 52, 12 }, { 53, 13 }, { 54, 20 }, { 55, 21 }, { 68, 76 },
	{ 69, 77 }, { 70, 84 }, { 71, 85 }, { 72, 92 }, { 73, 93 }, { 74, 6 }, { 75, 7 }, { 76, 14 },
	{ 77, 15 }, { 78, 22 }, { 79, 23 }, { 92, 78 }, { 93, 79 }, { 94, 86 }, { 95, 87 }, { 96, 94 },
	{ 97, 95 },
};

static void trellis_ref(bool half, const uint8_t *in, uint8_t *out)
{
	uint8_t raw[98];
	unsigned state = 0;
	for (unsigned t = 0; t < 49; t++) {
		unsigned sym = t < 48 ? in[t] : 0;
		unsigned pt = half ? t_half[state][sym] : t_34[state][sym];
		raw[2 * t] = (uint8_t)amp_to_dibit(amp[pt][0]);
		raw[2 * t + 1] = (uint8_t)amp_to_dibit(amp[pt][1]);
		state = sym;
	}
	for (unsigned j = 0; j < 98; j++) {
		unsigned src;
		if (j < 26) {
			src = 8 * (j / 2) + (j & 1);
		} else {
			src = 2 * ((j - 26) / 24 + 1) + 8 * (((j - 26) % 24) / 2) + (j & 1);
		}
		out[j] = raw[src];
	}
}

static void test_trellis(void)
{
	uint8_t in[48], out[98], ref[98], back[48], rx[98];

	// the interleaver permutation matches the legible Table 7-4 rows and is a bijection
	uint8_t probe[98], il[98], seen[98] = { 0 };
	for (unsigned i = 0; i < 98; i++) {
		probe[i] = (uint8_t)i;
	}
	fec_trellis_interleave(probe, il);
	for (unsigned i = 0; i < sizeof il_rows / sizeof il_rows[0]; i++) {
		CHECK(il[il_rows[i][0]] == il_rows[i][1]);
	}
	for (unsigned i = 0; i < 98; i++) {
		CHECK(il[i] < 98 && !seen[il[i]]);
		seen[il[i] % 98] = 1;
	}
	uint8_t un[98];
	fec_trellis_deinterleave(il, un);
	CHECK(memcmp(un, probe, 98) == 0);

	for (unsigned half = 0; half < 2; half++) {
		fec_trellis_rate_t rate = half ? FEC_TRELLIS_HALF : FEC_TRELLIS_3_4;
		unsigned span = half ? 4 : 8;
		unsigned clean = 0, spaced = 0, burst_ok = 0, burst_bad = 0;

		for (unsigned it = 0; it < 500; it++) {
			for (unsigned i = 0; i < 48; i++) {
				in[i] = (uint8_t)rndn(span);
			}
			fec_trellis_encode(rate, in, out);
			trellis_ref(half, in, ref);
			CHECK(memcmp(out, ref, 98) == 0);
			for (unsigned i = 0; i < 98; i++) {
				CHECK(out[i] < 4);
			}
			memset(back, 0xEE, sizeof back);
			CHECK(fec_trellis_decode(rate, out, back) == 0);
			CHECK(memcmp(back, in, 48) == 0);
			clean++;

			// isolated dibit errors, far apart in transmission order after deinterleaving
			memcpy(rx, out, 98);
			unsigned errs = 1 + it % 2;
			unsigned flipped = 0;
			// pick positions whose pre-interleave indexes are >= 24 apart
			unsigned base = rndn(98);
			for (unsigned q = 0; q < errs; q++) {
				unsigned pre = (base + 30 * q) % 98;
				unsigned j = 0;
				while (il[j] != pre) {
					j++;
				}
				unsigned x = 1u << rndn(2);
				rx[j] ^= (uint8_t)x;
				flipped += (x & 1) + (x >> 1);
			}
			memset(back, 0xEE, sizeof back);
			int m = fec_trellis_decode(rate, rx, back);
			CHECK(memcmp(back, in, 48) == 0);
			CHECK(m >= 0 && m <= (int)flipped);
			spaced++;

			// random burst of 4 consecutive transmitted dibits: must not crash; count outcomes
			memcpy(rx, out, 98);
			unsigned s = rndn(94);
			for (unsigned q = 0; q < 4; q++) {
				rx[s + q] = (uint8_t)rndn(4);
			}
			int mb = fec_trellis_decode(rate, rx, back);
			CHECK(mb >= 0);
			if (memcmp(back, in, 48) == 0) {
				burst_ok++;
			} else {
				burst_bad++;
			}
		}
		// every single-bit error; whole-dibit (2-bit) errors too for the stronger rate 1/2 code
		// (the 8-state rate 3/4 code only promises single-bit isolation)
		for (unsigned i = 0; i < 48; i++) {
			in[i] = (uint8_t)rndn(span);
		}
		fec_trellis_encode(rate, in, out);
		for (unsigned j = 0; j < 98; j++) {
			for (unsigned x = 1; x < (half ? 4u : 3u); x++) {
				memcpy(rx, out, 98);
				rx[j] ^= (uint8_t)x;
				memset(back, 0xEE, sizeof back);
				CHECK(fec_trellis_decode(rate, rx, back) >= 1);
				CHECK(memcmp(back, in, 48) == 0);
			}
		}
		// pure garbage input
		for (unsigned it = 0; it < 200; it++) {
			for (unsigned i = 0; i < 98; i++) {
				rx[i] = (uint8_t)rndn(4);
			}
			CHECK(fec_trellis_decode(rate, rx, back) >= 0);
			for (unsigned i = 0; i < 48; i++) {
				CHECK(back[i] < span);
			}
		}
		printf("trellis %s: %u clean round trips == independent model, %u 1-2 spaced single-bit errors fixed,\n"
		       "  all single-bit errors fixed, 4-dibit burst: %u recovered, %u not (no crash)\n",
		       half ? "1/2" : "3/4", clean, spaced, burst_ok, burst_bad);
	}
}

// ── K=5 rate 1/2 convolutional code (conv.h), with NXDN's polynomials (TS 1-A 4.5.1: G1 = 1 + D3 + D4,
// G2 = 1 + D + D2 + D4) ──
static uint32_t conv_rng = 12345;
static uint32_t conv_rand(void)
{
	conv_rng ^= conv_rng << 13;
	conv_rng ^= conv_rng >> 17;
	conv_rng ^= conv_rng << 5;
	return conv_rng;
}

static uint8_t cbit(const uint8_t *p, unsigned i) { return (uint8_t)((p[i >> 3] >> (7 - (i & 7))) & 1); }

static void test_conv(void)
{
	static const fec_conv_code_t code = { 0x19, 0x17 };
	uint8_t in[32], enc[64], out[32];
	int8_t soft[512];
	uint16_t dec[256];
	unsigned bad_hard = 0, bad_soft = 0, bits = 0;

	// The encoder against the polynomials written out, and a clean decode, blocks with four tail bits
	for (unsigned rep = 0; rep < 200; rep++) {
		unsigned n = 36 + rep % 61;		// 36 (SACCH) .. 96 (FACCH1) input bits
		uint8_t u[256] = { 0 };

		memset(in, 0, sizeof in);
		for (unsigned i = 0; i + 4 < n; i++) {
			u[i + 4] = (uint8_t)(conv_rand() & 1);
			in[i >> 3] |= (uint8_t)(u[i + 4] << (7 - (i & 7)));
		}
		fec_conv_encode(&code, in, (uint16_t)n, enc);
		for (unsigned i = 0; i < n; i++) {
			// u[i + 4] is input bit i; u[i + 4 - k] is D^k
			uint8_t g1 = u[i + 4] ^ u[i + 1] ^ u[i], g2 = u[i + 4] ^ u[i + 3] ^ u[i + 2] ^ u[i];

			CHECK(cbit(enc, 2 * i) == g1);
			CHECK(cbit(enc, 2 * i + 1) == g2);
		}
		fec_conv_soft_from_bits(enc, (uint16_t)(2 * n), soft);
		memset(out, 0, sizeof out);
		CHECK(fec_conv_decode(&code, soft, (uint16_t)n, dec, out) == 0);
		CHECK(memcmp(out, in, (n + 7) / 8 - 1) == 0);
		// Three hard errors anywhere: corrected (the code's free distance is 7)
		for (unsigned k = 0; k < 3; k++) {
			unsigned at = conv_rand() % (2 * n);

			if (soft[at] == (int8_t)(cbit(enc, at) ? 1 : -1))
				soft[at] = (int8_t)-soft[at];
			else
				k--;
		}
		memset(out, 0, sizeof out);
		CHECK(fec_conv_decode(&code, soft, (uint16_t)n, dec, out) == 3);
		for (unsigned i = 0; i < n; i++)
			CHECK(cbit(out, i) == cbit(in, i));
	}
	// Erasures (soft 0, as a puncture leaves): a third of the G2 bits gone and one error, still corrected
	for (unsigned rep = 0; rep < 200; rep++) {
		unsigned n = 40;

		for (unsigned i = 0; i < 5; i++)
			in[i] = (uint8_t)conv_rand();
		in[4] &= 0xF0;
		fec_conv_encode(&code, in, (uint16_t)n, enc);
		for (unsigned i = 0; i < 2 * n; i++)
			soft[i] = (int8_t)(cbit(enc, i) ? 40 : -40);
		for (unsigned i = 0; i < n; i += 3)
			soft[2 * i + 1] = 0;
		soft[conv_rand() % (2 * n) & ~1u] *= -1;
		memset(out, 0, sizeof out);
		CHECK(fec_conv_decode(&code, soft, (uint16_t)n, dec, out) == 1);
		CHECK(memcmp(out, in, 5) == 0);
	}
	// Long blocks at full scale: the metrics renormalize and stay right
	for (unsigned rep = 0; rep < 4; rep++) {
		unsigned n = 256;

		for (unsigned i = 0; i < 32; i++)
			in[i] = (uint8_t)conv_rand();
		in[31] &= 0xF0;	// (the tail: bits 252-255)
		fec_conv_encode(&code, in, (uint16_t)n, enc);
		for (unsigned i = 0; i < 2 * n; i++)
			soft[i] = (int8_t)(cbit(enc, i) ? 127 : -127);
		soft[100] = (int8_t)-soft[100];
		soft[401] = (int8_t)-soft[401];
		memset(out, 0, sizeof out);
		CHECK(fec_conv_decode(&code, soft, (uint16_t)n, dec, out) == 2);
		CHECK(memcmp(out, in, 32) == 0);
	}
	// Soft decisions beat hard ones on Gaussian noise (Eb/N0 about 3 dB: sigma 0.7 of the level, +-64 soft)
	for (unsigned rep = 0; rep < 300; rep++) {
		unsigned n = 96;
		int8_t hard[192];
		uint8_t o2[12];

		for (unsigned i = 0; i < 12; i++)
			in[i] = (uint8_t)conv_rand();
		in[11] &= 0xF0;
		fec_conv_encode(&code, in, (uint16_t)n, enc);
		for (unsigned i = 0; i < 2 * n; i++) {
			int v = 0;

			for (unsigned k = 0; k < 12; k++)
				v += (int)(conv_rand() & 0xFF);
			v = (v - 1530) * 448 / 2560;	// about N(0, 0.7 x 64): 12 uniforms of SD 74
			v += cbit(enc, i) ? 64 : -64;
			soft[i] = (int8_t)(v > 127 ? 127 : v < -127 ? -127 : v);
			hard[i] = (int8_t)(soft[i] > 0 ? 1 : -1);
		}
		fec_conv_decode(&code, soft, (uint16_t)n, dec, out);
		fec_conv_decode(&code, hard, (uint16_t)n, dec, o2);
		for (unsigned i = 0; i < n - 4; i++) {
			bad_soft += cbit(out, i) != cbit(in, i);
			bad_hard += cbit(o2, i) != cbit(in, i);
			bits++;
		}
	}
	printf("  conv K=5: noise sigma 0.7, bit errors soft %u, hard %u of %u\n", bad_soft, bad_hard, bits);
	CHECK(bad_soft * 2 < bad_hard);
}

int main(void)
{
	test_gf();
	test_rs();
	test_golay();
	test_hamming();
	test_bch();
	test_trellis();
	test_conv();
	printf("fecTest: %d checks, %d failed\n", g_checks, g_fail);
	return g_fail ? 1 : 0;
}
