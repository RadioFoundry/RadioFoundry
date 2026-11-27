// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The key check values of docs/spec/key-check.md (core/support/kcv.c) against values computed apart from this code
// (Python hashlib from the spec's text), and the published vectors (tests/vectors/key-check.txt)
#include <stdio.h>
#include <string.h>
#include "config.h"
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
	test_kcv();
	test_kcv_vectors();
	printf(failures ? "# FAILED (%d failures)\n" : "# PASSED (0 failures)\n", failures);
	return failures ? 1 : 0;
}
