// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// HMAC-SHA-256 (RFC 2104, FIPS 198-1) and HKDF-SHA-256 (RFC 5869) on support/sha256.h. Small and portable, no
// allocation. crypto_wipe and crypto_equal are for any code that handles a key
#ifndef RFOUNDRY_HKDF_H
#define RFOUNDRY_HKDF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "support/sha256.h"

typedef struct {
	sha256_t inner;
	sha256_t outer;		// already fed the outer pad: hmac_final finishes it
} hmac_sha256_t;

// Keys longer than a block are hashed first (RFC 2104 2)
void hmac_sha256_init(hmac_sha256_t *h, const uint8_t *key, size_t key_len);
void hmac_sha256_update(hmac_sha256_t *h, const void *data, size_t len);
void hmac_sha256_final(hmac_sha256_t *h, uint8_t out[SHA256_LEN]);
void hmac_sha256(const uint8_t *key, size_t key_len, const void *data, size_t len, uint8_t out[SHA256_LEN]);

// HKDF-Extract (RFC 5869 2.2): salt NULL or empty = a block of zeros, as the RFC says
void hkdf_extract(const uint8_t *salt, size_t salt_len, const uint8_t *ikm, size_t ikm_len, uint8_t prk[SHA256_LEN]);
// HKDF-Expand (2.3): out_len octets, at most 255 * 32. False (out untouched) when out_len is too long
bool hkdf_expand(const uint8_t prk[SHA256_LEN], const void *info, size_t info_len, uint8_t *out, size_t out_len);

// Constant time: true when the n octets match (a MAC check that a timing doesn't give away)
bool crypto_equal(const uint8_t *a, const uint8_t *b, size_t n);
// Zeroes n octets in a way the compiler doesn't drop as a dead store (a secret on its way out of scope)
void crypto_wipe(void *p, size_t n);

#endif
