// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The key primitives (core/support): HMAC-SHA-256 on RFC 4231's cases, HKDF on RFC 5869's, and the key check values
// of docs/spec/key-check.md against values computed apart from this code (Python hashlib from the spec's text)
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "support/hkdf.h"
#include "support/kcv.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static void unhex(const char *s, uint8_t *out, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		unsigned v;
		sscanf(s + 2 * i, "%2x", &v);
		out[i] = (uint8_t)v;
	}
}

static int same(const uint8_t *got, const char *want_hex, size_t n)
{
	uint8_t want[256];
	unhex(want_hex, want, n);
	if (memcmp(got, want, n) == 0)
		return 1;
	printf("  got  ");
	for (size_t i = 0; i < n; i++)
		printf("%02x", got[i]);
	printf("\n  want %s\n", want_hex);
	return 0;
}

static void test_hmac(void)
{
	uint8_t key[131], out[32];
	// RFC 4231 4.2, 4.3
	memset(key, 0x0b, 20);
	hmac_sha256(key, 20, "Hi There", 8, out);
	CHECK(same(out, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7", 32));
	hmac_sha256((const uint8_t *)"Jefe", 4, "what do ya want for nothing?", 28, out);
	CHECK(same(out, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", 32));
	// 4.7: a key longer than the block is hashed first
	memset(key, 0xaa, 131);
	const char *msg = "Test Using Larger Than Block-Size Key - Hash Key First";
	hmac_sha256(key, 131, msg, strlen(msg), out);
	CHECK(same(out, "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54", 32));
	// Fed in pieces: the same
	hmac_sha256_t h;
	hmac_sha256_init(&h, key, 131);
	hmac_sha256_update(&h, msg, 10);
	hmac_sha256_update(&h, msg + 10, strlen(msg) - 10);
	hmac_sha256_final(&h, out);
	CHECK(same(out, "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54", 32));
}

static void test_hkdf(void)
{
	uint8_t ikm[22], salt[13], info[10], prk[32], okm[42];
	memset(ikm, 0x0b, sizeof ikm);
	for (unsigned i = 0; i < sizeof salt; i++)
		salt[i] = (uint8_t)i;
	for (unsigned i = 0; i < sizeof info; i++)
		info[i] = (uint8_t)(0xf0 + i);
	// RFC 5869 A.1
	hkdf_extract(salt, sizeof salt, ikm, sizeof ikm, prk);
	CHECK(same(prk, "077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5", 32));
	CHECK(hkdf_expand(prk, info, sizeof info, okm, sizeof okm));
	CHECK(same(okm, "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865", 42));
	// A.3: no salt, no info
	hkdf_extract(NULL, 0, ikm, sizeof ikm, prk);
	CHECK(same(prk, "19ef24a32c717b167f33a91d6f648bdf96596776afdb6377ac434c1c293ccb04", 32));
	CHECK(hkdf_expand(prk, NULL, 0, okm, sizeof okm));
	CHECK(same(okm, "8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d9d201395faa4b61a96c8", 42));
	// Too long for HKDF: refused, out untouched
	uint8_t big[255 * 32 + 1];
	big[0] = 0x5a;
	CHECK(!hkdf_expand(prk, NULL, 0, big, sizeof big));
	CHECK(big[0] == 0x5a);
	uint8_t a[16] = { 1 }, b[16] = { 1 };
	CHECK(crypto_equal(a, b, sizeof a));
	b[15] ^= 0x80;
	CHECK(!crypto_equal(a, b, sizeof a));
}

static void test_kcv(void)
{
	uint8_t v[KCV_LEN];
	char t[KCV_TEXT];
	// Values computed with Python's hashlib from key-check.md 2's definition
	CHECK(kcv_privacy(1, 1, v));
	kcv_text(v, t);
	CHECK(strcmp(t, "9507A7") == 0);
	CHECK(kcv_privacy(1, 255, v));
	kcv_text(v, t);
	CHECK(strcmp(t, "E8273B") == 0);
	CHECK(kcv_privacy(2, 0x1234, v));
	kcv_text(v, t);
	CHECK(strcmp(t, "6DE8CA") == 0);
	// An NXDN scrambler key record (system type 4, algorithm 1, the seed 0x1234 little endian)
	const uint8_t seed[2] = { 0x34, 0x12 };
	CHECK(kcv_traffic(4, 1, seed, sizeof seed, v));
	kcv_text(v, t);
	CHECK(strcmp(t, "51588D") == 0);
	// The same octets under another label give another value
	uint8_t w[KCV_LEN];
	CHECK(kcv_traffic(4, 2, seed, sizeof seed, w));
	CHECK(memcmp(v, w, KCV_LEN) != 0);
	uint8_t k32[32];
	for (int i = 0; i < 32; i++)
		k32[i] = (uint8_t)i;
#if CONFIG_DMR_AES
	// AES-256 of a zero block under 00..1f, checked with an independent AES (Python cryptography)
	CHECK(kcv_aes(k32, 32, v));
	kcv_text(v, t);
	CHECK(strcmp(t, "F29000") == 0);
	CHECK(kcv_traffic(5, 0x25, k32, 32, w) && memcmp(v, w, KCV_LEN) == 0);
	CHECK(!kcv_aes(k32, 16, v));	// only AES-256 keys
#else
	CHECK(!kcv_aes(k32, 32, v));	// no AES in this build
#endif
}

// The shared vectors (tests/vectors/key-check.txt), the ones the Codeplug Editor's tests read too
static void test_kcv_vectors(void)
{
	FILE *f = fopen("../vectors/key-check.txt", "r");
	CHECK(f != NULL);
	if (f == NULL)
		return;
	char line[256], kind[16], hex[130], want[8];
	unsigned a, b, n = 0;
	while (fgets(line, sizeof line, f)) {
		uint8_t key[64], v[KCV_LEN];
		char t[KCV_TEXT];
		bool ok = false;
		if (sscanf(line, "%15s", kind) != 1 || kind[0] == '#')
			continue;
		if (!strcmp(kind, "privacy") && sscanf(line, "%*s %x %x %7s", &a, &b, want) == 3) {
			ok = kcv_privacy((uint8_t)a, b, v);
		} else if (!strcmp(kind, "traffic") && sscanf(line, "%*s %x %x %129s %7s", &a, &b, hex, want) == 4) {
			unhex(hex, key, strlen(hex) / 2);
			ok = kcv_traffic((uint8_t)a, (uint8_t)b, key, (uint16_t)(strlen(hex) / 2), v);
		} else if (!strcmp(kind, "aes") && sscanf(line, "%*s %129s %7s", hex, want) == 2) {
			unhex(hex, key, strlen(hex) / 2);
			ok = kcv_aes(key, (uint16_t)(strlen(hex) / 2), v);
#if !CONFIG_DMR_AES
			CHECK(!ok);	// no AES in this build
			continue;
#endif
		} else {
			CHECK(!"a vector this test doesn't read");
			continue;
		}
		n++;
		CHECK(ok);
		kcv_text(v, t);
		if (strcmp(t, want) != 0) {
			printf("  %s: got %s, want %s\n", kind, t, want);
			failures++;
		}
	}
	fclose(f);
	CHECK(n >= 4);
}

int main(void)
{
	test_hmac();
	test_hkdf();
	test_kcv();
	test_kcv_vectors();
	printf(failures ? "# FAILED (%d failures)\n" : "# PASSED (0 failures)\n", failures);
	return failures ? 1 : 0;
}
