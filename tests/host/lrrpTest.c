// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Motorola LRRP (core/dmr/dmrLrrp.c) on its own: requests from a location server over the air decoded (every request
// ID form, triggers, the tokens that are skipped, document lengths shorter and longer than the datagram, malformed
// uintvars, request IDs empty / too long / past the end, unknown tokens), the answers built within LRRP_MAX_DOCUMENT
// for the longest request ID and every optional field, the distance; then mutated requests (fuzz.h; ODR_FUZZ_ITERS),
// each decoded one answered within bounds. Vectors are the token layouts dmrLrrp.h names (odr_sdrbench's codec).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fuzz.h"
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

static int uintvar(uint8_t *out, uint32_t v)
{
	uint8_t t[5];
	int n = 0;

	do {
		t[n++] = v & 0x7F;
		v >>= 7;
	} while (v != 0);
	for (int i = 0; i < n; i++) {
		out[i] = t[n - 1 - i] | ((i < n - 1) ? 0x80 : 0);
	}
	return n;
}

// A document: id, the tokens' length, the tokens
static int doc(uint8_t *out, uint8_t id, const uint8_t *tokens, int n)
{
	int k = 0;

	out[k++] = id;
	k += uintvar(out + k, (uint32_t)n);
	memcpy(out + k, tokens, (size_t)n);
	return k + n;
}

static void test_decode(void)
{
	uint8_t d[200];
	lrrpRequest_t r;

	// Immediate (0x05), request ID 22 04 <4 bytes>, 0x51 (accuracy + time), 0x57 direction, 0x62 speed, 0x54 altitude
	const uint8_t t1[] = { 0x22, 0x04, 0x00, 0x00, 0x00, 0x01, 0x51, 0x57, 0x62, 0x54 };
	int n = doc(d, 0x05, t1, sizeof(t1));
	CHECK(decode(d, n, &r) && r.type == LRRP_REQUEST_IMMEDIATE && r.requestIdLength == 4 && r.requestId[3] == 1);
	CHECK(r.wanted == (LRRP_WANT_ACCURACY | LRRP_WANT_TIME | LRRP_WANT_DIRECTION | LRRP_WANT_SPEED | LRRP_WANT_ALTITUDE));
	// 23 <1 byte>; 24 <uintvar> kept as 4 octets big endian
	const uint8_t t2[] = { 0x23, 0x7A, 0x50 };
	n = doc(d, 0x04, t2, sizeof(t2));
	CHECK(decode(d, n, &r) && r.requestIdLength == 1 && r.requestId[0] == 0x7A && r.wanted == LRRP_WANT_ACCURACY);
	const uint8_t t3[] = { 0x24, 0x81, 0x80, 0x01, 0x52 };	// 16385
	n = doc(d, 0x04, t3, sizeof(t3));
	CHECK(decode(d, n, &r) && r.requestIdLength == 4 && r.requestId[2] == 0x40 && r.requestId[3] == 0x01);
	// Triggered start (0x09): periodic, interval 30, distance 500, emergency condition, skipped tokens of each shape
	const uint8_t t4[] = { 0x22, 0x01, 0x09, 0x34, 0x31, 0x1E, 0x78, 0x83, 0x74, 0x4A, 0x02, 0x3F, 0x01, 0x42, 0x81, 0x00,
			       0x56, 0x01, 0x02, 0x61, 0xFF, 0x64, 0x33 };
	n = doc(d, 0x09, t4, sizeof(t4));
	CHECK(decode(d, n, &r) && r.type == LRRP_REQUEST_TRIGGERED_START && r.intervalSeconds == 30 && r.distanceMeters == 500);
	CHECK(r.triggers == (LRRP_TRIGGER_PERIODIC | LRRP_TRIGGER_DISTANCE | LRRP_TRIGGER_EMERGENCY | LRRP_TRIGGER_ONESHOT));
	// Another trigger condition isn't emergency; stop (0x0F) and protocol (0x14) requests
	const uint8_t t5[] = { 0x22, 0x01, 0x09, 0x4A, 0x01 };
	n = doc(d, 0x08, t5, sizeof(t5));
	CHECK(decode(d, n, &r) && r.triggers == 0);
	n = doc(d, 0x0F, t5, 3);
	CHECK(decode(d, n, &r) && r.type == LRRP_REQUEST_TRIGGERED_STOP);
	n = doc(d, 0x14, t5, 3);
	CHECK(decode(d, n, &r) && r.type == LRRP_REQUEST_PROTOCOL);
	// Answers and unknown documents aren't requests
	n = doc(d, 0x07, t5, 3);
	CHECK(!decode(d, n, &r));
	n = doc(d, 0x7F, t5, 3);
	CHECK(!decode(d, n, &r));
	// No request ID: not a request we can answer
	const uint8_t t6[] = { 0x50, 0x34 };
	n = doc(d, 0x05, t6, sizeof(t6));
	CHECK(!decode(d, n, &r));

	// The document shorter than the datagram: what follows it is ignored (a token past it doesn't count)
	const uint8_t t7[] = { 0x23, 0x01, 0x50, 0x57 };
	n = doc(d, 0x05, t7, sizeof(t7));
	d[1] = 3;	// the length covers 23 01 50 only
	CHECK(decode(d, n, &r) && r.wanted == LRRP_WANT_ACCURACY);
	// ... longer than the datagram (cut in transit, or a huge length): the datagram's end is used
	d[1] = 0x7F;
	CHECK(decode(d, n, &r) && r.wanted == (LRRP_WANT_ACCURACY | LRRP_WANT_DIRECTION));
	uint8_t big[16] = { 0x05, 0x87, 0xFF, 0xFF, 0xFF, 0x7F, 0x23, 0x01, 0x50 };	// length 0x7FFFFFFF
	CHECK(decode(big, 9, &r) && r.requestIdLength == 1 && r.wanted == LRRP_WANT_ACCURACY);
	big[1] = 0x8F;	// 0xFFFFFFFF
	CHECK(decode(big, 9, &r) && r.requestIdLength == 1);

	// Malformed: every truncation of a good request, a uintvar of six octets, a request ID of 0, 17 and past the
	// end, a request ID token at the very end
	n = doc(d, 0x05, t1, sizeof(t1));
	int bad = 0;
	for (int k = 0; k < n; k++) {
		bool got = decode(d, k, &r);
		bad += (k < 8) ? got : !got;	// up to the request ID's last byte nothing; after it the tokens so far
	}
	CHECK(bad == 0);
	const uint8_t six[] = { 0x05, 0x06, 0x22, 0x81, 0x81, 0x81, 0x81, 0x81, 0x01 };
	CHECK(!decode(six, sizeof(six), &r));
	const uint8_t id0[] = { 0x22, 0x00, 0x50 };
	n = doc(d, 0x05, id0, sizeof(id0));
	CHECK(!decode(d, n, &r));
	uint8_t id17[20] = { 0x22, 17 };
	n = doc(d, 0x05, id17, 19);
	CHECK(!decode(d, n, &r));
	uint8_t id16[20] = { 0x22, 16 };
	memset(id16 + 2, 0xAB, 16);
	n = doc(d, 0x05, id16, 18);
	CHECK(decode(d, n, &r) && r.requestIdLength == 16 && r.requestId[15] == 0xAB);
	n = doc(d, 0x05, id16, 17);	// one byte of the ID missing
	CHECK(!decode(d, n, &r));
	const uint8_t idEnd[] = { 0x22, 0x01, 0x05, 0x23 };	// 23 with nothing after it
	n = doc(d, 0x05, idEnd, sizeof(idEnd));
	CHECK(!decode(d, n, &r));
	// An unknown token: what came before it is used, the rest skipped
	const uint8_t unk[] = { 0x23, 0x01, 0x50, 0x7E, 0x57 };
	n = doc(d, 0x05, unk, sizeof(unk));
	CHECK(decode(d, n, &r) && r.wanted == LRRP_WANT_ACCURACY);
	CHECK(!decode(d, 0, &r) && !decode(d, 1, &r));
}

