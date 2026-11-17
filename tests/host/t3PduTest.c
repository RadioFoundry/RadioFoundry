// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// DMR Tier III PDUs, system identity and channel plans (core/dmr/trunk/t3_pdu, t3_sys, t3_plan): the shared vector
// file (data/t3_vectors.jsonl: PDUs packed by a separate encoder written from the standard's field tables), then
// the rules that aren't a bit layout.
//
//   make -C tests/host t3PduTest && (cd tests/host && ./t3PduTest)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dmr/trunk/t3_pdu.h"
#include "dmr/trunk/t3_sys.h"
#include "dmr/trunk/t3_plan.h"
#include "dmr/fec/dmrCach.h"

static int failures;
static int checks;

#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

// --- A reader for the flat one-object-per-line JSON the vector file uses ---

static const char *jfind(const char *line, const char *key)
{
	char pat[48];
	snprintf(pat, sizeof(pat), "\"%s\":", key);
	const char *p = strstr(line, pat);
	return p ? p + strlen(pat) : NULL;
}

static bool jhas(const char *line, const char *key)
{
	return jfind(line, key) != NULL;
}

static long jint(const char *line, const char *key)
{
	const char *p = jfind(line, key);
	if (p == NULL) {
		printf("vector without \"%s\": %s", key, line);
		exit(2);
	}
	if (strncmp(p, "true", 4) == 0) {
		return 1;
	}
	if (strncmp(p, "false", 5) == 0) {
		return 0;
	}
	return strtol(p, NULL, 10);
}

static void jstr(const char *line, const char *key, char *out, size_t n)
{
	const char *p = jfind(line, key);
	out[0] = 0;
	if (p == NULL || *p != '"') {
		return;
	}
	p++;
	size_t i = 0;
	while (*p && *p != '"' && i + 1 < n) {
		out[i++] = *p++;
	}
	out[i] = 0;
}

static void unhex(const char *s, uint8_t *out, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		unsigned v = 0;
		sscanf(s + 2 * i, "%2x", &v);
		out[i] = (uint8_t)v;
	}
}

// --- Vectors ---

static uint16_t mask_of(const char *name)
{
	if (strcmp(name, "csbk") == 0) {
		return T3_CRC_MASK_CSBK;
	}
	if (strcmp(name, "mbc_hdr") == 0) {
		return T3_CRC_MASK_MBC_HDR;
	}
	return 0;
}

#define FIELD(line, name, expr) CHECK((long)(expr) == jint(line, name), "%s: %s is %ld, vector %ld", vname, name, \
	(long)(expr), jint(line, name))

