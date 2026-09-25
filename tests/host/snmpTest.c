// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The SNMP agent's pure parts: the BER codec (core/snmp/snmpBer.c) against encodings worked out by hand from ITU-T
// X.690, the SNMPv2c processing (core/snmp/snmp.c) with requests built byte by byte here (RFC 3416 / RFC 1901), the
// radio's MIB (core/snmp/snmpMib.c) on a filled snapshot, and the notifications (detection, queue, RF rate limit,
// SNMPv2-Trap encoding).
#include <stdio.h>
#include <string.h>
#include "snmp/snmp.h"
#include "snmp/snmpMib.h"
#include "dmr/dmrDataService.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

// ---- a tiny independent encoder for the requests (short-form lengths only, so every message here stays < 128) ----

typedef struct {
	uint8_t b[600];
	int n;
} buf_t;

static void raw(buf_t *o, const void *p, int n)
{
	memcpy(o->b + o->n, p, (size_t)n);
	o->n += n;
}

static void tlv(buf_t *o, uint8_t tag, const void *p, int n)
{
	o->b[o->n++] = tag;
	o->b[o->n++] = (uint8_t)n;
	raw(o, p, n);
}

static void wrap(buf_t *o, uint8_t tag, const buf_t *inner)
{
	tlv(o, tag, inner->b, inner->n);
}

static void intv(buf_t *o, uint32_t v, int octets)
{
	uint8_t t[4];

	for (int i = 0; i < octets; i++) {
		t[i] = (uint8_t)(v >> (8 * (octets - 1 - i)));
	}
	tlv(o, 0x02, t, octets);
}

// A request: version, community, the PDU with request ID 0x1234, f2 / f3, bindings (OID with NULL value)
static int request(uint8_t *out, int version, const char *community, uint8_t pdu, int f2, int f3, const uint8_t *const *oids,
		   const int *lens, int count)
{
	buf_t list = { .n = 0 }, p = { .n = 0 }, m = { .n = 0 }, msg = { .n = 0 };

	for (int i = 0; i < count; i++) {
		buf_t vb = { .n = 0 };

		tlv(&vb, 0x06, oids[i], lens[i]);
		tlv(&vb, 0x05, NULL, 0);
		wrap(&list, 0x30, &vb);
	}
	intv(&p, 0x1234, 2);
	intv(&p, (uint32_t)f2, 1);
	intv(&p, (uint32_t)f3, 1);
	wrap(&p, 0x30, &list);
	intv(&m, (uint32_t)version, 1);
	tlv(&m, 0x04, community, (int)strlen(community));
	wrap(&m, pdu, &p);
	wrap(&msg, 0x30, &m);
	memcpy(out, msg.b, (size_t)msg.n);
	return msg.n;
}

// ---- the response, decoded with the module's reader ----

typedef struct {
	int32_t requestId, status, index;
	int count;
	struct {
		uint8_t oid[64];
		int oidLen;
		uint8_t tag;
		uint8_t val[80];
		int valLen;
	} vb[40];
} resp_t;

static bool decode(const uint8_t *p, int n, resp_t *r, uint8_t expectPdu)
{
	berReader_t top, msg, pdu, list, vb;
	int32_t version;
	uint8_t tag;
	const uint8_t *v;
	int len;

	memset(r, 0, sizeof(*r));
	berReaderInit(&top, p, n);
	if (!berEnter(&top, BER_SEQUENCE, &msg) || !berAtEnd(&top) || !berGetInt(&msg, &version) || version != 1 ||
	    !berGet(&msg, &tag, &v, &len) || tag != BER_OCTETS || !berEnter(&msg, expectPdu, &pdu) ||
	    !berGetInt(&pdu, &r->requestId) || !berGetInt(&pdu, &r->status) || !berGetInt(&pdu, &r->index) ||
	    !berEnter(&pdu, BER_SEQUENCE, &list)) {
		return false;
	}
	while (!berAtEnd(&list)) {
		if (!berEnter(&list, BER_SEQUENCE, &vb) || !berGet(&vb, &tag, &v, &len) || tag != BER_OID || r->count >= 40) {
			return false;
		}
		memcpy(r->vb[r->count].oid, v, (size_t)len);
		r->vb[r->count].oidLen = len;
		if (!berGet(&vb, &r->vb[r->count].tag, &v, &len) || len > 80) {
			return false;
		}
		memcpy(r->vb[r->count].val, v, (size_t)len);
		r->vb[r->count].valLen = len;
		r->count++;
	}
	return !list.err;
}

static uint32_t valNum(const resp_t *r, int i)
{
	uint32_t u = 0;
	int32_t s;

	if (r->vb[i].tag == BER_INTEGER && berDecodeInt(r->vb[i].val, r->vb[i].valLen, &s)) {
		return (uint32_t)s;
	}
	berDecodeUnsigned(r->vb[i].val, r->vb[i].valLen, &u);
	return u;
}

// ---- BER vectors ----

static void encodes(void (*put)(berWriter_t *), const uint8_t *want, int n, const char *what)
{
	uint8_t out[300];
	berWriter_t w;

	berWriterInit(&w, out, sizeof(out));
	put(&w);
	bool ok = !w.err && w.pos == n && memcmp(out, want, (size_t)n) == 0;
	if (!ok) {
		printf("  %s: got", what);
		for (int i = 0; i < w.pos && i < 12; i++) {
			printf(" %02X", out[i]);
		}
		printf("\n");
	}
	CHECK(ok);
}

