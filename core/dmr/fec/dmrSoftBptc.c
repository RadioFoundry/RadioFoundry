// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors

#include "config.h"

#if CONFIG_DMR

#include <stdbool.h>
#include <string.h>
#include "dmr/fec/dmrSoftBptc.h"
#include "dmr/fec/dmrFec.h"
#include "dsp/fxp.h"

// The code matrix as in dmrFec.c: index 0 is R(3), then 13 rows of 15; matrix index k goes out as info bit
// (k * 181) mod 196
#define ROWS	13
#define COLS	15
#define AT(r, c)	(1 + (r) * COLS + (c))

// Each codeword bit's parity check column (rows 0x7AC8 0x3D64 0x1EB2 0x7591 of dmrFec.c, B.3.5, a codeword's first bit
// as the most significant; the column's bit k from row k): Hamming(15,11)'s are all 15 nonzero 4-bit vectors, and the
// shortened Hamming(13,9)'s (B.3.4) are its last 13, missing 9 and 11
static const uint8_t COLUMN[COLS] RFOUNDRY_CPU_DATA = { 9, 11, 15, 7, 14, 5, 10, 13, 3, 6, 12, 1, 2, 4, 8 };

// tanh(x / 16) in Q31: the reliability of an LLR of x eighths (tanh of half the LLR). The information of a sure bit
// is in how far this falls short of 1
static const int32_t TANH_HALF[128] RFOUNDRY_CPU_DATA = {
	0, 134043238, 267046038, 398000016, 525958823, 650064194, 769566653, 883839965,
	992389039, 1094851532, 1190993835, 1280702458, 1363971989, 1440890820, 1511625774, 1576406585,
	1635510996, 1689251036, 1737960815, 1781986033, 1821675246, 1857372819, 1889413451, 1918118093,
	1943791074, 1966718233, 1987165888, 2005380453, 2021588576, 2035997648, 2048796596, 2060156855,
	2070233464, 2079166216, 2087080830, 2094090114, 2100295089, 2105786059, 2110643629, 2114939645,
	2118738072, 2122095801, 2125063379, 2127685686, 2130002540, 2132049242, 2133857079, 2135453758,
	2136863812, 2138108952, 2139208386, 2140179101, 2141036119, 2141792720, 2142460640, 2143050249,
	2143570713, 2144030125, 2144435637, 2144793563, 2145109482, 2145388318, 2145634419, 2145851627,
	2146043330, 2146212522, 2146361844, 2146493629, 2146609936, 2146712581, 2146803170, 2146883117,
	2146953672, 2147015939, 2147070891, 2147119387, 2147162186, 2147199956, 2147233289, 2147262705,
	2147288666, 2147311576, 2147331794, 2147349637, 2147365383, 2147379279, 2147391543, 2147402365,
	2147411916, 2147420345, 2147427783, 2147434347, 2147440140, 2147445252, 2147449764, 2147453745,
	2147457259, 2147460360, 2147463096, 2147465511, 2147467642, 2147469523, 2147471183, 2147472647,
	2147473940, 2147475081, 2147476087, 2147476976, 2147477760, 2147478452, 2147479062, 2147479601,
	2147480077, 2147480496, 2147480867, 2147481193, 2147481482, 2147481736, 2147481961, 2147482159,
	2147482334, 2147482489, 2147482625, 2147482745, 2147482851, 2147482945, 2147483027, 2147483100,
};

// log2(1 + (m + 0.5) / 64) x 256
static const uint8_t LOG2_FRAC[64] RFOUNDRY_CPU_DATA = {
	3, 9, 14, 20, 25, 30, 36, 41, 46, 51, 56, 61, 66, 71, 75, 80,
	85, 89, 94, 98, 103, 107, 111, 116, 120, 124, 128, 132, 136, 140, 144, 148,
	152, 155, 159, 163, 167, 170, 174, 178, 181, 185, 188, 192, 195, 198, 202, 205,
	208, 212, 215, 218, 221, 224, 228, 231, 234, 237, 240, 243, 246, 249, 252, 255,
};

// log2(x) x 256, x > 0
static int32_t log2q(uint32_t x)
{
	int e = 31 - __builtin_clz(x);
	uint32_t m = e >= 6 ? (x >> (e - 6)) : (x << (6 - e));
	return e * 256 + LOG2_FRAC[m & 63u];
}

static int8_t clamp8(int v)
{
	return (int8_t)(v > 127 ? 127 : v < -127 ? -127 : v);
}

// Q31 product (the rounded high word of two Q31 values is Q30, one shift back)
#define MUL(a, b)	(fxp_mulhr((a), (b)) * 2)