static int vec_csbk(const char *line, const char *vname)
{
	char hex[32], crc[16];
	uint8_t pdu[T3_CSBK_BYTES];
	jstr(line, "hex", hex, sizeof(hex));
	jstr(line, "crc", crc, sizeof(crc));
	unhex(hex, pdu, sizeof(pdu));
	uint16_t mask = mask_of(crc);
	bool ok = jint(line, "crc_ok") != 0;
	CHECK(t3_crc_ok(pdu, mask) == ok, "%s %s: CRC check %d, vector %d", vname, hex, !ok, ok);
	if (!ok) {
		return 1;
	}
	t3_csbk_t c;
	CHECK(t3_csbk_parse(pdu, &c), "%s %s: not parsed", vname, hex);
	FIELD(line, "op", c.op);
	if (jhas(line, "lb")) {
		FIELD(line, "lb", c.lb);
	}
	switch (c.op) {
	case T3_OP_C_ALOHA:
		FIELD(line, "tsccas", c.u.aloha.tsccas);
		FIELD(line, "site_ts_sync", c.u.aloha.site_ts_sync);
		FIELD(line, "version", c.u.aloha.version);
		FIELD(line, "offset", c.u.aloha.offset);
		FIELD(line, "active_conn", c.u.aloha.active_conn);
		FIELD(line, "mask", c.u.aloha.mask);
		FIELD(line, "service_fn", c.u.aloha.service_fn);
		FIELD(line, "nrand_wait", c.u.aloha.nrand_wait);
		FIELD(line, "reg", c.u.aloha.reg);
		FIELD(line, "backoff", c.u.aloha.backoff);
		FIELD(line, "syscode", c.u.aloha.syscode);
		FIELD(line, "ms", c.u.aloha.ms);
		break;
	case T3_OP_PV_GRANT: case T3_OP_TV_GRANT: case T3_OP_BTV_GRANT: case T3_OP_PD_GRANT: case T3_OP_TD_GRANT:
		FIELD(line, "lpcn", c.u.grant.lpcn);
		FIELD(line, "slot", c.u.grant.slot);
		FIELD(line, "late_entry", c.u.grant.late_entry);
		FIELD(line, "hi_rate", c.u.grant.hi_rate);
		FIELD(line, "emergency", c.u.grant.emergency);
		FIELD(line, "offset", c.u.grant.offset);
		FIELD(line, "dst", c.u.grant.dst);
		FIELD(line, "src", c.u.grant.src);
		break;
	case T3_OP_C_BCAST:
		FIELD(line, "type", c.u.bcast.type);
		FIELD(line, "parms1", c.u.bcast.parms1);
		FIELD(line, "reg", c.u.bcast.reg);
		FIELD(line, "backoff", c.u.bcast.backoff);
		FIELD(line, "syscode", c.u.bcast.syscode);
		FIELD(line, "parms2", c.u.bcast.parms2);
		break;
	case T3_OP_C_AHOY:
		FIELD(line, "options", c.u.ahoy.options);
		FIELD(line, "kind_flag", c.u.ahoy.kind_flag);
		FIELD(line, "als", c.u.ahoy.als);
		FIELD(line, "group", c.u.ahoy.group);
		FIELD(line, "blocks", c.u.ahoy.blocks);
		FIELD(line, "kind", c.u.ahoy.kind);
		FIELD(line, "dst", c.u.ahoy.dst);
		FIELD(line, "src", c.u.ahoy.src);
		break;
	case T3_OP_C_ACKD: case T3_OP_C_ACKU: case T3_OP_P_ACKD: case T3_OP_P_ACKU:
		FIELD(line, "info", c.u.ack.info);
		FIELD(line, "reason", c.u.ack.reason);
		FIELD(line, "dst", c.u.ack.dst);
		FIELD(line, "src", c.u.ack.src);
		break;
	case T3_OP_C_RAND:
		FIELD(line, "options", c.u.rand.options);
		FIELD(line, "proxy", c.u.rand.proxy);
		FIELD(line, "extra", c.u.rand.extra);
		FIELD(line, "kind", c.u.rand.kind);
		FIELD(line, "dst", c.u.rand.dst);
		FIELD(line, "src", c.u.rand.src);
		break;
	case T3_OP_P_CLEAR:
		FIELD(line, "lpcn", c.u.clear.lpcn);
		FIELD(line, "group", c.u.clear.group);
		FIELD(line, "dst", c.u.clear.dst);
		FIELD(line, "src", c.u.clear.src);
		break;
	case T3_OP_P_PROTECT:
		FIELD(line, "kind", c.u.protect.kind);
		FIELD(line, "group", c.u.protect.group);
		FIELD(line, "dst", c.u.protect.dst);
		FIELD(line, "src", c.u.protect.src);
		break;
	case T3_OP_P_MAINT:
		FIELD(line, "kind", c.u.maint.kind);
		FIELD(line, "dst", c.u.maint.dst);
		FIELD(line, "src", c.u.maint.src);
		break;
	default:
		CHECK(false, "%s: opcode %u not checked", vname, c.op);
	}
	// And back: the encoder gives the same octets, CRC included
	uint8_t again[T3_CSBK_BYTES];
	CHECK(t3_csbk_build(&c, mask, again), "%s: not built", vname);
	CHECK(memcmp(again, pdu, sizeof(pdu)) == 0, "%s %s: built differently", vname, hex);
	return 1;
}

