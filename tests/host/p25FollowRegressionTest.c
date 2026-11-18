// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Copyright (c) 2026 tylerwatt12
// Regression cases at the follower's actual block, timing, tune and inbound-send interfaces.
#include <stdio.h>
#include <string.h>

#include "p25/p25_follow.h"

static unsigned checks, failures;
#define CHECK(c, text) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, text); } } while (0)

typedef struct {
	p25_follow_t f;
	p25_follow_cfg_t cfg;
	p25_follow_cc_t cc;
	p25_follow_iden_t plan[P25_FOLLOW_IDENS];
	p25_isp_t last_request;
	uint32_t now, sent_hz;
	unsigned sends, fallbacks, tunes;
	bool request_valid;
} fixture_t;

static void tuned(uint32_t hz, uint8_t what, uint32_t id, void *ctx)
{
	fixture_t *x = ctx;
	(void)hz; (void)what; (void)id;
	x->tunes++;
	p25_follow_retuned(&x->f, x->now);
}

static bool sent(uint32_t hz, const uint8_t *block, void *ctx)
{
	fixture_t *x = ctx;
	x->request_valid = p25_isp_parse(block, &x->last_request);
	x->sent_hz = hz;
	x->sends++;
	return true;
}

static void fallback(void *ctx)
{
	((fixture_t *)ctx)->fallbacks++;
}

static const p25_follow_hooks_t hooks = { .tune = tuned, .send = sent, .fallback = fallback };

static void feed(fixture_t *x, const p25_tsbk_t *message)
{
	uint8_t b[P25_TSBK_BYTES];
	p25_tsbk_make(message, b);
	p25_follow_tsbk(&x->f, b, 0x293, ++x->now);
}

static void network(fixture_t *x, uint16_t sysid)
{
	p25_tsbk_t m = { .kind = P25_TSBK_NET, .last = true };
	m.u.net.wacn = 0xABCDE;
	m.u.net.sysid = sysid;
	m.u.net.ch = 0x1000;
	feed(x, &m);
}

static void site(fixture_t *x, uint16_t sysid)
{
	p25_tsbk_t m = { .kind = P25_TSBK_RFSS, .last = true };
	m.u.site.sysid = sysid;
	m.u.site.rfss = 1;
	m.u.site.site = 2;
	m.u.site.ch = 0x1000;
	feed(x, &m);
}

static void identifier(fixture_t *x, bool unavailable, int16_t offset)
{
	p25_tsbk_t m = { .kind = P25_TSBK_IDEN, .last = true };
	m.u.iden = (p25_iden_air_t){ .id = 1, .form = P25_OP_IDEN_UP, .base = 86600000,
		.spacing = 100, .slots = 1, .tx_off = offset, .no_tx_offset = unavailable };
	feed(x, &m);
}

enum { WILDCARD_WACN = 1u, WILDCARD_SYSID = 2u, WILDCARD_BOTH = 3u };

static void begin(fixture_t *x, bool programmed, int32_t offset, uint32_t explicit_tx, unsigned wildcard)
{
	memset(x, 0, sizeof *x);
	x->now = 1;
	x->cc = (p25_follow_cc_t){ .hz = 433000000, .tx_hz = explicit_tx, .chan = 0x1000 };
	x->plan[1] = (p25_follow_iden_t){ .base_hz = 433000000, .spacing_hz = 12500, .tx_off_hz = offset };
	x->cfg = (p25_follow_cfg_t){ .wacn = wildcard & WILDCARD_WACN ? P25_FOLLOW_WACN_ANY : 0xABCDE,
		.sysid = wildcard & WILDCARD_SYSID ? P25_FOLLOW_SYSID_ANY : 0x123, .nac = 0x293,
		.unit_id = 0x123456, .talk_group = 0x2345, .flags = P25_FOLLOW_TX | P25_FOLLOW_NO_REG,
		.ncc = 1, .cc = &x->cc, .iden = x->plan, .iden_set = programmed ? 2 : 0 };
	p25_follow_begin(&x->f, &x->cfg, &hooks, x, x->now);
	network(x, 0x123);
	site(x, 0x123);
	CHECK(x->f.view.state == P25_FOLLOW_CONTROL, "matching network and RFSS acquire control");
}

static void request(fixture_t *x)
{
	p25_follow_ptt(&x->f, false, x->now);
	p25_follow_ptt(&x->f, true, x->now);
	x->now += 200;
	p25_follow_tick(&x->f, x->now);
}