#define VEC(name, body, ...) \
	static void put_##name(berWriter_t *w) { body; } \
	static const uint8_t want_##name[] = { __VA_ARGS__ };
VEC(i0, berPutInt(w, 2, 0), 0x02, 0x01, 0x00)
VEC(i127, berPutInt(w, 2, 127), 0x02, 0x01, 0x7F)
VEC(i128, berPutInt(w, 2, 128), 0x02, 0x02, 0x00, 0x80)
VEC(i256, berPutInt(w, 2, 256), 0x02, 0x02, 0x01, 0x00)
VEC(im1, berPutInt(w, 2, -1), 0x02, 0x01, 0xFF)
VEC(im128, berPutInt(w, 2, -128), 0x02, 0x01, 0x80)
VEC(im129, berPutInt(w, 2, -129), 0x02, 0x02, 0xFF, 0x7F)
VEC(imin, berPutInt(w, 2, INT32_MIN), 0x02, 0x04, 0x80, 0x00, 0x00, 0x00)
VEC(c32max, berPutUnsigned(w, 0x41, 0xFFFFFFFFu), 0x41, 0x05, 0x00, 0xFF, 0xFF, 0xFF, 0xFF)
VEC(g32, berPutUnsigned(w, 0x42, 0x8000u), 0x42, 0x03, 0x00, 0x80, 0x00)
VEC(null, berPutNull(w, 5), 0x05, 0x00)
VEC(str, berPutOctets(w, 4, "public", 6), 0x04, 0x06, 'p', 'u', 'b', 'l', 'i', 'c')

static void put_long(berWriter_t *w)
{
	uint8_t z[200];

	memset(z, 0xA5, sizeof(z));
	int m = berBegin(w, 0x30);
	berPutOctets(w, 4, z, 130);	// 04 81 82 ... = 133 octets of contents: 30 81 85
	berEnd(w, m);
}

static void test_ber(void)
{
	encodes(put_i0, want_i0, sizeof(want_i0), "0");
	encodes(put_i127, want_i127, sizeof(want_i127), "127");
	encodes(put_i128, want_i128, sizeof(want_i128), "128");
	encodes(put_i256, want_i256, sizeof(want_i256), "256");
	encodes(put_im1, want_im1, sizeof(want_im1), "-1");
	encodes(put_im128, want_im128, sizeof(want_im128), "-128");
	encodes(put_im129, want_im129, sizeof(want_im129), "-129");
	encodes(put_imin, want_imin, sizeof(want_imin), "INT32_MIN");
	encodes(put_c32max, want_c32max, sizeof(want_c32max), "Counter32 max");
	encodes(put_g32, want_g32, sizeof(want_g32), "Gauge32 0x8000");
	encodes(put_null, want_null, sizeof(want_null), "NULL");
	encodes(put_str, want_str, sizeof(want_str), "OCTET STRING");

	uint8_t out[300];
	berWriter_t w;
	berWriterInit(&w, out, sizeof(out));
	put_long(&w);
	CHECK(!w.err && w.pos == 136 && out[0] == 0x30 && out[1] == 0x81 && out[2] == 0x85 && out[3] == 0x04 &&
	      out[4] == 0x81 && out[5] == 0x82 && out[6] == 0xA5);
	berWriterInit(&w, out, 100);	// too small: an error, never an overrun
	put_long(&w);
	CHECK(w.err);

	// Decoding: the long length forms, a negative unsigned, indefinite length
	const uint8_t two[] = { 0x04, 0x82, 0x00, 0x02, 'h', 'i' };
	berReader_t r;
	uint8_t tag;
	const uint8_t *v;
	int n;
	berReaderInit(&r, two, sizeof(two));
	CHECK(berGet(&r, &tag, &v, &n) && tag == 4 && n == 2 && v[0] == 'h' && berAtEnd(&r));
	const uint8_t indef[] = { 0x30, 0x80, 0x00, 0x00 };
	berReaderInit(&r, indef, sizeof(indef));
	CHECK(!berGet(&r, &tag, &v, &n) && r.err);
	const uint8_t over[] = { 0x04, 0x05, 'a' };
	berReaderInit(&r, over, sizeof(over));
	CHECK(!berGet(&r, &tag, &v, &n));
	uint32_t u;
	const uint8_t neg[] = { 0x80, 0x00 };
	CHECK(!berDecodeUnsigned(neg, 2, &u));
	const uint8_t big[] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF };
	CHECK(berDecodeUnsigned(big, 5, &u) && u == 0xFFFFFFFFu);
	const uint8_t toobig[] = { 0x01, 0x00, 0x00, 0x00, 0x00 };
	CHECK(!berDecodeUnsigned(toobig, 5, &u));
	int32_t s;
	const uint8_t m129[] = { 0xFF, 0x7F };
	CHECK(berDecodeInt(m129, 2, &s) && s == -129);

	// OIDs: 1.3.6.1.2.1.1.1.0 (X.690 8.19: 40 * 1 + 3 = 43 = 0x2B), subidentifier 32473 = 0x81 0xFD 0x59
	const uint8_t sysDescr[] = { 0x2B, 6, 1, 2, 1, 1, 1, 0 };
	const uint8_t pen[] = { 0x2B, 6, 1, 4, 1, 0x81, 0xFD, 0x59 };
	CHECK(berOidValid(sysDescr, sizeof(sysDescr)) && berOidValid(pen, sizeof(pen)));
	const uint8_t lead80[] = { 0x2B, 0x80, 0x01 };
	const uint8_t open[] = { 0x2B, 0x81 };
	const uint8_t over32[] = { 0x2B, 0x90, 0x80, 0x80, 0x80, 0x00 };	// 2^32
	const uint8_t max32[] = { 0x2B, 0x8F, 0xFF, 0xFF, 0xFF, 0x7F };	// 2^32 - 1
	CHECK(!berOidValid(lead80, 3) && !berOidValid(open, 2) && !berOidValid(over32, 6) && berOidValid(max32, 6));
	// Order: 127 < 128 < 16384 as subidentifiers, a prefix first
	const uint8_t a127[] = { 0x2B, 0x7F }, a128[] = { 0x2B, 0x81, 0x00 }, a16384[] = { 0x2B, 0x81, 0x80, 0x00 };
	const uint8_t a1281[] = { 0x2B, 0x81, 0x00, 0x01 };
	CHECK(berOidCompare(a127, 2, a128, 3) < 0 && berOidCompare(a128, 3, a16384, 4) < 0);
	CHECK(berOidCompare(a128, 3, a1281, 4) < 0 && berOidCompare(a1281, 4, a16384, 4) < 0);
	CHECK(berOidCompare(sysDescr, 8, sysDescr, 8) == 0);
}