static lrrpPosition_t position(void)
{
	lrrpPosition_t p;

	memset(&p, 0, sizeof(p));
	p.location.fixValid = true;
	p.location.latDegrees = 33;
	p.location.latMinutes10000 = 450000;	// 45 minutes
	p.location.north = true;
	p.location.lonDegrees = 84;
	p.location.lonMinutes10000 = 230000;
	p.location.east = false;
	p.location.speedKnots = 12;
	p.utcYear = 2026;
	p.utcMonth = 10;
	p.utcDay = 11;
	p.location.utcHours = 23;
	p.location.utcMinutes = 59;
	p.location.utcSeconds = 58;
	p.hasCourse = true;
	p.courseDegrees = 359;
	return p;
}

static void test_build(void)
{
	lrrpRequest_t r;
	uint8_t out[LRRP_MAX_DOCUMENT + 8];
	lrrpPosition_t p = position();

	// The longest request ID with every field: within LRRP_MAX_DOCUMENT, the header's length right
	memset(&r, 0, sizeof(r));
	r.requestIdLength = LRRP_REQUEST_ID_MAX;
	memset(r.requestId, 0xCD, sizeof(r.requestId));
	r.wanted = 0xFF;
	memset(out, 0xEE, sizeof(out));
	int n = lrrpBuildImmediateResponse(&r, &p, out);
	CHECK(n > 0 && n <= LRRP_MAX_DOCUMENT && out[0] == 0x07 && out[1] == n - 2 && out[n] == 0xEE);
	CHECK(out[2] == 0x22 && out[3] == 16 && out[4] == 0xCD);
	// direction 359 -> 179, the last token
	CHECK(out[n - 2] == 0x56 && out[n - 1] == 179);
	// the speed at its largest (a knots value that doesn't happen, still encoded within bounds)
	p.location.speedKnots = 0xFFFE;
	n = lrrpBuildTriggeredReport(r.requestId, r.requestIdLength, 0xFF, &p, out);
	CHECK(n > 0 && n <= LRRP_MAX_DOCUMENT && out[0] == 0x0D);
	// No fix: the result code, and the last known position still
	p.location.fixValid = false;
	n = lrrpBuildImmediateResponse(&r, &p, out);
	CHECK(n > 0 && n <= LRRP_MAX_DOCUMENT && out[20] == 0x37 && out[21] == LRRP_RESULT_NO_FIX);
	n = lrrpBuildImmediateResponse(&r, NULL, out);
	CHECK(n == 2 + 18 + 2 && out[20] == 0x37);
	n = lrrpBuildTriggeredReport(r.requestId, 1, 0, &p, out);
	CHECK(n > 0 && out[5] == 0x37);
	// Start (OK, an error code past one octet) and stop
	n = lrrpBuildTriggeredStartResponse(r.requestId, 1, LRRP_RESULT_SUCCESS, out);
	CHECK(n == 6 && out[0] == 0x0B && out[5] == 0x38);
	n = lrrpBuildTriggeredStartResponse(r.requestId, 1, 300, out);
	CHECK(n == 8 && out[5] == 0x37 && out[6] == 0x82 && out[7] == 0x2C);
	n = lrrpBuildTriggeredStopResponse(r.requestId, 16, out);
	CHECK(n == 2 + 18 + 1 && out[0] == 0x11);
	// Southern and western hemispheres, the poles
	p = position();
	p.location.north = false;
	p.location.latDegrees = 90;
	p.location.latMinutes10000 = 0;
	p.location.lonDegrees = 180;
	n = lrrpBuildTriggeredReport(r.requestId, 1, 0, &p, out);
	CHECK(n > 0 && out[5 + 6] == 0x51 && out[5 + 7] == 0xFF && out[5 + 8] == 0xFF);	// -90: 0x7FFFFFFF | sign

	// Distance: one minute of latitude is 1852 m; nothing between a place and itself
	dmrDataLocation_t a = position().location, b = a;
	CHECK(lrrpDistanceMeters(&a, &b) == 0);
	b.latMinutes10000 += 10000;
	uint32_t m = lrrpDistanceMeters(&a, &b);
	CHECK(m > 1840 && m < 1865);
}

