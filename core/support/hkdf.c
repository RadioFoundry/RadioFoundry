// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// HMAC-SHA-256 from RFC 2104 section 2 and HKDF from RFC 5869 section 2
#include <string.h>
#include "support/hkdf.h"

#define BLOCK	64u

static void pad_key(sha256_t *s, const uint8_t k0[BLOCK], uint8_t pad)
{
	uint8_t b[BLOCK];
	for (unsigned i = 0; i < BLOCK; i++)
		b[i] = (uint8_t)(k0[i] ^ pad);
	sha256_init(s);
	sha256_update(s, b, BLOCK);
	crypto_wipe(b, sizeof b);
}

void hmac_sha256_init(hmac_sha256_t *h, const uint8_t *key, size_t key_len)
{
	uint8_t k0[BLOCK];
	memset(k0, 0, sizeof k0);
	if (key_len > BLOCK) {
		sha256_t s;
		sha256_init(&s);
		sha256_update(&s, key, key_len);
		sha256_final(&s, k0);
	} else if (key_len > 0) {
		memcpy(k0, key, key_len);
	}
	pad_key(&h->inner, k0, 0x36);
	pad_key(&h->outer, k0, 0x5c);
	crypto_wipe(k0, sizeof k0);
}

void hmac_sha256_update(hmac_sha256_t *h, const void *data, size_t len)
{
	if (len != 0)	// data may be NULL then (HKDF with no info)
		sha256_update(&h->inner, data, len);
}

void hmac_sha256_final(hmac_sha256_t *h, uint8_t out[SHA256_LEN])
{
	uint8_t ih[SHA256_LEN];
	sha256_final(&h->inner, ih);
	sha256_update(&h->outer, ih, sizeof ih);
	sha256_final(&h->outer, out);
	crypto_wipe(ih, sizeof ih);
	crypto_wipe(h, sizeof *h);
}

void hmac_sha256(const uint8_t *key, size_t key_len, const void *data, size_t len, uint8_t out[SHA256_LEN])
{
	hmac_sha256_t h;
	hmac_sha256_init(&h, key, key_len);
	hmac_sha256_update(&h, data, len);
	hmac_sha256_final(&h, out);
}

void hkdf_extract(const uint8_t *salt, size_t salt_len, const uint8_t *ikm, size_t ikm_len, uint8_t prk[SHA256_LEN])
{
	static const uint8_t zeros[SHA256_LEN];
	if (salt == NULL || salt_len == 0) {
		salt = zeros;
		salt_len = sizeof zeros;
	}
	hmac_sha256(salt, salt_len, ikm, ikm_len, prk);
}

bool hkdf_expand(const uint8_t prk[SHA256_LEN], const void *info, size_t info_len, uint8_t *out, size_t out_len)
{
	if (out_len > 255u * SHA256_LEN)
		return false;
	uint8_t t[SHA256_LEN];
	size_t done = 0;
	for (uint8_t n = 1; done < out_len; n++) {
		hmac_sha256_t h;
		hmac_sha256_init(&h, prk, SHA256_LEN);
		if (n > 1)
			hmac_sha256_update(&h, t, sizeof t);	// T(n) = HMAC(PRK, T(n-1) | info | n)
		hmac_sha256_update(&h, info, info_len);
		hmac_sha256_update(&h, &n, 1);
		hmac_sha256_final(&h, t);
		size_t take = out_len - done < SHA256_LEN ? out_len - done : SHA256_LEN;
		memcpy(out + done, t, take);
		done += take;
	}
	crypto_wipe(t, sizeof t);
	return true;
}

bool crypto_equal(const uint8_t *a, const uint8_t *b, size_t n)
{
	uint8_t d = 0;
	for (size_t i = 0; i < n; i++)
		d |= (uint8_t)(a[i] ^ b[i]);
	return d == 0;
}

void crypto_wipe(void *p, size_t n)
{
	volatile uint8_t *v = p;
	while (n--)
		*v++ = 0;
}