// ---- the radio's MIB on a snapshot ----

static snmpData_t data;
static snmpConfig_t cfg;
static snmpStats_t stats;
static snmpMibCtx_t ctx;
static snmpMib_t mib;

static void setup(void)
{
	memset(&data, 0, sizeof(data));
	memset(&cfg, 0, sizeof(cfg));
	memset(&stats, 0, sizeof(stats));
	memset(&ctx, 0, sizeof(ctx));
	data.uptimeMs = 123450;
	data.dmrId = 3100001;
	strcpy(data.name, "N0CALL");
	data.board = "dm1701";
	data.firmware = "abcdef12";
	data.batteryMv = 7400;
	data.batteryPct = 80;
	data.mode = 1;
	data.rssiDbm = -93;
	strcpy(data.zoneName, "Local");
	strcpy(data.channelName, "Simplex 1");
	data.channelNumber = 7;
	data.rxHz = data.txHz = 438500000;
	data.powerName = "High";
	data.power = 2;
	data.cc = 1;
	data.ts = 2;
	data.talkTarget = 91;
	ctx.data = &data;
	ctx.cfg = &cfg;
	ctx.stats = &stats;
	snmpMibInit(&mib, &ctx);
}

static snmpRequestCfg_t rcfg(bool writeAllowed)
{
	snmpRequestCfg_t c = { .mib = &mib, .readHash = cfg.readHash, .writeHash = cfg.writeHash,
			       .writeAllowed = writeAllowed, .maxRepetitions = 20, .stats = &stats };
	return c;
}

static int process(bool rf, const uint8_t *req, int n, uint8_t *out, int cap, bool *auth)
{
	snmpRequestCfg_t c = rcfg(!rf);
	return snmpProcess(&c, req, n, out, cap, auth);
}

static const uint8_t SYS_DESCR[] = { 0x2B, 6, 1, 2, 1, 1, 1, 0 };
static const uint8_t SYS_NAME[] = { 0x2B, 6, 1, 2, 1, 1, 5, 0 };
static const uint8_t SYS_UPTIME[] = { 0x2B, 6, 1, 2, 1, 1, 3, 0 };
static const uint8_t SYSTEM[] = { 0x2B, 6, 1, 2, 1, 1 };
static const uint8_t ODR_DMR_ID[] = { 0x2B, 6, 1, 4, 1, 0x81, 0xFD, 0x59, 2, 1, 1, 1, 0 };
static const uint8_t ODR_RF_MANAGER[] = { 0x2B, 6, 1, 4, 1, 0x81, 0xFD, 0x59, 2, 1, 7, 1, 0 };