static int vec_cont(const char *line, const char *vname)
{
	char hex[32];
	uint8_t blk[T3_CSBK_BYTES];
	jstr(line, "hex", hex, sizeof(hex));
	unhex(hex, blk, sizeof(blk));
	CHECK(t3_crc_ok(blk, 0), "%s %s: CRC", vname, hex);
	uint8_t op = (uint8_t)jint(line, "op");
	t3_cdef_t d;
	CHECK(t3_cdef_parse(blk, op, &d), "%s %s: not parsed", vname, hex);
	FIELD(line, "cc", d.cc);
	FIELD(line, "chan", d.chan);
	FIELD(line, "tx_hz", d.tx_hz);
	FIELD(line, "rx_hz", d.rx_hz);
	uint8_t again[T3_CSBK_BYTES];
	t3_cdef_build(&d, op, again);
	CHECK(memcmp(again, blk, sizeof(blk)) == 0, "%s %s: built differently", vname, hex);
	return 1;
}

static int vec_syscode(const char *line, const char *vname)
{
	uint16_t code = (uint16_t)jint(line, "code");
	t3_syscode_t s;
	t3_syscode_split(code, &s);
	FIELD(line, "model", s.model);
	FIELD(line, "net", s.net);
	FIELD(line, "site", s.site);
	FIELD(line, "par", s.par);
	CHECK(t3_syscode_join(&s) == code, "syscode %04x: joined differently", code);
	FIELD(line, "sys_area", t3_sys_area(code, (uint8_t)jint(line, "dmrla")));
	FIELD(line, "response_check", t3_response_check(code));
	return 1;
}

static int vec_slc(const char *line, const char *vname)
{
	uint32_t slc = (uint32_t)jint(line, "slc");
	t3_sysparms_t p;
	CHECK(t3_sysparms_parse(slc, &p), "slc %07x: not parsed", slc);
	FIELD(line, "control", p.control);
	FIELD(line, "model", p.model);
	FIELD(line, "netsite", p.netsite);
	FIELD(line, "flag", p.flag);
	FIELD(line, "csc", p.csc);
	CHECK(t3_sysparms_build(&p) == slc, "slc %07x: built differently", slc);
	// through the CACH's short LC FEC and back
	uint32_t rows[4], back = 0;
	dmrShortLcEncode(slc, rows);
	CHECK(dmrShortLcDecode(rows, &back) && back == slc, "slc %07x: short LC FEC round trip", slc);
	return 1;
}

static int vec_plan(const char *line, const char *vname)
{
	t3_fixed_plan_t f = {
		.base_hz = (uint32_t)jint(line, "base_hz"), .first = (uint16_t)jint(line, "first"),
		.sep = (uint8_t)jint(line, "sep"), .tx_low = jint(line, "tx_low") != 0,
		.split_hz = (uint32_t)jint(line, "split_hz"),
	};
	t3_chan_t c;
	CHECK(t3_fixed_lookup(&f, (uint16_t)jint(line, "lcn"), &c), "plan: lcn %ld not mapped", jint(line, "lcn"));
	FIELD(line, "tx_hz", c.tx_hz);
	FIELD(line, "rx_hz", c.rx_hz);
	return 1;
}

static void vectors(const char *path)
{
	FILE *f = fopen(path, "r");
	if (f == NULL) {
		printf("FAIL: no vector file %s\n", path);
		failures++;
		return;
	}
	char line[1024];
	int n = 0;
	while (fgets(line, sizeof(line), f)) {
		char vec[16], name[24];
		jstr(line, "vec", vec, sizeof(vec));
		jstr(line, "name", name, sizeof(name));
		const char *vname = name[0] ? name : vec;
		if (strcmp(vec, "csbk") == 0 || strcmp(vec, "mbc_hdr") == 0) {
			n += vec_csbk(line, vname);
		} else if (strcmp(vec, "mbc_cont") == 0) {
			n += vec_cont(line, vname);
		} else if (strcmp(vec, "syscode") == 0) {
			n += vec_syscode(line, vname);
		} else if (strcmp(vec, "slc") == 0) {
			n += vec_slc(line, vname);
		} else if (strcmp(vec, "fixed_plan") == 0) {
			n += vec_plan(line, vname);
		} else {
			CHECK(false, "unknown vector kind \"%s\"", vec);
		}
	}
	fclose(f);
	CHECK(n >= 100, "only %d vectors", n);
	printf("%d vectors\n", n);
}

// --- Rules ---

