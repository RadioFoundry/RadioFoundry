// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors

#include "gf64.h"

// alpha^i for i = 0..62, reduced by x^6+x+1 (TIA-102.BAAA-A 5.9 gives the same table in octal).
static const uint8_t exp_tab[63] = {
	0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x03, 0x06, 0x0c, 0x18, 0x30, 0x23,
	0x05, 0x0a, 0x14, 0x28, 0x13, 0x26, 0x0f, 0x1e, 0x3c, 0x3b, 0x35, 0x29,
	0x11, 0x22, 0x07, 0x0e, 0x1c, 0x38, 0x33, 0x25, 0x09, 0x12, 0x24, 0x0b,
	0x16, 0x2c, 0x1b, 0x36, 0x2f, 0x1d, 0x3a, 0x37, 0x2d, 0x19, 0x32, 0x27,
	0x0d, 0x1a, 0x34, 0x2b, 0x15, 0x2a, 0x17, 0x2e, 0x1f, 0x3e, 0x3f, 0x3d,
	0x39, 0x31, 0x21,
};

static const uint8_t log_tab[64] = {
	0x00, 0x00, 0x01, 0x06, 0x02, 0x0c, 0x07, 0x1a, 0x03, 0x20, 0x0d, 0x23, 0x08, 0x30, 0x1b, 0x12,
	0x04, 0x18, 0x21, 0x10, 0x0e, 0x34, 0x24, 0x36, 0x09, 0x2d, 0x31, 0x26, 0x1c, 0x29, 0x13, 0x38,
	0x05, 0x3e, 0x19, 0x0b, 0x22, 0x1f, 0x11, 0x2f, 0x0f, 0x17, 0x35, 0x33, 0x25, 0x2c, 0x37, 0x28,
	0x0a, 0x3d, 0x2e, 0x1e, 0x32, 0x16, 0x27, 0x2b, 0x1d, 0x3c, 0x2a, 0x15, 0x14, 0x3b, 0x39, 0x3a,
};

uint8_t fec_gf64_alpha(unsigned e)
{
	return exp_tab[e % FEC_GF64_ORDER];
}

unsigned fec_gf64_log(uint8_t a)
{
	return log_tab[a & 63];
}

uint8_t fec_gf64_mul(uint8_t a, uint8_t b)
{
	a &= 63;
	b &= 63;
	if (!a || !b) {
		return 0;
	}
	unsigned s = log_tab[a] + log_tab[b];
	return exp_tab[s >= FEC_GF64_ORDER ? s - FEC_GF64_ORDER : s];
}

uint8_t fec_gf64_inv(uint8_t a)
{
	a &= 63;
	if (!a) {
		return 0;
	}
	return exp_tab[(FEC_GF64_ORDER - log_tab[a]) % FEC_GF64_ORDER];
}

// One code serves RS (erasure-seeded) and BCH (nerase = 0). With erasures the loop starts at
// step nerase and the length update uses r + nerase (Blahut's errors-and-erasures form), so
// the erasure positions never cost a syndrome of correction power twice.
int fec_gf64_bm(const uint8_t *syn, uint8_t nsyn, uint8_t *lambda, uint8_t nerase)
{
	uint8_t b[24];	// previous locator; nsyn <= 23 for every code here
	uint8_t t[24];
	uint8_t last = 1;	// discrepancy at the last length change
	unsigned shift = 1;
	unsigned len = nerase;

	if (nsyn > 23) {
		return -1;
	}
	for (unsigned i = 0; i <= nsyn; i++) {
		b[i] = lambda[i];
	}
	for (unsigned r = nerase; r < nsyn; r++) {
		uint8_t d = 0;
		for (unsigned i = 0; i <= len && i <= r; i++) {
			d ^= fec_gf64_mul(lambda[i], syn[r - i]);
		}
		if (!d) {
			shift++;
			continue;
		}
		uint8_t scale = fec_gf64_mul(d, fec_gf64_inv(last));
		int grow = 2 * len <= r + nerase;
		if (grow) {
			for (unsigned i = 0; i <= nsyn; i++) {
				t[i] = lambda[i];
			}
		}
		for (unsigned i = 0; i + shift <= nsyn; i++) {
			lambda[i + shift] ^= fec_gf64_mul(scale, b[i]);
		}
		if (grow) {
			for (unsigned i = 0; i <= nsyn; i++) {
				b[i] = t[i];
			}
			len = r + 1 + nerase - len;
			last = d;
			shift = 1;
		} else {
			shift++;
		}
	}
	return (int)len;
}