static void test_get(void)
{
	setup();
	// The GET of sysDescr.0 built entirely by hand (RFC 1901 message, RFC 3416 GetRequest-PDU)
	const uint8_t req[] = {
		0x30, 0x29, 0x02, 0x01, 0x01, 0x04, 0x06, 'p', 'u', 'b', 'l', 'i', 'c',
		0xA0, 0x1C, 0x02, 0x04, 0x12, 0x34, 0x56, 0x78, 0x02, 0x01, 0x00, 0x02, 0x01, 0x00,
		0x30, 0x0E, 0x30, 0x0C, 0x06, 0x08, 0x2B, 0x06, 0x01, 0x02, 0x01, 0x01, 0x01, 0x00, 0x05, 0x00,
	};
	uint8_t out[512];
	bool auth;
	resp_t r;
	int n = process(false, req, sizeof(req), out, sizeof(out), &auth);
	CHECK(n > 0 && decode(out, n, &r, SNMP_PDU_RESPONSE));
	CHECK(r.requestId == 0x12345678 && r.status == 0 && r.count == 1);
	CHECK(r.vb[0].tag == BER_OCTETS && r.vb[0].valLen == 32 &&
	      memcmp(r.vb[0].val, "OpenDigitalRadio dm1701 abcdef12", 32) == 0);
	// The whole answer as bytes: 30 49 02 01 01 04 06 public A2 3C 02 04 12345678 02 01 00 02 01 00 30 2E 30 2C 06 08
	// <sysDescr.0> 04 20 <text>
	CHECK(n == 75 && out[0] == 0x30 && out[1] == 0x49 && out[13] == 0xA2 && out[14] == 0x3C && out[27] == 0x30 &&
	      out[28] == 0x2E && out[29] == 0x30 && out[30] == 0x2C && out[41] == 0x04 && out[42] == 32);
	CHECK(stats.inPkts == 1 && stats.outPkts == 1 && stats.gets == 1);

	// Several at once, one missing object and one missing instance
	const uint8_t noObj[] = { 0x2B, 6, 1, 2, 1, 99, 0 };
	const uint8_t noInst[] = { 0x2B, 6, 1, 2, 1, 1, 1, 1 };
	const uint8_t *oids[] = { SYS_UPTIME, ODR_DMR_ID, noObj, noInst };
	const int lens[] = { sizeof(SYS_UPTIME), sizeof(ODR_DMR_ID), sizeof(noObj), sizeof(noInst) };
	uint8_t q[300];
	int qn = request(q, 1, "public", SNMP_PDU_GET, 0, 0, oids, lens, 4);
	n = process(true, q, qn, out, sizeof(out), &auth);
	CHECK(n > 0 && decode(out, n, &r, SNMP_PDU_RESPONSE) && r.count == 4 && r.requestId == 0x1234);
	CHECK(r.vb[0].tag == BER_TIMETICKS && valNum(&r, 0) == 12345);
	CHECK(r.vb[1].tag == BER_GAUGE32 && valNum(&r, 1) == 3100001);
	CHECK(r.vb[2].tag == BER_NO_SUCH_OBJECT && r.vb[2].valLen == 0);
	CHECK(r.vb[3].tag == BER_NO_SUCH_OBJECT);	// no instance table: noSuchObject (snmp.md 3)

	// An FM channel has no color code: noSuchInstance
	const uint8_t cc[] = { 0x2B, 6, 1, 4, 1, 0x81, 0xFD, 0x59, 2, 1, 3, 11, 0 };
	const uint8_t *o1[] = { cc };
	const int l1[] = { sizeof(cc) };
	data.mode = 0;
	qn = request(q, 1, "public", SNMP_PDU_GET, 0, 0, o1, l1, 1);
	n = process(false, q, qn, out, sizeof(out), &auth);
	CHECK(n > 0 && decode(out, n, &r, SNMP_PDU_RESPONSE) && r.vb[0].tag == BER_NO_SUCH_INSTANCE);
	data.mode = 1;
	n = process(false, q, qn, out, sizeof(out), &auth);
	CHECK(n > 0 && decode(out, n, &r, SNMP_PDU_RESPONSE) && r.vb[0].tag == BER_GAUGE32 && valNum(&r, 0) == 1);
}

static void test_drops(void)
{
	setup();
	CHECK(snmpCommunityHash((const uint8_t *)"public", 6) == SNMP_HASH_PUBLIC);
	uint8_t q[300], out[512];
	bool auth;
	const uint8_t *oids[] = { SYS_DESCR };
	const int lens[] = { sizeof(SYS_DESCR) };

	int qn = request(q, 1, "private", SNMP_PDU_GET, 0, 0, oids, lens, 1);
	CHECK(process(false, q, qn, out, sizeof(out), &auth) == 0 && auth && stats.inBadCommunityNames == 1);
	qn = request(q, 0, "public", SNMP_PDU_GET, 0, 0, oids, lens, 1);	// SNMPv1
	CHECK(process(false, q, qn, out, sizeof(out), &auth) == 0 && !auth && stats.inBadVersions == 1);
	qn = request(q, 1, "public", SNMP_PDU_GET, 0, 0, oids, lens, 1);
	CHECK(process(false, q, qn - 3, out, sizeof(out), &auth) == 0 && stats.inASNParseErrs == 1);	// truncated
	q[qn - 12] = 0x80;	// a malformed OID (leading 0x80 octet) in the binding
	CHECK(process(false, q, qn, out, sizeof(out), &auth) == 0 && stats.inASNParseErrs == 2);
	qn = request(q, 1, "public", SNMP_PDU_RESPONSE, 0, 0, oids, lens, 1);	// not a request
	CHECK(process(false, q, qn, out, sizeof(out), &auth) == 0);
	CHECK(stats.outPkts == 0);

	// A configured read community replaces "public"
	cfg.readHash = snmpCommunityHash((const uint8_t *)"s3cret", 6);
	qn = request(q, 1, "public", SNMP_PDU_GET, 0, 0, oids, lens, 1);
	CHECK(process(false, q, qn, out, sizeof(out), &auth) == 0 && auth);
	qn = request(q, 1, "s3cret", SNMP_PDU_GET, 0, 0, oids, lens, 1);
	CHECK(process(false, q, qn, out, sizeof(out), &auth) > 0);
}

