// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A small seeded mutation fuzzer for the host tests' parsers of untrusted input (header only).
//
//   fuzz_t f;
//   fuzz_init(&f, 0x5EED);                     fixed seed: every run tries the same inputs
//   for (long i = 0; i < fuzz_iters(2000); i++) {
//           int n = fuzz_mutate(&f, buf, len, sizeof(buf));    a mutated copy of a valid seed input in buf
//           uint8_t *p = fuzz_exact(buf, n);                    on the heap at exactly n bytes (ASan sees overreads)
//           ... feed p, n to the parser, check its invariants ...
//           free(p);
//   }
//
// RFOUNDRY_FUZZ_ITERS=<n> in the environment replaces every default count (long runs by hand or nightly, e.g. under
// make SANITIZE=1); RFOUNDRY_FUZZ_SEED=<n> changes the seed. Build with SANITIZE=1 for the memory checks to mean much.
#ifndef RFOUNDRY_TEST_FUZZ_H
#define RFOUNDRY_TEST_FUZZ_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	uint64_t s;
} fuzz_t;

static inline uint32_t fuzz_next(fuzz_t *f)
{
	// xorshift64* (Vigna)
	f->s ^= f->s >> 12;
	f->s ^= f->s << 25;
	f->s ^= f->s >> 27;
	return (uint32_t)((f->s * 0x2545F4914F6CDD1DULL) >> 32);
}

static inline uint32_t fuzz_below(fuzz_t *f, uint32_t n)
{
	return (n == 0) ? 0 : (fuzz_next(f) % n);
}

static inline void fuzz_init(fuzz_t *f, uint64_t seed)
{
	const char *e = getenv("RFOUNDRY_FUZZ_SEED");

	if ((e != NULL) && (*e != '\0')) {
		seed ^= strtoull(e, NULL, 0);
	}
	f->s = seed ? seed : 0x9E3779B97F4A7C15ULL;
	for (int i = 0; i < 4; i++) {
		(void)fuzz_next(f);
	}
}

static inline long fuzz_iters(long dflt)
{
	const char *e = getenv("RFOUNDRY_FUZZ_ITERS");

	if ((e != NULL) && (*e != '\0')) {
		long n = strtol(e, NULL, 0);

		return (n > 0) ? n : dflt;
	}
	return dflt;
}

// Values parsers tend to treat specially: length octets, escapes, sign bits, delimiters
static const uint8_t FUZZ_MAGIC[] = { 0x00, 0x01, 0x02, 0x04, 0x05, 0x06, 0x0A, 0x0D, 0x20, 0x2E, 0x30, 0x3A, 0x7E, 0x7F,
				      0x80, 0x81, 0x82, 0x83, 0x84, 0xC0, 0xC1, 0xDB, 0xDC, 0xDD, 0xFE, 0xFF };

// One to four mutations of buf[0..len) in place (cap: buf's size); the new length
static inline int fuzz_mutate(fuzz_t *f, uint8_t *buf, int len, int cap)
{
	int rounds = 1 + (int)fuzz_below(f, 4);

	for (int r = 0; r < rounds; r++) {
		uint32_t op = fuzz_below(f, 9);
		int at = (len > 0) ? (int)fuzz_below(f, (uint32_t)len) : 0;

		switch (op) {
		case 0:	// flip a bit
			if (len > 0) {
				buf[at] ^= (uint8_t)(1u << fuzz_below(f, 8));
			}
			break;
		case 1:	// a random byte
			if (len > 0) {
				buf[at] = (uint8_t)fuzz_next(f);
			}
			break;
		case 2:	// a magic byte
		case 3:
			if (len > 0) {
				buf[at] = FUZZ_MAGIC[fuzz_below(f, sizeof(FUZZ_MAGIC))];
			}
			break;
		case 4:	// truncate
			len = at;
			break;
		case 5:	// delete a run
			if (len > 0) {
				int n = 1 + (int)fuzz_below(f, 8);

				if (at + n > len) {
					n = len - at;
				}
				memmove(buf + at, buf + at + n, (size_t)(len - at - n));
				len -= n;
			}
			break;
		case 6:	// insert bytes (magic or random)
		{
			int n = 1 + (int)fuzz_below(f, 8);

			if (len + n > cap) {
				break;
			}
			memmove(buf + at + n, buf + at, (size_t)(len - at));
			for (int i = 0; i < n; i++) {
				buf[at + i] = (fuzz_below(f, 2) == 0) ? FUZZ_MAGIC[fuzz_below(f, sizeof(FUZZ_MAGIC))]
								     : (uint8_t)fuzz_next(f);
			}
			len += n;
			break;
		}
		case 7:	// duplicate a chunk somewhere else
		{
			if (len == 0) {
				break;
			}
			int n = 1 + (int)fuzz_below(f, 16);

			if (at + n > len) {
				n = len - at;
			}
			if (len + n > cap) {
				break;
			}
			int to = (int)fuzz_below(f, (uint32_t)len + 1);
			uint8_t tmp[16];

			memcpy(tmp, buf + at, (size_t)n);
			memmove(buf + to + n, buf + to, (size_t)(len - to));
			memcpy(buf + to, tmp, (size_t)n);
			len += n;
			break;
		}
		default:	// a 16-bit field set to an edge value (lengths, ports, block numbers)
			if (len > 1) {
				static const uint16_t edges[] = { 0x0000, 0x0001, 0x007F, 0x0080, 0x00FF, 0x0100, 0x7FFF, 0x8000,
								  0xFFFE, 0xFFFF };
				uint16_t v = edges[fuzz_below(f, sizeof(edges) / sizeof(edges[0]))];

				if (at + 1 >= len) {
					at = len - 2;
				}
				buf[at] = (uint8_t)(v >> 8);
				buf[at + 1] = (uint8_t)v;
			}
			break;
		}
	}
	return len;
}

// A copy of p[0..n) on the heap, exactly n bytes long (one byte for n == 0), so reads past it are ASan reports
static inline uint8_t *fuzz_exact(const uint8_t *p, int n)
{
	uint8_t *q = malloc((n > 0) ? (size_t)n : 1u);

	if ((q != NULL) && (n > 0)) {
		memcpy(q, p, (size_t)n);
	}
	return q;
}

#endif