// The extrinsic LLR of each of the n bits of a codeword (Hartmann-Rudolph over the dual code). The dual of either code
// is the simplex code: word u (1-15) covers the bits whose column v has <u, v> odd. With rho(v) = tanh(L / 2) of the
// bit whose column is v (positive for a 0; a column the code lacks, and v = 0, a sure 0) and P(u) the product of rho
// over word u (P(0) = 1), T-(v) sums P(u) over the words with <u, v> odd and T+(v) over the rest: one Walsh-Hadamard
// transform of P gives both for every v. The extrinsic LLR is ln((S0 + S1) / (S0 - S1)) with S0 = T+ and S1 = T- / rho,
// so ln((rho T+ + T-) / (rho T+ - T-)): no division. For a row of sure bits that disagree, S0 and S1 are sums of terms
// near +-1 that cancel to 1e-4 or less: products in Q31 (fxp_mulhr: one instruction on a core with a 32 x 32
// multiply), sums in Q27 (16 terms of at most 1)
static void siso(const int8_t *in, int8_t *ext, int n)
{
	const uint8_t *col = COLUMN + (COLS - n);
	int32_t rho[16], w[16];
	for (int v = 1; v < 16; v++) {
		rho[v] = INT32_MAX;
	}
	for (int j = 0; j < n; j++) {
		int a = in[j] < 0 ? -in[j] : in[j];
		// An LLR of 0 has rho 0, and with rho both T+ and T- of its bit vanish: take it as just above 0 (0.002)
		rho[col[j]] = a == 0 ? 1 << 22 : in[j] > 0 ? -TANH_HALF[a] : TANH_HALF[a];	// in: positive for a 1
	}
	// P(u) by groups: v = 4g + l and u = 4a + b, so <u, v> = <a, g> + <b, l>. Within group g, o<g><b> is the product
	// of rho over the l with <b, l> odd, e<g><b> over the rest (o<g>0, the empty product, is 1); P(u) takes e<g><b> from
	// the groups with <a, g> odd and o<g><b> from the others. Written out (generated): 24 products for the groups and
	// 39 for P, against 105 word by word, and no branches
	// Group 0 is always even (<a, 0> = 0): only its odd products
	const int32_t o01 = MUL(rho[1], rho[3]), o02 = MUL(rho[2], rho[3]), o03 = MUL(rho[1], rho[2]);
	const int32_t e11 = MUL(rho[4], rho[6]), o11 = MUL(rho[5], rho[7]), e12 = MUL(rho[4], rho[5]), o12 = MUL(rho[6], rho[7]);
	const int32_t e13 = MUL(rho[4], rho[7]), o13 = MUL(rho[5], rho[6]), e10 = MUL(e11, o11);
	const int32_t e21 = MUL(rho[8], rho[10]), o21 = MUL(rho[9], rho[11]), e22 = MUL(rho[8], rho[9]), o22 = MUL(rho[10], rho[11]);
	const int32_t e23 = MUL(rho[8], rho[11]), o23 = MUL(rho[9], rho[10]), e20 = MUL(e21, o21);
	const int32_t e31 = MUL(rho[12], rho[14]), o31 = MUL(rho[13], rho[15]), e32 = MUL(rho[12], rho[13]), o32 = MUL(rho[14], rho[15]);
	const int32_t e33 = MUL(rho[12], rho[15]), o33 = MUL(rho[13], rho[14]), e30 = MUL(e31, o31);
	w[0] = 1 << 27;
	w[1] = MUL(MUL(o01, o11), MUL(o21, o31)) >> 4;
	w[2] = MUL(MUL(o02, o12), MUL(o22, o32)) >> 4;
	w[3] = MUL(MUL(o03, o13), MUL(o23, o33)) >> 4;
	w[4] = MUL(e10, e30) >> 4;
	w[5] = MUL(MUL(o01, e11), MUL(o21, e31)) >> 4;
	w[6] = MUL(MUL(o02, e12), MUL(o22, e32)) >> 4;
	w[7] = MUL(MUL(o03, e13), MUL(o23, e33)) >> 4;
	w[8] = MUL(e20, e30) >> 4;
	w[9] = MUL(MUL(o01, o11), MUL(e21, e31)) >> 4;
	w[10] = MUL(MUL(o02, o12), MUL(e22, e32)) >> 4;
	w[11] = MUL(MUL(o03, o13), MUL(e23, e33)) >> 4;
	w[12] = MUL(e10, e20) >> 4;
	w[13] = MUL(MUL(o01, e11), MUL(e21, o31)) >> 4;
	w[14] = MUL(MUL(o02, e12), MUL(e22, o32)) >> 4;
	w[15] = MUL(MUL(o03, e13), MUL(e23, o33)) >> 4;
	// In place: w[v] = sum of P(u) (-1)^<u, v>, so T+(v) = (w[0] + w[v]) / 2 and T-(v) = (w[0] - w[v]) / 2. No partial sum
	// passes 16 terms of at most 2^27
#define BF(i, j)	{ int32_t x = w[i], y = w[j]; w[i] = x + y; w[j] = x - y; }
	BF(0, 1) BF(2, 3) BF(4, 5) BF(6, 7) BF(8, 9) BF(10, 11) BF(12, 13) BF(14, 15)
	BF(0, 2) BF(1, 3) BF(4, 6) BF(5, 7) BF(8, 10) BF(9, 11) BF(12, 14) BF(13, 15)
	BF(0, 4) BF(1, 5) BF(2, 6) BF(3, 7) BF(8, 12) BF(9, 13) BF(10, 14) BF(11, 15)
	BF(0, 8) BF(1, 9) BF(2, 10) BF(3, 11) BF(4, 12) BF(5, 13) BF(6, 14) BF(7, 15)
#undef BF
	int32_t half = w[0] >> 1;
	for (int j = 0; j < n; j++) {
		int v = col[j];
		int32_t r = rho[v], tp = half + (w[v] >> 1), tm = half - (w[v] >> 1);
		int32_t rt = MUL(r, tp), num = rt + tm, den = rt - tm, e0;
		if (r < 0) {
			num = -num;
			den = -den;
		}
		if (num <= 0) {
			e0 = -127;
		} else if (den <= 0) {
			e0 = 127;
		} else {
			// (log2 x 256) to eighths of a natural log: x 8 ln 2 / 256 = 177 / 8192
			e0 = ((log2q((uint32_t)num) - log2q((uint32_t)den)) * 177 + 4096) >> 13;
		}
		ext[j] = clamp8(-e0);	// back to positive for a 1
	}
}