// Mutated requests: a decoded one has a request ID within bounds and every answer to it fits
static void test_fuzz(void)
{
	uint8_t seeds[4][64], buf[200];
	int seedLen[4];
	const uint8_t t1[] = { 0x22, 0x04, 0x00, 0x00, 0x00, 0x01, 0x51, 0x57, 0x62, 0x54 };
	const uint8_t t4[] = { 0x22, 0x01, 0x09, 0x34, 0x31, 0x1E, 0x78, 0x83, 0x74, 0x4A, 0x02, 0x3F, 0x01, 0x56, 0x01, 0x02 };
	const uint8_t t5[] = { 0x24, 0x8F, 0xFF, 0xFF, 0xFF, 0x7F, 0x61, 0x00, 0x64 };
	seedLen[0] = doc(seeds[0], 0x05, t1, sizeof(t1));
	seedLen[1] = doc(seeds[1], 0x09, t4, sizeof(t4));
	seedLen[2] = doc(seeds[2], 0x0F, t5, sizeof(t5));
	seedLen[3] = doc(seeds[3], 0x14, t1, 6);
	fuzz_t f;
	fuzz_init(&f, 0x4C525250u);
	long iters = fuzz_iters(50000), bad = 0, decoded = 0;
	lrrpPosition_t p = position();

	for (long i = 0; i < iters; i++) {
		int s = (int)fuzz_below(&f, 4);
		memcpy(buf, seeds[s], (size_t)seedLen[s]);
		int n = fuzz_mutate(&f, buf, seedLen[s], (int)sizeof(buf));
		lrrpRequest_t r;

		if (!decode(buf, n, &r)) {
			continue;
		}
		decoded++;
		uint8_t out[LRRP_MAX_DOCUMENT + 1];
		out[LRRP_MAX_DOCUMENT] = 0x5A;
		bool ok = (r.requestIdLength >= 1) && (r.requestIdLength <= LRRP_REQUEST_ID_MAX);
		int m = lrrpBuildImmediateResponse(&r, &p, out);
		ok = ok && (m > 0) && (m <= LRRP_MAX_DOCUMENT);
		m = lrrpBuildTriggeredReport(r.requestId, r.requestIdLength, r.wanted, &p, out);
		ok = ok && (m > 0) && (m <= LRRP_MAX_DOCUMENT) && (out[LRRP_MAX_DOCUMENT] == 0x5A);
		bad += !ok;
	}
	CHECK(bad == 0 && decoded > iters / 20);
	printf("# lrrp fuzz: %ld requests, %ld decoded\n", iters, decoded);
}

int main(void)
{
	test_decode();
	test_build();
	test_fuzz();
	printf("lrrpTest: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
