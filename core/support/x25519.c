// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// X25519 from RFC 7748 sections 5 (the ladder, clamping, masking) and 6.1 (the all-zero check).
//
// Field elements mod p = 2^255 - 19 are 16 limbs of 16 bits, unsigned. A product goes through 64-bit column sums and
// folds the limbs at 2^256 and up back in times 38 (2^256 = 38 mod p). After a product every limb but the lowest is at
// most 0xffff and the lowest at most 0xffff + 38 (fe_carry); sums of two such are below 2^18, so the 16 column terms
// of a product stay far below 2^64. A difference adds 4p first, so with the subtrahend so bounded no limb goes
// negative and nothing needs a signed shift.
#include <string.h>
#include "support/x25519.h"
#include "support/hkdf.h"

// 4p, limb by limb: subtracting a reduced element from x + 4p leaves every limb positive
static const uint32_t P4[16] = {
	0x3ffb4u, 0x3fffcu, 0x3fffcu, 0x3fffcu, 0x3fffcu, 0x3fffcu, 0x3fffcu, 0x3fffcu,
	0x3fffcu, 0x3fffcu, 0x3fffcu, 0x3fffcu, 0x3fffcu, 0x3fffcu, 0x3fffcu, 0x1fffcu,
};
// (486662 - 2) / 4 = 121665, the ladder's constant a24 (5)
static const x25519_fe A24 = { 0xdb41u, 0x0001u };

static void fe_add(x25519_fe o, const x25519_fe a, const x25519_fe b)
{
	for (unsigned i = 0; i < 16; i++)
		o[i] = a[i] + b[i];
}

static void fe_sub(x25519_fe o, const x25519_fe a, const x25519_fe b)
{
	for (unsigned i = 0; i < 16; i++)
		o[i] = a[i] + P4[i] - b[i];
}

// Two passes bring every limb to 16 bits, the lowest to at most 0xffff + 38 (the top limb's carry is at most 1 on the
// second pass)
static void fe_carry(x25519_fe o, uint64_t t[16])
{
	for (unsigned pass = 0; pass < 2; pass++) {
		for (unsigned i = 0; i < 16; i++) {
			uint64_t c = t[i] >> 16;
			t[i] &= 0xffffu;
			if (i < 15)
				t[i + 1] += c;
			else
				t[0] += 38u * c;
		}
	}
	for (unsigned i = 0; i < 16; i++)
		o[i] = (uint32_t)t[i];
}

// noinline: one 248-byte column array on the stack at a time, whoever calls
__attribute__((noinline)) static void fe_mul(x25519_fe o, const x25519_fe a, const x25519_fe b)
{
	uint64_t t[31];
	memset(t, 0, sizeof t);
	for (unsigned i = 0; i < 16; i++)
		for (unsigned j = 0; j < 16; j++)
			t[i + j] += (uint64_t)a[i] * b[j];
	for (unsigned i = 0; i < 15; i++)
		t[i] += 38u * t[i + 16];
	fe_carry(o, t);
}

static void fe_cswap(x25519_fe a, x25519_fe b, uint32_t swap)
{
	uint32_t mask = 0u - swap;
	for (unsigned i = 0; i < 16; i++) {
		uint32_t x = mask & (a[i] ^ b[i]);
		a[i] ^= x;
		b[i] ^= x;
	}
}

static void fe_from_bytes(x25519_fe o, const uint8_t b[X25519_LEN])
{
	for (unsigned i = 0; i < 16; i++)
		o[i] = (uint32_t)b[2 * i] | (uint32_t)b[2 * i + 1] << 8;
	o[15] &= 0x7fffu;	// 5: the top bit of u is masked
}