// Walk the whole MIB with GetNext from 1.3: every instance once, in increasing order
static void test_walk(void)
{
	setup();
	uint8_t q[300], out[512], cur[64] = { 0x2B };
	int curLen = 1, count = 0;
	bool auth, ordered = true;
	resp_t r;

	for (;;) {
		const uint8_t *oids[] = { cur };
		const int lens[] = { curLen };
		int qn = request(q, 1, "public", SNMP_PDU_GETNEXT, 0, 0, oids, lens, 1);
		int n = process(false, q, qn, out, sizeof(out), &auth);
		if (n <= 0 || !decode(out, n, &r, SNMP_PDU_RESPONSE) || r.count != 1) {
			ordered = false;
			break;
		}
		if (r.vb[0].tag == BER_END_OF_MIB_VIEW) {
			CHECK(r.vb[0].oidLen == curLen && memcmp(r.vb[0].oid, cur, (size_t)curLen) == 0);
			break;
		}
		if (berOidCompare(r.vb[0].oid, r.vb[0].oidLen, cur, curLen) <= 0) {
			ordered = false;
			break;
		}
		memcpy(cur, r.vb[0].oid, (size_t)r.vb[0].oidLen);
		curLen = r.vb[0].oidLen;
		count++;
	}
	CHECK(ordered);
	CHECK(count == mib.count - 1);	// every instance but odrLastAge, which has no value until a call is heard
	uint8_t a[SNMP_OID_MAX], b[SNMP_OID_MAX];
	for (int i = 1; i < mib.count; i++) {
		int an = snmpObjectOid(&mib, i - 1, a), bn = snmpObjectOid(&mib, i, b);
		CHECK(berOidCompare(a, an, b, bn) < 0 && berOidValid(b, bn));
	}
	CHECK(snmpFind(&mib, SYS_NAME, sizeof(SYS_NAME)) == 4);	// the fifth instance
}

static void test_bulk(void)
{
	setup();
	uint8_t q[300], out[512];
	bool auth;
	resp_t r;
	// non-repeaters 1 (sysDescr's next: sysObjectID), repeaters system and the snmp group's start, 3 rounds
	const uint8_t snmpGroup[] = { 0x2B, 6, 1, 2, 1, 11 };
	const uint8_t *oids[] = { SYS_DESCR, SYSTEM, snmpGroup };
	const int lens[] = { sizeof(SYS_DESCR), sizeof(SYSTEM), sizeof(snmpGroup) };
	int qn = request(q, 1, "public", SNMP_PDU_GETBULK, 1, 3, oids, lens, 3);
	int n = process(false, q, qn, out, sizeof(out), &auth);
	CHECK(n > 0 && decode(out, n, &r, SNMP_PDU_RESPONSE) && r.status == 0 && r.count == 7);
	CHECK(r.vb[0].oid[6] == 2);					// sysObjectID.0
	CHECK(r.vb[1].oid[6] == 1 && r.vb[2].oid[5] == 11 && r.vb[2].oid[6] == 1);	// round 1
	CHECK(r.vb[3].oid[6] == 2 && r.vb[4].oid[6] == 3);		// round 2: sysObjectID, snmpInBadVersions
	CHECK(r.vb[5].oid[6] == 3 && r.vb[6].oid[6] == 4);		// round 3
	CHECK(stats.getBulks == 1);

	// Near the end: the column runs out (endOfMibView named as the last instance), the walk stops after a round
	// with every column at the end
	const uint8_t last[] = { 0x2B, 6, 1, 4, 1, 0x81, 0xFD, 0x59, 2, 1, 7, 7, 0 };
	const uint8_t *o2[] = { last };
	const int l2[] = { sizeof(last) };
	qn = request(q, 1, "public", SNMP_PDU_GETBULK, 0, 10, o2, l2, 1);
	n = process(false, q, qn, out, sizeof(out), &auth);
	CHECK(n > 0 && decode(out, n, &r, SNMP_PDU_RESPONSE) && r.count == 2);
	CHECK(r.vb[0].tag == BER_COUNTER32 && r.vb[1].tag == BER_END_OF_MIB_VIEW);
	CHECK(r.vb[1].oidLen == r.vb[0].oidLen && memcmp(r.vb[1].oid, r.vb[0].oid, (size_t)r.vb[0].oidLen) == 0);

	// max-repetitions 100 from the start of the MIB: as many as fit, whole bindings only
	const uint8_t start[] = { 0x2B };
	const uint8_t *o3[] = { start };
	const int l3[] = { 1 };
	qn = request(q, 1, "public", SNMP_PDU_GETBULK, 0, 100, o3, l3, 1);
	n = process(false, q, qn, out, 300, &auth);
	CHECK(n > 200 && n <= 300 && decode(out, n, &r, SNMP_PDU_RESPONSE) && r.count > 5 && r.count < 20);
	snmpRequestCfg_t c = rcfg(true);
	c.maxRepetitions = 3;
	n = snmpProcess(&c, q, qn, out, sizeof(out), &auth);
	CHECK(n > 0 && decode(out, n, &r, SNMP_PDU_RESPONSE) && r.count == 3);	// capped
}

static void test_too_big(void)
{
	setup();
	uint8_t q[300], out[512];
	bool auth;
	resp_t r;
	const uint8_t *oids[] = { SYS_DESCR, SYS_DESCR, SYS_DESCR, SYS_DESCR };
	const int lens[] = { 8, 8, 8, 8 };
	int qn = request(q, 1, "public", SNMP_PDU_GET, 0, 0, oids, lens, 4);
	int n = process(false, q, qn, out, 120, &auth);
	CHECK(n > 0 && n <= 120 && decode(out, n, &r, SNMP_PDU_RESPONSE) && r.status == SNMP_ERR_TOO_BIG && r.count == 0);
	CHECK(process(false, q, qn, out, 20, &auth) == 0 && stats.silentDrops == 1);	// not even tooBig fits
}

