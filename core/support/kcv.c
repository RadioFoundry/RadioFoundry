// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Key check values, docs/spec/key-check.md
#include <string.h>
#include "config.h"
#include "support/kcv.h"
#include "support/sha256.h"
#include "support/hkdf.h"
#if CONFIG_DMR_AES
#include "dmr/dmrAes.h"
#endif

// The label every hashed check value starts with (key-check.md 2): a check value of this scheme never equals one of
// another use of SHA-256 over the same octets
static const uint8_t LABEL[6] = { 'R', 'F', 'K', 'C', 'V', '1' };

// The traffic key algorithms that are AES, by system type (key-check.md 3). A mode whose keys are AES adds its row
// when it claims its ids (channel-modes.md 4); every other key gets the hashed form
static const struct { uint8_t sys_type, alg; } AES_ALGS[] = {
#if CONFIG_DMR_AES
	{ 5, DMR_AES_ALG_AES256 },	// conventional DMR AES-256
#endif
	{ 0, 0 },			// none: no system type is 0
};

bool kcv_hashed(kcv_class_t cls, uint8_t a, uint8_t b, const uint8_t *key, uint16_t n, uint8_t out[KCV_LEN])
{
	uint8_t head[5] = { (uint8_t)cls, a, b, (uint8_t)(n >> 8), (uint8_t)n };
	uint8_t h[SHA256_LEN];
	sha256_t s;
	sha256_init(&s);
	sha256_update(&s, LABEL, sizeof LABEL);
	sha256_update(&s, head, sizeof head);
	sha256_update(&s, key, n);
	sha256_final(&s, h);
	memcpy(out, h, KCV_LEN);
	crypto_wipe(h, sizeof h);
	crypto_wipe(&s, sizeof s);
	return true;
}

bool kcv_aes(const uint8_t *key, uint16_t n, uint8_t out[KCV_LEN])
{
#if CONFIG_DMR_AES
	if (n != DMR_AES_KEY_BYTES)
		return false;
	static const uint8_t zero[16];
	uint8_t block[16];
	aes256_ctx_t ctx;	// 240 octets of round keys: the caller's stack, for a moment
	aes256Init(&ctx, key);
	aes256Encrypt(&ctx, zero, block);
	memcpy(out, block, KCV_LEN);
	crypto_wipe(&ctx, sizeof ctx);
	crypto_wipe(block, sizeof block);
	return true;
#else
	(void)key;
	(void)n;
	(void)out;
	return false;
#endif
}

bool kcv_privacy(uint8_t type, uint32_t key, uint8_t out[KCV_LEN])
{
	uint8_t v[4] = { (uint8_t)(key >> 24), (uint8_t)(key >> 16), (uint8_t)(key >> 8), (uint8_t)key };
	bool ok = kcv_hashed(KCV_CLASS_PRIVACY, type, 0, v, sizeof v, out);
	crypto_wipe(v, sizeof v);
	return ok;
}

bool kcv_traffic(uint8_t sys_type, uint8_t alg, const uint8_t *key, uint16_t n, uint8_t out[KCV_LEN])
{
	for (unsigned i = 0; i < sizeof AES_ALGS / sizeof AES_ALGS[0]; i++)
		if (AES_ALGS[i].sys_type != 0 && AES_ALGS[i].sys_type == sys_type && AES_ALGS[i].alg == alg)
			return kcv_aes(key, n, out);
	return kcv_hashed(KCV_CLASS_TRAFFIC, sys_type, alg, key, n, out);
}

void kcv_text(const uint8_t kcv[KCV_LEN], char out[KCV_TEXT])
{
	static const char hex[] = "0123456789ABCDEF";
	for (unsigned i = 0; i < KCV_LEN; i++) {
		out[2 * i] = hex[kcv[i] >> 4];
		out[2 * i + 1] = hex[kcv[i] & 15];
	}
	out[2 * KCV_LEN] = '\0';
}