static void grant(fixture_t *x)
{
	p25_tsbk_t m = { .kind = P25_TSBK_GRANT, .opcode = P25_OP_GRP_V_GRANT, .last = true };
	m.u.grant = (p25_grant_t){ .has_svc = true, .svc = 4, .ch = 0x100A, .tg = 0x2345, .src = 0x112233 };
	feed(x, &m);
}

static void testUnavailableOffsets(void)
{
	fixture_t x;
	begin(&x, false, 0, 0, false);
	identifier(&x, true, 0);
	request(&x);
	CHECK(x.sends == 0 && x.f.view.reg == P25_REG_NO_TX, "unavailable learned offset never sends on downlink");
	identifier(&x, false, 20);
	request(&x);
	CHECK(x.sends == 1 && x.sent_hz == 438000000, "later valid offset re-enables requests without re-hunt");

	begin(&x, false, 0, 0, false);
	identifier(&x, false, 0);
	request(&x);
	CHECK(x.sends == 1 && x.sent_hz == 433000000, "known zero offset remains legitimate simplex");

	begin(&x, false, 0, 438000000, false);
	identifier(&x, true, 0);
	request(&x);
	CHECK(x.sends == 1 && x.sent_hz == 438000000, "explicit control uplink survives unavailable implicit offset");

	begin(&x, true, 5000000, 0, false);
	identifier(&x, true, 0);
	request(&x);
	CHECK(x.sends == 1 && x.sent_hz == 438000000, "unavailable advertisement preserves configured offset");

	begin(&x, true, 0, 0, false);
	identifier(&x, false, 20);
	identifier(&x, true, 0);
	request(&x);
	CHECK(x.sends == 0, "unavailable advertisement revokes OTA offset in programmed receive plan");
	identifier(&x, false, 0);
	request(&x);
	CHECK(x.sends == 1 && x.sent_hz == 433000000, "programmed receive plan can learn known simplex afterward");
}

static void testSiteIdentity(void)
{
	fixture_t x;
	for (unsigned wildcard = 0; wildcard < 2; wildcard++) {
		begin(&x, true, 5000000, 0, wildcard ? WILDCARD_BOTH : 0);
		site(&x, 0x456);
		grant(&x);
		CHECK(x.f.view.state == P25_FOLLOW_HUNT && x.f.view.follows == 0 && x.f.view.wrong_sys == 1,
		      "wrong-system RFSS invalidates existing network proof and prevents stale grant follow");
		request(&x);
		CHECK(x.sends == 0, "identity conflict prevents requests even with explicit configured uplink");
		site(&x, 0x123);
		grant(&x);
		CHECK(x.f.view.follows == 0, "RFSS alone cannot reacquire after conflicting identity");
		network(&x, 0x123);
		site(&x, 0x123);
		grant(&x);
		CHECK(x.f.view.state == P25_FOLLOW_VOICE && x.f.view.follows == 1,
		      "fresh matching identity permits normal grant following again");
	}
	begin(&x, true, 5000000, 0, WILDCARD_BOTH);
	site(&x, 0x456);
	grant(&x);
	CHECK(x.f.view.state == P25_FOLLOW_HUNT && x.f.view.follows == 0,
	      "a different RFSS cannot immediately reuse retained wildcard identity");
	network(&x, 0x456);
	site(&x, 0x456);
	grant(&x);
	CHECK(x.f.view.state == P25_FOLLOW_VOICE && x.f.view.sysid == 0x456,
	      "fresh NET can admit a different valid system under wildcard selection");
	begin(&x, true, 5000000, 0, false);
	x.cfg.fallback_ms = 500;
	for (unsigned i = 0; i < 30; i++) {
		site(&x, 0x456);
		x.now += 50;
		p25_follow_tick(&x.f, x.now);
	}
	CHECK(x.f.view.state == P25_FOLLOW_HUNT && x.f.view.follows == 0 && x.fallbacks == 1,
	      "continuous conflicting RFSS cannot slide hunt fallback deadline");
}