// The decision on matrix bit k: the input and both extrinsic LLRs
static bool hard(const dmrSoftBptcWork_t *w, int k)
{
	return w->ch[k] + w->row[k] + w->col[k] > 0;
}

// The decisions to the hard decoder, which clears the few errors left, and its payload to accept
static bool tryHard(const dmrSoftBptcWork_t *w, uint8_t *payload, uint8_t *rbits, dmrSoftBptcAccept_t accept, void *ctx)
{
	uint16_t rows[ROWS];

	for (int r = 0; r < ROWS; r++) {
		uint32_t v = 0;
		for (int c = 0; c < COLS; c++) {
			v = v << 1 | hard(w, AT(r, c));
		}
		rows[r] = (uint16_t)v;
	}
	if (dmrFecBptcDecodeRows(rows, payload) < 0 || (accept != NULL && !accept(payload, ctx))) {
		return false;
	}
	// R(3) is in no codeword: its own decision; R(2)-R(0) lead row 0
	if (rbits) {
		*rbits = (uint8_t)((w->ch[0] > 0) << 3 | rows[0] >> 12);
	}
	return true;
}

int dmrSoftBptcDecode(dmrSoftBptcWork_t *w, const int8_t *llr, uint8_t *payload, uint8_t *rbits, int maxIters,
		      dmrSoftBptcAccept_t accept, void *ctx)
{
	int8_t in[COLS], out[COLS];
	for (int k = 0, i = 0; k < DMR_SOFT_BPTC_BITS; k++, i += 181) {
		if (i >= DMR_SOFT_BPTC_BITS) {
			i -= DMR_SOFT_BPTC_BITS;
		}
		w->ch[k] = clamp8(llr[i]);
	}
	memset(w->row, 0, sizeof(w->row));
	memset(w->col, 0, sizeof(w->col));
	// A clean burst needs no soft decoding: the hard decoder alone, a tenth of the cost of an iteration
	if (tryHard(w, payload, rbits, accept, ctx)) {
		return 0;
	}
	for (int it = 1; it <= maxIters; it++) {
		for (int r = 0; r < ROWS; r++) {
			for (int c = 0; c < COLS; c++) {
				in[c] = clamp8(w->ch[AT(r, c)] + w->col[AT(r, c)]);
			}
			siso(in, out, COLS);
			for (int c = 0; c < COLS; c++) {
				w->row[AT(r, c)] = out[c];
			}
		}
		for (int c = 0; c < COLS; c++) {
			for (int r = 0; r < ROWS; r++) {
				in[r] = clamp8(w->ch[AT(r, c)] + w->row[AT(r, c)]);
			}
			siso(in, out, ROWS);
			for (int r = 0; r < ROWS; r++) {
				w->col[AT(r, c)] = out[r];
			}
		}
		if (tryHard(w, payload, rbits, accept, ctx)) {
			return it;
		}
	}
	return -1;
}

#endif