static void rules(void)
{
	// Mask (6.2.1.1.1): 0 = everyone, 24 = one MS, n = the n low bits
	CHECK(t3_mask_match(0, 0, 1234567), "mask 0");
	CHECK(t3_mask_match(24, 1234567, 1234567) && !t3_mask_match(24, 1234566, 1234567), "mask 24");
	CHECK(t3_mask_match(4, 0x5, 0x12345) && !t3_mask_match(4, 0x4, 0x12345), "mask 4");
	CHECK(t3_mask_match(1, 1, 3) && !t3_mask_match(1, 0, 3), "mask 1");

	// Tables 6.2 and 6.3
	CHECK(t3_nwait_frames(0) == 0 && t3_nwait_frames(13) == 13 && t3_nwait_frames(14) == 15 &&
	      t3_nwait_frames(15) == 24, "NRand_Wait");
	CHECK(t3_backoff_frames(1) == 1 && t3_backoff_frames(6) == 8 && t3_backoff_frames(9) == 20 &&
	      t3_backoff_frames(15) == 100, "Backoff");

	// Duplex grants (tables 7.10, 7.14): bit 15 is Call Direction, and the payload channel always uses offset timing
	for (uint8_t op = T3_OP_PV_GRANT_DX; op <= T3_OP_PD_GRANT_DX; op++) {
		t3_csbk_t g = { .lb = true, .op = op };
		g.u.grant.lpcn = 0x123;
		g.u.grant.slot = 2;
		g.u.grant.emergency = true;
		g.u.grant.dx_calling = true;
		g.u.grant.dst = 1234567;
		g.u.grant.src = 7654321;
		uint8_t pdu[T3_CSBK_BYTES];
		CHECK(t3_csbk_build(&g, T3_CRC_MASK_CSBK, pdu), "dx grant %u built", op);
		CHECK(pdu[2] == 0x12 && pdu[3] == 0x3B && pdu[4] == 0x12 && pdu[5] == 0xD6 && pdu[6] == 0x87,
		      "dx grant %u layout %02x %02x %02x", op, pdu[2], pdu[3], pdu[4]);
		t3_csbk_t back;
		CHECK(t3_csbk_parse(pdu, &back) && back.u.grant.dx_calling && back.u.grant.offset &&
		      back.u.grant.emergency && back.u.grant.slot == 2 && back.u.grant.lpcn == 0x123 &&
		      back.u.grant.dst == 1234567 && back.u.grant.src == 7654321 && !back.u.grant.hi_rate,
		      "dx grant %u parsed", op);
		pdu[3] &= (uint8_t)~0x01u;	// Call Direction 0: the target is the called MS
		CHECK(t3_csbk_parse(pdu, &back) && !back.u.grant.dx_calling && back.u.grant.offset, "dx grant %u called", op);
	}

	// Acknowledgement classes
	CHECK(t3_ack_class(T3_RC_REG_ACCEPTED) == T3_ACK && t3_ack_class(T3_RC_MS_ACCEPTED) == T3_ACK, "ACK");
	CHECK(t3_ack_class(T3_RC_MS_NOT_REGISTERED) == T3_NACK && t3_ack_class(T3_RC_MS_NOT_SUPPORTED) == T3_NACK &&
	      t3_ack_class(T3_RC_REFUSED_UNKNOWN) == T3_NACK, "NACK");
	CHECK(t3_ack_class(T3_RC_QUEUED_BUSY) == T3_QACK && t3_ack_class(T3_RC_WAIT) == T3_WACK, "QACK / WACK");

	// TSCC authorization (6.3.2.2.2): model, net, the site list, PAR
	t3_syscode_t s = { T3_MODEL_SMALL, 5, 17, T3_PAR_A };
	uint16_t code = t3_syscode_join(&s);
	uint16_t sites[2] = { 3, 17 };
	t3_auth_t a = { T3_MODEL_SMALL, 5, NULL, 0, T3_PAR_A };
	CHECK(t3_authorize(&a, code) == T3_AUTH_OK, "auth any site");
	a.sites = sites;
	a.nsites = 2;
	CHECK(t3_authorize(&a, code) == T3_AUTH_OK, "auth listed site");
	sites[1] = 18;
	CHECK(t3_authorize(&a, code) == T3_AUTH_SITE, "auth unlisted site");
	a.nsites = 0;
	a.category = T3_PAR_B;
	CHECK(t3_authorize(&a, code) == T3_AUTH_PAR, "auth category B on a PAR A TSCC");
	s.par = T3_PAR_AB;
	CHECK(t3_authorize(&a, t3_syscode_join(&s)) == T3_AUTH_OK, "auth category B on a PAR A+B TSCC");
	a.net = 6;
	CHECK(t3_authorize(&a, code) == T3_AUTH_NET, "auth other net");
	a.model = T3_MODEL_TINY;
	CHECK(t3_authorize(&a, code) == T3_AUTH_MODEL, "auth other model");

	// SYS_AREA (figure 6.19): large network, DMRLA 6 = the 6 most significant of the 8 SITE bits
	t3_syscode_t l = { T3_MODEL_LARGE, 3, 0xB7, T3_PAR_AB };
	CHECK(t3_sys_area(t3_syscode_join(&l), 6) == 0xB7 >> 2, "SYS_AREA");
	CHECK(t3_sys_area(t3_syscode_join(&l), 12) == 0xB7, "SYS_AREA clamped to the SITE width");

	// Plans: learned first (when allowed), then the table, then the formula
	t3_learned_t learned;
	t3_learn_clear(&learned);
	t3_cdef_t cd = { 0, 0, 7, 451000000, 456000000 };
	CHECK(t3_learn_add(&learned, &cd), "learn new");
	CHECK(!t3_learn_add(&learned, &cd), "learn same");
	cd.tx_hz += 12500;
	CHECK(t3_learn_add(&learned, &cd) && learned.n == 1, "learn changed");
	static const t3_chan_t table[] = { { .lcn = 7, .cc = 3, .rx_hz = 460000000, .tx_hz = 465000000 },
					    { .lcn = 8, .cc = T3_CC_ANY, .rx_hz = 460012500, .tx_hz = 465012500 } };
	t3_plan_t plan = { { 450000000, 1, 3, false, 0 }, table, 2, true };
	t3_chan_t c;
	CHECK(t3_plan_lookup(&plan, &learned, 7, &c) && c.rx_hz == 456000000 && c.tx_hz == 451012500, "learned wins");
	plan.learn = false;
	CHECK(t3_plan_lookup(&plan, &learned, 7, &c) && c.rx_hz == 460000000 && c.cc == 3, "table");
	CHECK(t3_plan_lookup(&plan, &learned, 9, &c) && c.tx_hz == 450100000 && c.rx_hz == 450100000, "formula");
	CHECK(!t3_plan_lookup(&plan, &learned, 0, &c) && !t3_plan_lookup(&plan, &learned, 4095, &c), "LPCN 0, 4095");
	plan.fixed.base_hz = 0;
	CHECK(!t3_plan_lookup(&plan, &learned, 9, &c), "no formula");
	// a full learned list replaces the oldest
	t3_learn_clear(&learned);
	for (uint16_t i = 1; i <= T3_LEARN_MAX + 2; i++) {
		t3_cdef_t e = { 0, 0, i, 451000000u + i * 12500u, 456000000u + i * 12500u };
		t3_learn_add(&learned, &e);
	}
	plan.learn = true;
	CHECK(learned.n == T3_LEARN_MAX && !t3_plan_lookup(&plan, &learned, 1, &c) &&
	      t3_plan_lookup(&plan, &learned, T3_LEARN_MAX + 2, &c), "learned list full");

	// Opcodes this module doesn't decode still report op and FID
	uint8_t raw[T3_CSBK_BYTES] = { 0x80 | 41u, 0, 1, 2, 3, 4, 5, 6, 7, 8 };
	t3_csbk_t m;
	CHECK(!t3_csbk_parse(raw, &m) && m.op == 41u && m.raw[0] == 1, "unknown opcode");
	raw[0] = 0x80 | T3_OP_C_ALOHA;
	raw[1] = 0x10;
	CHECK(!t3_csbk_parse(raw, &m) && m.fid == 0x10, "manufacturer FID");
}

