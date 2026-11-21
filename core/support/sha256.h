// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// SHA-256 (FIPS 180-4), small and portable: the image hash of the support info (image_hash.h)
#ifndef SHA256_H
#define SHA256_H

#include <stddef.h>
#include <stdint.h>

#define SHA256_LEN	32u

typedef struct {
	uint32_t h[8];
	uint64_t bytes;		// message bytes so far
	uint8_t block[64];
	uint8_t used;		// bytes waiting in block
} sha256_t;

void sha256_init(sha256_t *s);
void sha256_update(sha256_t *s, const void *data, size_t len);
void sha256_final(sha256_t *s, uint8_t out[SHA256_LEN]);

#endif