// A SET request: (oid, tag, value) triples
static int setRequest(uint8_t *out, const char *community, const uint8_t *const *oids, const int *lens, const uint8_t *tags,
		      const uint8_t *const *vals, const int *vlens, int count)
{
	buf_t list = { .n = 0 }, p = { .n = 0 }, m = { .n = 0 }, msg = { .n = 0 };

	for (int i = 0; i < count; i++) {
		buf_t vb = { .n = 0 };

		tlv(&vb, 0x06, oids[i], lens[i]);
		tlv(&vb, tags[i], vals[i], vlens[i]);
		wrap(&list, 0x30, &vb);
	}
	intv(&p, 77, 1);
	intv(&p, 0, 1);
	intv(&p, 0, 1);
	wrap(&p, 0x30, &list);
	intv(&m, 1, 1);
	tlv(&m, 0x04, community, (int)strlen(community));
	wrap(&m, SNMP_PDU_SET, &p);
	wrap(&msg, 0x30, &m);
	memcpy(out, msg.b, (size_t)msg.n);
	return msg.n;
}

static void test_set(void)
{
	setup();
	uint8_t q[300], out[512];
	bool auth;
	resp_t r;
	const uint8_t *oids[] = { SYS_NAME, ODR_RF_MANAGER };
	const int lens[] = { sizeof(SYS_NAME), sizeof(ODR_RF_MANAGER) };
	const uint8_t tags[] = { BER_OCTETS, BER_GAUGE32 };
	const uint8_t mgr[] = { 0x0F, 0x42, 0x40 };	// 1000000
	const uint8_t *vals[] = { (const uint8_t *)"W1AW", mgr };
	const int vlens[] = { 4, 3 };

	// No write community yet: refused for the read community (noAccess, index 1)
	int qn = setRequest(q, "public", oids, lens, tags, vals, vlens, 2);
	int n = process(false, q, qn, out, sizeof(out), &auth);
	CHECK(n > 0 && decode(out, n, &r, SNMP_PDU_RESPONSE) && r.status == SNMP_ERR_NO_ACCESS && r.index == 1);
	CHECK(r.count == 2 && r.requestId == 77 && stats.inBadCommunityUses == 1 && !ctx.nameChanged);

	cfg.writeHash = snmpCommunityHash((const uint8_t *)"private", 7);
	// Over RF without the setting: noAccess even with the write community
	qn = setRequest(q, "private", oids, lens, tags, vals, vlens, 2);
	n = process(true, q, qn, out, sizeof(out), &auth);
	CHECK(n > 0 && decode(out, n, &r, SNMP_PDU_RESPONSE) && r.status == SNMP_ERR_NO_ACCESS && !ctx.cfgChanged);
	// Over USB: both change, the bindings come back as sent
	n = process(false, q, qn, out, sizeof(out), &auth);
	CHECK(n > 0 && decode(out, n, &r, SNMP_PDU_RESPONSE) && r.status == 0 && r.count == 2);
	CHECK(r.vb[0].valLen == 4 && memcmp(r.vb[0].val, "W1AW", 4) == 0 && valNum(&r, 1) == 1000000);
	CHECK(ctx.nameChanged && strcmp(ctx.newName, "W1AW") == 0 && strcmp(data.name, "W1AW") == 0);
	CHECK(ctx.cfgChanged && cfg.rfManager == 1000000 && stats.setsDone == 1);

	// Atomic: the second binding's wrong type fails the request (index 2), the first isn't applied
	ctx.nameChanged = ctx.cfgChanged = false;
	const uint8_t tags2[] = { BER_OCTETS, BER_INTEGER };
	const uint8_t *vals2[] = { (const uint8_t *)"K1ABC", mgr };
	const int vlens2[] = { 5, 3 };
	qn = setRequest(q, "private", oids, lens, tags2, vals2, vlens2, 2);
	n = process(false, q, qn, out, sizeof(out), &auth);
	CHECK(n > 0 && decode(out, n, &r, SNMP_PDU_RESPONSE) && r.status == SNMP_ERR_WRONG_TYPE && r.index == 2);
	CHECK(!ctx.nameChanged && strcmp(data.name, "W1AW") == 0);

	// notWritable, noCreation, wrongLength, wrongValue
	const uint8_t *o1[] = { SYS_DESCR };
	const int l1[] = { 8 };
	const uint8_t t1[] = { BER_OCTETS };
	const uint8_t *v1[] = { (const uint8_t *)"x" };
	const int vl1[] = { 1 };
	qn = setRequest(q, "private", o1, l1, t1, v1, vl1, 1);
	n = process(false, q, qn, out, sizeof(out), &auth);
	CHECK(n > 0 && decode(out, n, &r, SNMP_PDU_RESPONSE) && r.status == SNMP_ERR_NOT_WRITABLE && r.index == 1);
	const uint8_t inst2[] = { 0x2B, 6, 1, 2, 1, 1, 5, 1 };
	const uint8_t *o2[] = { inst2 };
	qn = setRequest(q, "private", o2, l1, t1, v1, vl1, 1);
	n = process(false, q, qn, out, sizeof(out), &auth);
	CHECK(n > 0 && decode(out, n, &r, SNMP_PDU_RESPONSE) && r.status == SNMP_ERR_NO_CREATION);
	const uint8_t *o3[] = { SYS_NAME };
	const uint8_t *v3[] = { (const uint8_t *)"THIS NAME IS TOO LONG" };
	const int vl3[] = { 21 };
	qn = setRequest(q, "private", o3, l1, t1, v3, vl3, 1);
	n = process(false, q, qn, out, sizeof(out), &auth);
	CHECK(n > 0 && decode(out, n, &r, SNMP_PDU_RESPONSE) && r.status == SNMP_ERR_WRONG_LENGTH);
	const uint8_t gap[] = { 0x05 };	// odrAgentRfTrapInterval 5 s: below 10
	const uint8_t gapOid[] = { 0x2B, 6, 1, 4, 1, 0x81, 0xFD, 0x59, 2, 1, 7, 2, 0 };
	const uint8_t *o4[] = { gapOid };
	const int l4[] = { sizeof(gapOid) };
	const uint8_t t4[] = { BER_GAUGE32 };
	const uint8_t *v4[] = { gap };
	qn = setRequest(q, "private", o4, l4, t4, v4, vl1, 1);
	n = process(false, q, qn, out, sizeof(out), &auth);
	CHECK(n > 0 && decode(out, n, &r, SNMP_PDU_RESPONSE) && r.status == SNMP_ERR_WRONG_VALUE);

	// TruthValues and snmpEnableAuthenTraps map onto the flags
	const uint8_t authOid[] = { 0x2B, 6, 1, 2, 1, 11, 30, 0 };
	const uint8_t rfAnsOid[] = { 0x2B, 6, 1, 4, 1, 0x81, 0xFD, 0x59, 2, 1, 7, 3, 0 };
	const uint8_t one[] = { 1 }, two[] = { 2 };
	const uint8_t *o5[] = { authOid, rfAnsOid };
	const int l5[] = { sizeof(authOid), sizeof(rfAnsOid) };
	const uint8_t t5[] = { BER_INTEGER, BER_INTEGER };
	const uint8_t *v5[] = { one, two };
	const int vl5[] = { 1, 1 };
	qn = setRequest(q, "private", o5, l5, t5, v5, vl5, 2);
	n = process(false, q, qn, out, sizeof(out), &auth);
	CHECK(n > 0 && decode(out, n, &r, SNMP_PDU_RESPONSE) && r.status == 0);
	CHECK((cfg.flags & SNMP_CFG_AUTH_TRAPS) && (cfg.flags & SNMP_CFG_RF_OFF));
}

