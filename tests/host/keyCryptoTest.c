// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The key primitives (core/support): HMAC-SHA-256 on RFC 4231's cases, HKDF on RFC 5869's, X25519 on RFC 7748's
// vectors (5.2 one and a thousand iterations, the 6.1 exchange) with the low-order refusal, and the key check values
// of docs/spec/key-check.md against values computed apart from this code (Python hashlib from the spec's text)
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "support/hkdf.h"
#include "support/kcv.h"
#include "support/x25519.h"

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

static void test_x25519(void)
{
	static x25519_ws_t ws;
	uint8_t k[32], u[32], out[32];
	// RFC 7748 5.2
	unhex("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4", k, 32);
	unhex("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c", u, 32);
	CHECK(x25519(out, k, u, &ws));
	CHECK(same(out, "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552", 32));
	unhex("4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d", k, 32);
	unhex("e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493", u, 32);
	CHECK(x25519(out, k, u, &ws));
	CHECK(same(out, "95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957", 32));
	// 5.2 iterated: k = u = 9, then k <- X25519(k, u), u <- old k
	memset(k, 0, 32);
	k[0] = 9;
	memcpy(u, k, 32);
	for (int i = 1; i <= 1000; i++) {
		x25519(out, k, u, &ws);
		memcpy(u, k, 32);
		memcpy(k, out, 32);
		if (i == 1)
			CHECK(same(k, "422c8e7a6227d7bca1350b3e2bb7279f7897b87bb6854b783c60e80311ae3079", 32));
	}
	CHECK(same(k, "684cf59ba83309552800ef566f2f4d3c1c3887c49360e3875f2eb94d99532c51", 32));
	// 6.1: both public keys and the shared secret
	uint8_t a[32], b[32], pa[32], pb[32], s1[32], s2[32];
	unhex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", a, 32);
	unhex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb", b, 32);
	x25519_public(pa, a, &ws);
	x25519_public(pb, b, &ws);
	CHECK(same(pa, "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a", 32));
	CHECK(same(pb, "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f", 32));
	CHECK(x25519(s1, a, pb, &ws));
	CHECK(x25519(s2, b, pa, &ws));
	CHECK(same(s1, "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742", 32));
	CHECK(memcmp(s1, s2, 32) == 0);
	// Low order u (0, 1): the all-zero result is refused (6.1)
	memset(u, 0, 32);
	CHECK(!x25519(out, a, u, &ws));
	u[0] = 1;
	CHECK(!x25519(out, a, u, &ws));
	// The workspace is wiped
	static const x25519_ws_t zero;
	CHECK(memcmp(&ws, &zero, sizeof ws) == 0);
	// Agreement on many key pairs, made by a simple generator (not a source of keys: a test's)
	uint32_t r = 0x12345678u;
	for (int n = 0; n < 20; n++) {
		for (int i = 0; i < 32; i++) {
			r = r * 1103515245u + 12345u;
			a[i] = (uint8_t)(r >> 16);
			r = r * 1103515245u + 12345u;
			b[i] = (uint8_t)(r >> 16);
		}
		x25519_public(pa, a, &ws);
		x25519_public(pb, b, &ws);
		CHECK(x25519(s1, a, pb, &ws) && x25519(s2, b, pa, &ws) && memcmp(s1, s2, 32) == 0);
	}
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
	test_x25519();
	test_kcv();
	test_kcv_vectors();
	printf(failures ? "# FAILED (%d failures)\n" : "# PASSED (0 failures)\n", failures);
	return failures ? 1 : 0;
}