// UDT (6.5): the header's fields and CRC, and the blocks' padding and message CRC (table B.5 for ISO 8-bit text)
static void udt(void)
{
	t3_udt_t h = { .op = T3_OP_C_UDTHU, .group = true, .a = true, .format = T3_UDTF_ISO8, .dst = 0x123456u,
		       .src = 0xABCDEFu, .pad = 18, .blocks = 3, .sf = false };
	uint8_t b[T3_CSBK_BYTES];
	t3_udt_build(&h, b);
	CHECK(b[0] == 0xC0 && b[1] == 0x04 && b[2] == 0x12 && b[7] == 0xEF && b[8] == ((18 << 3) | 2) && b[9] == 27,
	      "UDT header octets %02x %02x .. %02x %02x", b[0], b[1], b[8], b[9]);
	t3_udt_t q;
	CHECK(t3_udt_parse(b, &q) && q.op == T3_OP_C_UDTHU && q.group && q.a && q.format == T3_UDTF_ISO8 &&
	      q.dst == 0x123456u && q.src == 0xABCDEFu && q.pad == 18 && q.blocks == 3 && !q.sf, "UDT header round trip");
	CHECK(!t3_crc_ok(b, T3_CRC_MASK_CSBK), "UDT header: data header mask, not the CSBK's");
	b[4] ^= 1u;
	CHECK(!t3_udt_parse(b, &q), "UDT header: bad CRC refused");

	static const struct { uint8_t len, uab, pad; } b5[] = {
		{ 1, 0, 18 }, { 10, 0, 0 }, { 11, 1, 22 }, { 22, 1, 0 }, { 23, 2, 22 }, { 34, 2, 0 }, { 35, 3, 22 },
		{ 46, 3, 0 },
	};
	uint8_t text[T3_UDT_MAX];
	for (unsigned i = 0; i < sizeof(text); i++) {
		text[i] = (uint8_t)('a' + i % 26u);
	}
	for (unsigned i = 0; i < sizeof(b5) / sizeof(b5[0]); i++) {
		uint8_t blk[T3_UDT_BLOCKS_MAX][T3_UDT_BLOCK];
		uint8_t pad = 0xEE;
		uint8_t n = t3_udt_pack(text, b5[i].len, 0xFF, blk, &pad);
		CHECK(n == b5[i].uab + 1u && pad == b5[i].pad, "%u characters: %u blocks, pad %u", b5[i].len, n, pad);
		CHECK(t3_udt_crc_ok((const uint8_t (*)[T3_UDT_BLOCK])blk, n), "%u characters: CRC", b5[i].len);
		const uint8_t *flat = &blk[0][0];
		CHECK(memcmp(flat, text, b5[i].len) == 0 && (b5[i].pad == 0 || flat[b5[i].len] == 0xFF),
		      "%u characters: data, then 0xFF", b5[i].len);
		blk[0][0] ^= 0x80u;
		CHECK(!t3_udt_crc_ok((const uint8_t (*)[T3_UDT_BLOCK])blk, n), "%u characters: corrupt CRC", b5[i].len);
	}
	uint8_t blk[T3_UDT_BLOCKS_MAX][T3_UDT_BLOCK];
	uint8_t pad;
	CHECK(t3_udt_pack(text, 0, 0, blk, &pad) == 0 && t3_udt_pack(text, 47, 0, blk, &pad) == 0, "UDT: 1-46 octets");
}

// MS authentication (6.4.8): the test vectors of tables 6.15 and 6.16
static void auth(void)
{
	uint8_t k[T3_AUTH_KEY_LEN];
	for (int i = 0; i < T3_AUTH_KEY_LEN; i++) {
		k[i] = (uint8_t)(i + 1);
	}
	uint32_t r = t3_auth_response(k, 0x7A17C0u, 0);
	CHECK(r == 0xE8CA1Du, "response %06x without PSN", (unsigned)r);
	r = t3_auth_response(k, 0x7A17C0u, 0x010203u);
	CHECK(r == 0xE9C81Eu, "response %06x with PSN", (unsigned)r);
}

int main(int argc, char **argv)
{
	vectors(argc > 1 ? argv[1] : "data/t3_vectors.jsonl");
	rules();
	udt();
	auth();
	printf("t3PduTest: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
