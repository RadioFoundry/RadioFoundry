// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// SHA-256 from FIPS 180-4 sections 4.1.2, 4.2.2, 5.1.1, 5.3.3 and 6.2
#include <string.h>
#include "support/sha256.h"

static const uint32_t K[64] = {
	0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
	0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
	0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
	0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
	0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
	0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
	0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
	0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static inline uint32_t ror(uint32_t x, unsigned n)
{
	return (x >> n) | (x << (32u - n));
}

// The message schedule rolls through 16 words (6.2.2 computes the same Wt), so a small task stack can run it
static void compress(uint32_t h[8], const uint8_t *p)
{
	uint32_t w[16];
	for (unsigned t = 0; t < 16; t++, p += 4) {
		w[t] = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
	}
	uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
	for (unsigned t = 0; t < 64; t++) {
		if (t >= 16) {
			uint32_t w15 = w[(t - 15) & 15], w2 = w[(t - 2) & 15];
			uint32_t s0 = ror(w15, 7) ^ ror(w15, 18) ^ (w15 >> 3);
			uint32_t s1 = ror(w2, 17) ^ ror(w2, 19) ^ (w2 >> 10);
			w[t & 15] += s0 + w[(t - 7) & 15] + s1;
		}
		uint32_t t1 = k + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + K[t] + w[t & 15];
		uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
		k = g;
		g = f;
		f = e;
		e = d + t1;
		d = c;
		c = b;
		b = a;
		a = t1 + t2;
	}
	h[0] += a;
	h[1] += b;
	h[2] += c;
	h[3] += d;
	h[4] += e;
	h[5] += f;
	h[6] += g;
	h[7] += k;
}

void sha256_init(sha256_t *s)
{
	static const uint32_t h0[8] = {
		0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
	};
	memcpy(s->h, h0, sizeof h0);
	s->bytes = 0;
	s->used = 0;
}

void sha256_update(sha256_t *s, const void *data, size_t len)
{
	const uint8_t *p = data;
	s->bytes += len;
	if (s->used != 0) {
		size_t n = 64u - s->used < len ? 64u - s->used : len;
		memcpy(s->block + s->used, p, n);
		s->used = (uint8_t)(s->used + n);
		p += n;
		len -= n;
		if (s->used < 64u) {
			return;
		}
		compress(s->h, s->block);
		s->used = 0;
	}
	for (; len >= 64u; p += 64, len -= 64u) {
		compress(s->h, p);
	}
	memcpy(s->block, p, len);
	s->used = (uint8_t)len;
}

void sha256_final(sha256_t *s, uint8_t out[SHA256_LEN])
{
	uint64_t bits = s->bytes * 8u;
	uint8_t pad = 0x80;
	sha256_update(s, &pad, 1);
	pad = 0;
	while (s->used != 56u) {
		sha256_update(s, &pad, 1);
	}
	uint8_t len[8];
	for (unsigned i = 0; i < 8; i++) {
		len[i] = (uint8_t)(bits >> (56u - 8u * i));
	}
	sha256_update(s, len, sizeof len);
	for (unsigned i = 0; i < 8; i++) {
		out[4 * i] = (uint8_t)(s->h[i] >> 24);
		out[4 * i + 1] = (uint8_t)(s->h[i] >> 16);
		out[4 * i + 2] = (uint8_t)(s->h[i] >> 8);
		out[4 * i + 3] = (uint8_t)s->h[i];
	}
}