// ---- notifications ----

static void test_events(void)
{
	setup();
	snmpEvents_t e;
	snmpEventsInit(&e);

	// No manager: only the host. coldStart first
	snmpEventsDetect(&e, &data, &cfg);
	snmpEvent_t *ev = snmpEventsNext(&e, SNMP_TARGET_USB);
	CHECK(ev != NULL && ev->kind == SNMP_EV_COLD_START && ev->targets == SNMP_TARGET_USB);
	CHECK(snmpEventsNext(&e, SNMP_TARGET_RF) == NULL);
	snmpEventsDone(&e, ev, SNMP_TARGET_USB, false);
	CHECK(e.count == 0);

	// A call: start, then end with its duration; a group call on TS2
	data.dmrState = 1;
	data.callSrc = 3100002;
	data.callDst = 91;
	data.callGroup = true;
	snmpEventsDetect(&e, &data, &cfg);
	data.uptimeMs += 4500;
	data.dmrState = 0;
	snmpEventsDetect(&e, &data, &cfg);
	CHECK(e.count == 2 && e.q[0].kind == SNMP_EV_CALL_START && e.q[0].src == 3100002 && e.q[0].dst == 91 &&
	      e.q[0].group && e.q[0].slot == 2);
	CHECK(e.q[1].kind == SNMP_EV_CALL_END && e.q[1].durationMs == 4500);

	// The trap on the wire: v2c, community public, SNMPv2-Trap-PDU, sysUpTime, snmpTrapOID = odrCallStart
	uint8_t out[300];
	resp_t r;
	int n = snmpEventEncode(&e.q[0], 5, out, sizeof(out));
	CHECK(n > 0 && decode(out, n, &r, SNMP_PDU_TRAP2) && r.requestId == 5 && r.count == 6);
	CHECK(r.vb[0].tag == BER_TIMETICKS && valNum(&r, 0) == 12345);
	const uint8_t callStart[] = { 0x2B, 6, 1, 4, 1, 0x81, 0xFD, 0x59, 2, 0, 1 };
	CHECK(r.vb[1].tag == BER_OID && r.vb[1].valLen == sizeof(callStart) &&
	      memcmp(r.vb[1].val, callStart, sizeof(callStart)) == 0);
	CHECK(valNum(&r, 2) == 3100002 && valNum(&r, 3) == 91 && valNum(&r, 4) == 1 && valNum(&r, 5) == 2);
	n = snmpEventEncode(&e.q[1], 6, out, sizeof(out));
	CHECK(n > 0 && decode(out, n, &r, SNMP_PDU_TRAP2) && r.count == 7 && valNum(&r, 6) == 450);
	snmpEventsDone(&e, &e.q[0], SNMP_TARGET_USB, false);
	snmpEventsDone(&e, &e.q[0], SNMP_TARGET_USB, false);
	CHECK(e.count == 0);

	// With an RF manager: SMS, ARS, low battery go to both, calls only to the host; the queue keeps the newest 4
	cfg.rfManager = 1234567;
	data.dataEventSerial++;
	data.dataEventKind = DMR_DATA_EV_SMS_RECEIVED;
	data.dataEventId = 3100003;
	strcpy(data.smsText, "hello there");
	data.arsState = 2;
	data.batteryState = 1;
	snmpEventsDetect(&e, &data, &cfg);
	CHECK(e.count == 3 && e.q[0].kind == SNMP_EV_LOW_BATTERY && e.q[1].kind == SNMP_EV_ARS && e.q[2].kind == SNMP_EV_SMS);
	CHECK(e.q[0].targets == (SNMP_TARGET_USB | SNMP_TARGET_RF) && e.q[0].value == 7400 && e.q[1].value == 3);
	n = snmpEventEncode(&e.q[2], 7, out, sizeof(out));
	CHECK(n > 0 && decode(out, n, &r, SNMP_PDU_TRAP2) && r.count == 5 && valNum(&r, 2) == 3100003 &&
	      r.vb[4].tag == BER_OCTETS && r.vb[4].valLen == 11 && memcmp(r.vb[4].val, "hello there", 11) == 0);
	data.dmrState = 4;	// our TX
	data.callSrc = 3100001;
	snmpEventsDetect(&e, &data, &cfg);
	CHECK(e.count == 4 && e.q[3].kind == SNMP_EV_CALL_START && e.q[3].targets == SNMP_TARGET_USB);
	data.batteryState = 2;	// critical: another low battery; the queue is full, the oldest goes
	snmpEventsDetect(&e, &data, &cfg);
	CHECK(e.count == 4 && e.dropped == 1 && e.q[0].kind == SNMP_EV_ARS && e.q[3].kind == SNMP_EV_LOW_BATTERY);

	// RF: the first may go, then one per gap (60 s by default), emergency exempt
	ev = snmpEventsNext(&e, SNMP_TARGET_RF);
	CHECK(ev != NULL && ev->kind == SNMP_EV_ARS && snmpEventsRfAllowed(&e, ev, &cfg, 1000));
	snmpEventsRfSent(&e, 1000);
	snmpEventsDone(&e, ev, SNMP_TARGET_RF, false);
	ev = snmpEventsNext(&e, SNMP_TARGET_RF);
	CHECK(ev != NULL && ev->kind == SNMP_EV_SMS && !snmpEventsRfAllowed(&e, ev, &cfg, 30000));
	CHECK(snmpEventsRfAllowed(&e, ev, &cfg, 61000));
	snmpEvent_t em = { .kind = SNMP_EV_EMERGENCY };
	CHECK(snmpEventsRfAllowed(&e, &em, &cfg, 2000));
	cfg.rfTrapGapS = 10;
	CHECK(snmpEventsRfAllowed(&e, ev, &cfg, 11000));
	// Calls over RF only with the setting
	cfg.flags |= SNMP_CFG_RF_CALL_TRAPS;
	snmpEvent_t call = { .kind = SNMP_EV_CALL_END };
	snmpEventsPush(&e, &call, &cfg);
	CHECK(e.q[e.count - 1].targets == (SNMP_TARGET_USB | SNMP_TARGET_RF));
	// Off: nothing queued
	cfg.flags |= SNMP_CFG_OFF;
	int before = e.count;
	snmpEventsPush(&e, &call, &cfg);
	CHECK(e.count == before);
	cfg.flags = SNMP_CFG_USB_TRAPS_OFF;
	cfg.rfManager = 0;
	snmpEventsPush(&e, &call, &cfg);
	CHECK(e.count == before);

	// An emergency alarm heard, and an emergency call
	snmpEventsInit(&e);
	memset(&cfg, 0, sizeof(cfg));
	data.dmrState = 0;
	snmpEventsDetect(&e, &data, &cfg);
	snmpEventsDone(&e, &e.q[0], SNMP_TARGET_USB, false);	// coldStart
	data.dataEventSerial++;
	data.dataEventKind = DMR_DATA_EV_EMERGENCY_ALARM;
	data.dataEventId = 3100009;
	snmpEventsDetect(&e, &data, &cfg);
	CHECK(e.count == 1 && e.q[0].kind == SNMP_EV_EMERGENCY && e.q[0].src == 3100009);
	n = snmpEventEncode(&e.q[0], 8, out, sizeof(out));
	const uint8_t emergency[] = { 0x2B, 6, 1, 4, 1, 0x81, 0xFD, 0x59, 2, 0, 4 };
	CHECK(n > 0 && decode(out, n, &r, SNMP_PDU_TRAP2) && r.count == 5 && memcmp(r.vb[1].val, emergency, 11) == 0);
	n = snmpEventEncode(&(snmpEvent_t){ .kind = SNMP_EV_COLD_START, .atMs = 50 }, 9, out, sizeof(out));
	const uint8_t cold[] = { 0x2B, 6, 1, 6, 3, 1, 1, 5, 1 };
	CHECK(n > 0 && decode(out, n, &r, SNMP_PDU_TRAP2) && r.count == 2 && memcmp(r.vb[1].val, cold, 9) == 0 &&
	      valNum(&r, 0) == 5);
}

int main(void)
{
	test_ber();
	test_get();
	test_drops();
	test_walk();
	test_bulk();
	test_too_big();
	test_set();
	test_events();
	printf("snmpTest: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
