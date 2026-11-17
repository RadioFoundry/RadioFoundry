// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors

#include "rs63.h"
#include "gf64.h"

// Parity symbols R = n - k per code. The generator is g(x) = (x+a)(x+a^2)...(x+a^R)
// (TIA-102.BAAA-A 5.9), so the code roots are a^1..a^R. It is rebuilt per call from the
// roots rather than tabulated: 3 bytes of constants instead of 39, and it cannot drift.
static const uint8_t code_n[3] = { 24, 24, 36 };
static const uint8_t code_r[3] = { 12, 8, 16 };

unsigned fec_rs63_n(fec_rs_code_t code)
{
	return code_n[code];
}

unsigned fec_rs63_k(fec_rs_code_t code)
{
	return code_n[code] - code_r[code];
}

// g[0..r], monic, g[i] is the coefficient of x^i.
static void gen_poly(uint8_t r, uint8_t *g)
{
	g[0] = 1;
	for (unsigned i = 1; i <= r; i++) {
		g[i] = 0;
	}
	for (unsigned j = 1; j <= r; j++) {
		uint8_t root = fec_gf64_alpha(j);
		for (unsigned i = j; i > 0; i--) {
			g[i] = g[i - 1] ^ fec_gf64_mul(g[i], root);
		}
		g[0] = fec_gf64_mul(g[0], root);
	}
}

void fec_rs63_encode(fec_rs_code_t code, uint8_t *cw)
{
	unsigned r = code_r[code];
	unsigned k = code_n[code] - r;
	uint8_t g[17];
	uint8_t *par = cw + k;

	gen_poly(r, g);
	for (unsigned i = 0; i < r; i++) {
		par[i] = 0;
	}
	// Polynomial division by g: par[0] is the x^(r-1) remainder term.
	for (unsigned i = 0; i < k; i++) {
		uint8_t fb = (cw[i] & 63) ^ par[0];
		for (unsigned j = 0; j + 1 < r; j++) {
			par[j] = par[j + 1] ^ fec_gf64_mul(fb, g[r - 1 - j]);
		}
		par[r - 1] = fec_gf64_mul(fb, g[0]);
	}
}

// S[j] = c(a^(j+1)), c(x) = sum cw[i] x^(n-1-i). Returns nonzero when any syndrome is nonzero.
static int syndromes(const uint8_t *cw, unsigned n, unsigned r, uint8_t *s)
{
	int bad = 0;
	for (unsigned j = 0; j < r; j++) {
		uint8_t x = fec_gf64_alpha(j + 1);
		uint8_t acc = 0;
		for (unsigned i = 0; i < n; i++) {
			acc = fec_gf64_mul(acc, x) ^ (cw[i] & 63);
		}
		s[j] = acc;
		bad |= acc;
	}
	return bad;
}

static uint8_t poly_eval(const uint8_t *p, unsigned deg, uint8_t x)
{
	uint8_t acc = 0;
	for (unsigned i = deg + 1; i > 0; i--) {
		acc = fec_gf64_mul(acc, x) ^ p[i - 1];
	}
	return acc;
}

int fec_rs63_decode(fec_rs_code_t code, uint8_t *cw, const uint8_t *erasures, uint8_t nerasures)
{
	unsigned n = code_n[code];
	unsigned r = code_r[code];
	uint8_t syn[16];
	uint8_t lam[17];
	uint8_t omega[16];
	uint8_t pos[16];
	uint8_t val[16];
	unsigned nfound = 0;
	unsigned nera = 0;

	if (nerasures > r) {
		return -1;
	}
	for (unsigned e = 0; e < nerasures; e++) {
		if (erasures[e] >= n) {
			return -1;	// checked even for a clean word: a bad index is a caller bug
		}
	}
	if (!syndromes(cw, n, r, syn)) {
		return 0;
	}

	// Erasure locator prod(1 + X_e x), X_e = a^(n-1-index). Repeated indexes count once.
	for (unsigned i = 0; i <= r; i++) {
		lam[i] = 0;
	}
	lam[0] = 1;
	for (unsigned e = 0; e < nerasures; e++) {
		unsigned idx = erasures[e];
		unsigned dup = 0;
		for (unsigned q = 0; q < e; q++) {
			dup |= erasures[q] == idx;
		}
		if (dup) {
			continue;
		}
		uint8_t x = fec_gf64_alpha(n - 1 - idx);
		for (unsigned i = ++nera; i > 0; i--) {
			lam[i] ^= fec_gf64_mul(lam[i - 1], x);
		}
	}

	int len = fec_gf64_bm(syn, r, lam, nera);
	if (len < 0 || (unsigned)len > r) {
		return -1;
	}
	// 2e + f > r makes BM return a locator whose degree or root count disagrees with len.
	unsigned deg = 0;
	for (unsigned i = 0; i <= (unsigned)len; i++) {
		if (lam[i]) {
			deg = i;
		}
	}
	if (deg != (unsigned)len) {
		return -1;
	}

	// Omega = S(x) Lambda(x) mod x^r (error evaluator).
	for (unsigned k = 0; k < r; k++) {
		uint8_t acc = 0;
		for (unsigned i = 0; i <= k && i <= deg; i++) {
			acc ^= fec_gf64_mul(lam[i], syn[k - i]);
		}
		omega[k] = acc;
	}

	// Chien search plus Forney (first root a^1, so the value is Omega(Xinv) / Lambda'(Xinv)).
	for (unsigned i = 0; i < n; i++) {
		uint8_t xinv = fec_gf64_alpha(FEC_GF64_ORDER - (n - 1 - i));
		if (poly_eval(lam, deg, xinv)) {
			continue;
		}
		if (nfound >= deg) {
			return -1;
		}
		uint8_t dl = 0;	// formal derivative: odd terms only in characteristic 2
		for (unsigned q = 1; q <= deg; q += 2) {
			dl ^= fec_gf64_mul(lam[q], fec_gf64_alpha((q - 1) * fec_gf64_log(xinv)));
		}
		if (!dl) {
			return -1;
		}
		pos[nfound] = (uint8_t)i;
		val[nfound] = fec_gf64_mul(poly_eval(omega, r - 1, xinv), fec_gf64_inv(dl));
		nfound++;
	}
	if (nfound != deg) {
		return -1;	// roots outside the shortened word or repeated: uncorrectable
	}

	for (unsigned i = 0; i < nfound; i++) {
		cw[pos[i]] ^= val[i];
	}
	if (syndromes(cw, n, r, syn)) {
		for (unsigned i = 0; i < nfound; i++) {
			cw[pos[i]] ^= val[i];
		}
		return -1;
	}
	int changed = 0;
	for (unsigned i = 0; i < nfound; i++) {
		changed += val[i] != 0;
	}
	return changed;
}