// The unique representative below p, little endian
static void fe_to_bytes(uint8_t b[X25519_LEN], x25519_fe a)
{
	// Carried until every limb is 16 bits: then a < 2^256 < 2p + 38, and two conditional subtractions of p leave it
	// below p
	for (unsigned pass = 0; pass < 3; pass++) {
		for (unsigned i = 0; i < 16; i++) {
			uint32_t c = a[i] >> 16;
			a[i] &= 0xffffu;
			if (i < 15)
				a[i + 1] += c;
			else
				a[0] += 38u * c;
		}
	}
	for (unsigned round = 0; round < 2; round++) {
		uint32_t m[16], borrow = 0;
		for (unsigned i = 0; i < 16; i++) {
			uint32_t p = i == 0 ? 0xffedu : i == 15 ? 0x7fffu : 0xffffu;
			uint32_t d = a[i] + 0x10000u - p - borrow;
			m[i] = d & 0xffffu;
			borrow = 1u - (d >> 16);
		}
		uint32_t keep = 0u - borrow;	// a borrow out of the top: a < p, keep a
		for (unsigned i = 0; i < 16; i++)
			a[i] = (a[i] & keep) | (m[i] & ~keep);
	}
	for (unsigned i = 0; i < 16; i++) {
		b[2 * i] = (uint8_t)a[i];
		b[2 * i + 1] = (uint8_t)(a[i] >> 8);
	}
}

// o = z^(p - 2) = 1/z. p - 2 = 2^255 - 21: every bit from 254 down set but bits 4 and 2. The exponent is public
static void fe_invert(x25519_fe o, const x25519_fe z)
{
	memcpy(o, z, sizeof(x25519_fe));
	for (int i = 253; i >= 0; i--) {
		fe_mul(o, o, o);
		if (i != 4 && i != 2)
			fe_mul(o, o, z);
	}
}

bool x25519(uint8_t out[X25519_LEN], const uint8_t scalar[X25519_LEN], const uint8_t u[X25519_LEN],
	    x25519_ws_t *ws)
{
	uint8_t k[X25519_LEN];
	memcpy(k, scalar, sizeof k);
	k[0] &= 248u;		// 5, decodeScalar25519
	k[31] &= 127u;
	k[31] |= 64u;

	uint32_t *x1 = ws->v[0], *x2 = ws->v[1], *z2 = ws->v[2], *x3 = ws->v[3], *z3 = ws->v[4];
	uint32_t *a = ws->v[5], *b = ws->v[6], *c = ws->v[7], *d = ws->v[8], *e = ws->v[9];
	memset(ws, 0, sizeof *ws);
	fe_from_bytes(x1, u);
	x2[0] = 1;
	memcpy(x3, x1, sizeof(x25519_fe));
	z3[0] = 1;

	uint32_t swap = 0;
	for (int t = 254; t >= 0; t--) {
		uint32_t bit = (k[t >> 3] >> (t & 7)) & 1u;
		swap ^= bit;
		fe_cswap(x2, x3, swap);
		fe_cswap(z2, z3, swap);
		swap = bit;
		fe_add(a, x2, z2);	// A
		fe_sub(b, x2, z2);	// B
		fe_add(c, x3, z3);	// C
		fe_sub(d, x3, z3);	// D
		fe_mul(d, d, a);	// DA
		fe_mul(c, c, b);	// CB
		fe_add(x3, d, c);
		fe_mul(x3, x3, x3);	// x_3 = (DA + CB)^2
		fe_sub(z3, d, c);
		fe_mul(z3, z3, z3);
		fe_mul(z3, z3, x1);	// z_3 = x_1 * (DA - CB)^2
		fe_mul(a, a, a);	// AA
		fe_mul(b, b, b);	// BB
		fe_mul(x2, a, b);	// x_2 = AA * BB
		fe_sub(e, a, b);	// E = AA - BB
		fe_mul(z2, e, A24);
		fe_add(z2, z2, a);
		fe_mul(z2, z2, e);	// z_2 = E * (AA + a24 * E)
	}
	fe_cswap(x2, x3, swap);
	fe_cswap(z2, z3, swap);
	fe_invert(a, z2);
	fe_mul(x2, x2, a);
	fe_to_bytes(out, x2);

	uint8_t any = 0;
	for (unsigned i = 0; i < X25519_LEN; i++)
		any |= out[i];
	crypto_wipe(k, sizeof k);
	crypto_wipe(ws, sizeof *ws);
	return any != 0;
}

void x25519_public(uint8_t out[X25519_LEN], const uint8_t scalar[X25519_LEN], x25519_ws_t *ws)
{
	static const uint8_t base[X25519_LEN] = { 9 };
	(void)x25519(out, scalar, base, ws);
}
