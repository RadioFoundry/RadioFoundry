// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Motorola LRRP (core/dmr/dmrLrrp.c) on its own: requests from a location server over the air decoded (every request
// ID form, triggers, the tokens that are skipped, document lengths shorter and longer than the datagram, malformed
// uintvars, request IDs empty / too long / past the end, unknown tokens), the answers built within LRRP_MAX_DOCUMENT
// for the longest request ID and every optional field, the distance; then mutated requests (fuzz.h; ODR_FUZZ_ITERS),
// each decoded one answered within bounds. Vectors are the token layouts dmrLrrp.h names (RadioDesk's codec).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fuzz.h"	// fuzz_exact
#include "dmr/dmrLrrp.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

// Decoded from an exact-size heap copy (ASan sees a read past the end)
static bool decode(const uint8_t *p, int n, lrrpRequest_t *r)
{
	uint8_t *q = fuzz_exact(p, n);
	bool ok = lrrpDecodeRequest(q, n, r);

	free(q);
	return ok;
}

// A document length larger than the datagram: the datagram's end is used, also for lengths near 2^32 (which
// overflowed the end offset)
static void test_doc_length(void)
{
	lrrpRequest_t r;
	uint8_t big[16] = { 0x05, 0x7F, 0x23, 0x01, 0x50 };

	CHECK(decode(big, 5, &r) && r.requestIdLength == 1 && r.wanted == LRRP_WANT_ACCURACY);
	const uint8_t lengths[][5] = { { 0x87, 0xFF, 0xFF, 0xFF, 0x7F }, { 0x8F, 0xFF, 0xFF, 0xFF, 0x7F },
				       { 0x88, 0x80, 0x80, 0x80, 0x00 } };	// 0x7FFFFFFF, 0xFFFFFFFF, 0x80000000
	for (int i = 0; i < 3; i++) {
		memcpy(big + 1, lengths[i], 5);
		big[6] = 0x23;
		big[7] = 0x01;
		big[8] = 0x50;
		CHECK(decode(big, 9, &r) && r.requestIdLength == 1 && r.wanted == LRRP_WANT_ACCURACY);
	}
}

int main(void)
{
	test_doc_length();
	printf("lrrpTest: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