static void testRfssBeforeNetwork(void)
{
	fixture_t x;
	for (unsigned fixed_wacn = 0; fixed_wacn < 2; fixed_wacn++) {
		begin(&x, true, 5000000, 0, fixed_wacn ? WILDCARD_SYSID : WILDCARD_BOTH);
		x.now += P25_FOLLOW_CC_LOST_MS + 1;
		p25_follow_tick(&x.f, x.now);
		CHECK(x.f.view.state == P25_FOLLOW_HUNT && x.f.view.wacn == 0xABCDE && x.f.view.sysid == 0x123,
		      "control loss retains the prior identity while hunting");
		unsigned tunes = x.tunes, wrong = x.f.view.wrong_sys;
		uint8_t cc = x.f.view.cc;
		site(&x, 0x456);
		CHECK(x.f.view.wrong_sys == wrong && x.tunes == tunes && x.f.view.cc == cc,
		      "acceptable RFSS before NET neither rejects nor leaves the current candidate");
		CHECK(x.f.view.state == (fixed_wacn ? P25_FOLLOW_HUNT : P25_FOLLOW_CONTROL),
		      "RFSS settles any WACN, but a configured WACN still needs NET");
		if (fixed_wacn) {
			grant(&x);
			request(&x);
			CHECK(x.f.view.follows == 0 && x.sends == 0,
			      "retained identity cannot admit grants or access while configured WACN remains unproved");
		}
		network(&x, 0x456);
		CHECK(x.f.view.state == P25_FOLLOW_CONTROL && x.f.view.sysid == 0x456 &&
		      x.f.view.wacn == 0xABCDE && x.f.view.wrong_sys == wrong && x.tunes == tunes,
		      "later acceptable NET acquires the same candidate and updates the learned identity");
	}
	// A configured system ID remains authoritative during HUNT, even when WACN is wildcard.
	begin(&x, true, 5000000, 0, WILDCARD_WACN);
	x.now += P25_FOLLOW_CC_LOST_MS + 1;
	p25_follow_tick(&x.f, x.now);
	unsigned wrong = x.f.view.wrong_sys;
	site(&x, 0x456);
	grant(&x);
	request(&x);
	CHECK(x.f.view.state == P25_FOLLOW_HUNT && x.f.view.wrong_sys == wrong + 1 &&
	      x.f.view.follows == 0 && x.sends == 0,
	      "HUNT still rejects configured system mismatch and blocks grants and access");
}

static void testWarmHuntRegistration(void)
{
	fixture_t x;
	memset(&x, 0, sizeof x);
	x.now = 1;
	x.cc = (p25_follow_cc_t){ .hz = 433000000, .tx_hz = 438000000, .chan = 0x1000 };
	x.cfg = (p25_follow_cfg_t){ .wacn = P25_FOLLOW_WACN_ANY, .sysid = 0x123, .nac = 0x293,
		.unit_id = 0x123456, .talk_group = 0x2345, .flags = P25_FOLLOW_TX, .ncc = 1, .cc = &x.cc };
	p25_follow_begin(&x.f, &x.cfg, &hooks, &x, x.now);
	network(&x, 0x123);
	site(&x, 0x123);
	x.now = 500;
	p25_follow_tick(&x.f, x.now);
	x.now = 800;
	p25_follow_tick(&x.f, x.now);
	CHECK(x.sends == 1 && x.request_valid && x.last_request.opcode == P25_ISP_U_REG_REQ &&
	      x.last_request.wacn == 0xABCDE, "initial registration uses the observed network identity");
	// Lose the control channel while registration is waiting. RFSS can reacquire it before the next NET.
	x.now = 1900;
	p25_follow_tick(&x.f, x.now);
	CHECK(x.f.view.state == P25_FOLLOW_HUNT && x.f.view.wacn == 0xABCDE,
	      "warm hunt retains the last network identity independently of active candidate proof");
	site(&x, 0x123);
	CHECK(x.f.view.state == P25_FOLLOW_CONTROL, "same RFSS can reacquire a wildcard-WACN selection");
	x.now = 2100;
	p25_follow_tick(&x.f, x.now);
	x.now = 2400;
	p25_follow_tick(&x.f, x.now);
	CHECK(x.sends == 2 && x.request_valid && x.last_request.opcode == P25_ISP_U_REG_REQ &&
	      x.last_request.wacn == 0xABCDE && x.last_request.sysid == 0x123,
	      "registration after warm hunt preserves the network identity instead of substituting zero");
}

static void testEncryptedLinkControl(void)
{
	fixture_t x;
	begin(&x, true, 5000000, 0, false);
	x.cfg.flags |= P25_FOLLOW_SKIP_ENC;
	grant(&x);
	p25_call_t call = { .have_lc = true, .lc = { .lcf = P25_LCF_GROUP, .tgid = 0x2345, .encrypted = true } };
	p25_follow_call(&x.f, &call, ++x.now);
	CHECK(x.f.view.state == P25_FOLLOW_CONTROL && x.f.view.enc_skip == 1 && x.f.view.enc,
	      "protected LC skips encrypted late entry before algorithm sync");
}

int main(void)
{
	testUnavailableOffsets();
	testSiteIdentity();
	testRfssBeforeNetwork();
	testWarmHuntRegistration();
	testEncryptedLinkControl();
	printf("P25 follower regressions: %u checks, %u failures\n", checks, failures);
	return failures != 0;
}
