// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The DMR Tier III mobile station engine, receive side (core/dmr/trunk/t3_ms): hunting, confirming the TSCC,
// following grants, returning, T_Nosig, learned channels, adjacent sites and Vote Now, leaving a weak TSCC, against
// PDUs built from the standard's layouts.
//
//   make -C tests/host t3MsTest && (cd tests/host && ./t3MsTest)
#include <stdio.h>
#include <string.h>
#include "dmr/trunk/t3_ms.h"

static int failures;
static int checks;

#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

#define OWN		2345678u
#define TG_A		5278505u
#define TG_B		5310449u
#define TG_OTHER	5275843u
#define CC		1

// The fixed plan of a system whose MS receives at 450.000 + (CHAN-1) x 12.5 kHz and transmits 5 MHz above
#define RX(lcn)		(450000000u + ((uint32_t)(lcn) - 1u) * 12500u)

static uint32_t T;		// the engine's clock, ms
static t3_act_t last;		// the last action the engine asked for
static int nact;

static uint16_t ours, theirs;	// system codes

static void poll_all(t3_ms_t *ms)
{
	t3_act_t a;
	while (t3_ms_poll(ms, T, &a)) {
		last = a;
		nact++;
	}
}

static void adv(t3_ms_t *ms, uint32_t ms_)
{
	for (uint32_t t = 0; t < ms_; t += 10) {
		T += 10;
		poll_all(ms);
	}
}

static void rx(t3_ms_t *ms, uint8_t slot, uint8_t dt, uint8_t cc, const uint8_t d[T3_CSBK_BYTES])
{
	t3_ms_rx(ms, T, slot, dt, cc, d);
	poll_all(ms);
}

static void send(t3_ms_t *ms, uint8_t slot, uint8_t cc, const t3_csbk_t *p)
{
	uint8_t d[T3_CSBK_BYTES];
	CHECK(t3_csbk_build(p, T3_CRC_MASK_CSBK, d), "build op %u", p->op);
	rx(ms, slot, T3_DT_CSBK, cc, d);
}

static void send_mbc(t3_ms_t *ms, uint8_t slot, const t3_csbk_t *hdr, const t3_cdef_t *c)
{
	uint8_t d[T3_CSBK_BYTES];
	CHECK(t3_csbk_build(hdr, T3_CRC_MASK_MBC_HDR, d), "build header op %u", hdr->op);
	rx(ms, slot, T3_DT_MBC_HDR, CC, d);
	adv(ms, 60);
	t3_cdef_build(c, hdr->op, d);
	rx(ms, slot, T3_DT_MBC_CONT, CC, d);
}

static void aloha(t3_ms_t *ms, uint8_t cc, uint16_t syscode, bool reg)
{
	t3_csbk_t p = { .lb = true, .op = T3_OP_C_ALOHA };
	p.u.aloha.syscode = syscode;
	p.u.aloha.reg = reg;
	p.u.aloha.nrand_wait = 6;
	p.u.aloha.backoff = 6;
	p.u.aloha.version = 2;
	send(ms, 1, cc, &p);
}

// The TSCC's slot 1 sends an Aloha every 60 ms for this long
static void camp(t3_ms_t *ms, uint32_t dur)
{
	for (uint32_t t = 0; t < dur; t += 60) {
		adv(ms, 60);
		aloha(ms, CC, ours, true);
	}
}

static t3_csbk_t grant(uint8_t op, uint16_t lpcn, uint8_t slot, uint32_t dst, uint32_t src)
{
	t3_csbk_t p = { .lb = true, .op = op };
	p.u.grant.lpcn = lpcn;
	p.u.grant.slot = slot;
	p.u.grant.dst = dst;
	p.u.grant.src = src;
	return p;
}

static t3_cfg_t cfg;

static void setup(void)
{
	memset(&cfg, 0, sizeof(cfg));
	t3_syscode_t s = { T3_MODEL_LARGE, 12, 0, T3_PAR_AB };
	ours = t3_syscode_join(&s);
	s.net = 13;
	theirs = t3_syscode_join(&s);
	cfg.own_id = OWN;
	cfg.auth.model = T3_MODEL_LARGE;
	cfg.auth.net = 12;
	cfg.auth.category = T3_PAR_A;
	cfg.plan.fixed = (t3_fixed_plan_t){ 455000000u, 1, 3, false, 5000000u };
	cfg.plan.learn = true;
	// silent, another system, ours
	cfg.ctrl[0] = (t3_chan_t){ .lcn = 0, .cc = T3_CC_ANY, .rx_hz = 452000000u, .tx_hz = 457000000u };
	cfg.ctrl[1] = (t3_chan_t){ .lcn = 0, .cc = T3_CC_ANY, .rx_hz = 453000000u, .tx_hz = 458000000u };
	cfg.ctrl[2] = (t3_chan_t){ .lcn = 154, .cc = T3_CC_ANY, .rx_hz = RX(154), .tx_hz = RX(154) + 5000000u };
	cfg.nctrl = 3;
	cfg.tg[0] = (t3_tg_t){ TG_A, T3_TG_LISTEN | T3_TG_TALK };
	cfg.tg[1] = (t3_tg_t){ TG_B, T3_TG_LISTEN };
	cfg.tg[2] = (t3_tg_t){ TG_OTHER, 0 };
	cfg.ntg = 3;
	cfg.opts = T3_OPT_LISTEN_ONLY | T3_OPT_PRIVATE | T3_OPT_ALLCALL;
	T = 1000;
	nact = 0;
	memset(&last, 0, sizeof(last));
}

// Hunt to the third candidate and camp there
static void hunt_to_ours(t3_ms_t *ms)
{
	t3_ms_start(ms, &cfg, T);
	poll_all(ms);
	adv(ms, T3_DWELL_MS + 20);		// candidate 0 hears nothing
	CHECK(last.chan.rx_hz == 453000000u, "second candidate, got %u", last.chan.rx_hz);
	adv(ms, 100);
	aloha(ms, CC, theirs, true);		// candidate 1 is another network: next at once
	CHECK(last.chan.rx_hz == RX(154) && last.slot == 0 && !last.payload, "third candidate, got %u",
	      last.chan.rx_hz);
	camp(ms, 300);
}

static void test_hunt(void)
{
	setup();
	t3_ms_t ms;
	t3_ms_start(&ms, &cfg, T);
	poll_all(&ms);
	CHECK(nact == 1 && last.chan.rx_hz == 452000000u, "first candidate");
	CHECK(ms.state == T3_HUNT, "hunting");
	hunt_to_ours(&ms);
	CHECK(ms.state == T3_CAMPED && ms.confirmed, "camped: %s", t3_state_name(ms.state));
	CHECK(ms.tscc.rx_hz == RX(154) && ms.tscc.cc == CC, "TSCC %u cc %u", ms.tscc.rx_hz, ms.tscc.cc);
	CHECK(ms.syscode == ours && ms.aloha.nrand_wait == 6 && ms.aloha.version == 2, "Aloha parameters");
	CHECK(ms.reg == T3_REG_LISTEN, "listen-only with Reg = 1: %s", t3_reg_name(ms.reg));
	CHECK(ms.st.foreign == 1 && ms.st.hunts == 1, "foreign %u hunts %u", ms.st.foreign, ms.st.hunts);
	CHECK(ms.tscc_slots == 1, "TSCC on TS1");

	// The candidate list wraps when nothing answers
	setup();
	t3_ms_start(&ms, &cfg, T);
	poll_all(&ms);
	adv(&ms, 3 * (T3_DWELL_MS + 10));
	CHECK(ms.state == T3_HUNT && last.chan.rx_hz == 452000000u, "wrapped to the first, at %u", last.chan.rx_hz);

	// Without listen-only, Reg = 1 means registration is needed; Reg = 0 that it isn't
	setup();
	cfg.opts &= (uint16_t)~T3_OPT_LISTEN_ONLY;
	hunt_to_ours(&ms);
	CHECK(ms.reg == T3_REG_NEEDED, "reg needed: %s", t3_reg_name(ms.reg));
	adv(&ms, 60);
	aloha(&ms, CC, ours, false);
	CHECK(ms.reg == T3_REG_NOT_NEEDED, "reg not needed: %s", t3_reg_name(ms.reg));
}

static void test_settle_crc_cc(void)
{
	setup();
	t3_ms_t ms;
	t3_ms_start(&ms, &cfg, T);
	poll_all(&ms);
	// What the modem still had from the old channel right after a retune
	aloha(&ms, CC, ours, true);
	CHECK(ms.state == T3_HUNT, "an Aloha inside the settle time doesn't confirm");
	adv(&ms, T3_SETTLE_MS);
	uint8_t d[T3_CSBK_BYTES];
	t3_csbk_t p = { .lb = true, .op = T3_OP_C_ALOHA };
	p.u.aloha.syscode = ours;
	t3_csbk_build(&p, T3_CRC_MASK_CSBK, d);
	d[5] ^= 0x01;
	rx(&ms, 1, T3_DT_CSBK, CC, d);
	CHECK(ms.state == T3_HUNT && ms.st.crc_bad == 1, "a bad CRC is dropped");
	// An MBC header mask on a CSBK fails too
	t3_csbk_build(&p, T3_CRC_MASK_CSBK, d);
	rx(&ms, 1, T3_DT_MBC_HDR, CC, d);
	CHECK(ms.state == T3_HUNT && ms.st.crc_bad == 2, "wrong mask is dropped");
	aloha(&ms, CC, ours, true);
	CHECK(ms.state == T3_CAMPED && ms.tscc.rx_hz == 452000000u, "good Aloha confirms");
	// Camped: another color code is another channel
	t3_csbk_t g = grant(T3_OP_TV_GRANT, 159, 1, TG_A, 1234);
	adv(&ms, 60);
	send(&ms, 1, CC + 1, &g);
	CHECK(ms.state == T3_CAMPED && ms.st.grants == 0, "a grant in another color code is ignored");
	// A programmed color code on a candidate filters while hunting
	setup();
	cfg.ctrl[0].cc = 7;
	t3_ms_start(&ms, &cfg, T);
	poll_all(&ms);
	adv(&ms, 100);
	aloha(&ms, CC, ours, true);
	CHECK(ms.state == T3_HUNT, "candidate's color code differs");
}

// Accept invalid CRC (T3_OPT_ACCEPT_BAD_CRC): a PDU that fails its CRC is taken when the same bytes failed recently,
// and the radio is listen-only until one passes
static void test_accept_bad_crc(void)
{
	setup();
	cfg.opts = (uint16_t)((cfg.opts & ~T3_OPT_LISTEN_ONLY) | T3_OPT_ACCEPT_BAD_CRC);
	t3_ms_t ms;
	t3_ms_start(&ms, &cfg, T);
	poll_all(&ms);
	adv(&ms, T3_SETTLE_MS);
	uint8_t d[T3_CSBK_BYTES];
	t3_csbk_t p = { .lb = true, .op = T3_OP_C_ALOHA };
	p.u.aloha.syscode = ours;
	p.u.aloha.reg = true;
	t3_csbk_build(&p, T3_CRC_MASK_CSBK, d);
	d[10] ^= 0x01;	// the CRC itself: the content stays right
	rx(&ms, 1, T3_DT_CSBK, CC, d);
	CHECK(ms.state == T3_HUNT && ms.st.crc_bad == 1 && ms.st.crc_taken == 0, "the first one is dropped: %s",
	      t3_state_name(ms.state));
	adv(&ms, 60);
	rx(&ms, 1, T3_DT_CSBK, CC, d);
	CHECK(ms.state == T3_CAMPED && ms.st.crc_taken == 1 && ms.st.crc_bad == 1, "the same one again is taken: %s",
	      t3_state_name(ms.state));
	CHECK(ms.unverified && ms.reg == T3_REG_LISTEN, "unverified: listen-only, reg %s", t3_reg_name(ms.reg));
	adv(&ms, 60);
	aloha(&ms, CC, ours, true);
	CHECK(!ms.unverified && ms.reg == T3_REG_NEEDED, "a good CRC: may register, reg %s", t3_reg_name(ms.reg));

	// An MBC whose header failed is taken when the whole MBC repeats, unverified though its continuation's CRC passes
	t3_csbk_t h = { .lb = false, .op = T3_OP_C_BCAST };
	h.u.bcast.type = T3_ANN_CHAN_FREQ;
	h.u.bcast.syscode = ours;
	h.u.bcast.parms2 = 2000;
	t3_cdef_t c = { 0, 0, 2000, 465000000u, 460000000u };
	for (int i = 0; i < 2; i++) {
		adv(&ms, 60);
		t3_csbk_build(&h, T3_CRC_MASK_MBC_HDR, d);
		d[11] ^= 0x01;
		rx(&ms, 1, T3_DT_MBC_HDR, CC, d);
		adv(&ms, 60);
		t3_cdef_build(&c, h.op, d);
		rx(&ms, 1, T3_DT_MBC_CONT, CC, d);
		CHECK(ms.unverified == (i == 1) && ms.st.crc_taken == 1u + (uint32_t)i && ms.st.crc_bad == 2,
		      "MBC %d: taken only the second time", i);
	}
	adv(&ms, 60);
	aloha(&ms, CC, ours, true);
	CHECK(!ms.unverified, "verified again");

	// Without the option the same CSBK is dropped every time
	setup();
	t3_ms_start(&ms, &cfg, T);
	poll_all(&ms);
	adv(&ms, T3_SETTLE_MS);
	t3_csbk_build(&p, T3_CRC_MASK_CSBK, d);
	d[10] ^= 0x01;
	rx(&ms, 1, T3_DT_CSBK, CC, d);
	adv(&ms, 60);
	rx(&ms, 1, T3_DT_CSBK, CC, d);
	CHECK(ms.state == T3_HUNT && ms.st.crc_bad == 2 && ms.st.crc_taken == 0 && !ms.unverified, "dropped");
}

static void test_follow(void)
{
	setup();
	t3_ms_t ms;
	hunt_to_ours(&ms);
	int n0 = nact;

	// Not ours: another talkgroup, a talkgroup we hold without LISTEN, a private call or data to someone else, data to
	// a talkgroup we don't follow
	t3_csbk_t g = grant(T3_OP_TV_GRANT, 159, 1, 9999u, 1234u);
	send(&ms, 1, CC, &g);
	g = grant(T3_OP_TV_GRANT, 159, 1, TG_OTHER, 1234u);
	send(&ms, 1, CC, &g);
	g = grant(T3_OP_PV_GRANT, 159, 1, 1111u, 1234u);
	send(&ms, 1, CC, &g);
	g = grant(T3_OP_PD_GRANT, 159, 1, 1111u, 1234u);
	send(&ms, 1, CC, &g);
	g = grant(T3_OP_TD_GRANT, 159, 1, TG_OTHER, 1234u);
	send(&ms, 1, CC, &g);
	CHECK(ms.state == T3_CAMPED && nact == n0, "no follow: %s", t3_state_name(ms.state));
	CHECK(ms.st.grants == 5, "grants %u", ms.st.grants);

	// Ours: TS2 of channel 159
	g = grant(T3_OP_TV_GRANT, 159, 2, TG_A, 1234u);
	send(&ms, 1, CC, &g);
	CHECK(ms.state == T3_FOLLOW, "follows: %s", t3_state_name(ms.state));
	CHECK(last.type == T3_ACT_TUNE && last.payload && last.slot == 2 && last.chan.rx_hz == RX(159) &&
	      last.chan.tx_hz == RX(159) + 5000000u && last.chan.cc == CC,
	      "tune to 159 TS2: %u slot %u cc %u", last.chan.rx_hz, last.slot, last.chan.cc);
	CHECK(ms.call.dst == TG_A && ms.call.src == 1234u && ms.call.group, "call");

	// Voice on the slot keeps it
	for (int i = 0; i < 100; i++) {
		adv(&ms, 60);
		t3_ms_activity(&ms, T, 2);
	}
	CHECK(ms.state == T3_FOLLOW, "voice keeps the call");
	// The other slot's traffic doesn't
	for (int i = 0; i < 50; i++) {
		adv(&ms, 60);
		t3_ms_activity(&ms, T, 1);
	}
	CHECK(ms.state == T3_CAMPED && last.chan.rx_hz == RX(154) && !last.payload && last.slot == 0,
	      "inactivity returns: %s", t3_state_name(ms.state));
	CHECK(ms.st.returns == 1, "returns %u", ms.st.returns);

	// The TSCC repeats the grant for late entry: not followed straight back
	adv(&ms, 100);
	aloha(&ms, CC, ours, true);
	g = grant(T3_OP_TV_GRANT, 159, 2, TG_A, 1234u);
	g.u.grant.late_entry = true;
	send(&ms, 1, CC, &g);
	CHECK(ms.state == T3_CAMPED, "late-entry re-grant just after leaving is ignored");
	camp(&ms, T3_REGRANT_MS);
	send(&ms, 1, CC, &g);
	CHECK(ms.state == T3_FOLLOW, "re-grant later is followed");

	// P_CLEAR for the call on its slot: back now
	adv(&ms, 200);
	t3_csbk_t c = { .lb = true, .op = T3_OP_P_CLEAR };
	c.u.clear.group = true;
	c.u.clear.dst = TG_OTHER;
	send(&ms, 2, CC, &c);
	CHECK(ms.state == T3_FOLLOW, "P_CLEAR for another call is ignored");
	c.u.clear.dst = TG_A;
	send(&ms, 1, CC, &c);
	CHECK(ms.state == T3_FOLLOW, "P_CLEAR on the other slot is ignored");
	send(&ms, 2, CC, &c);
	CHECK(ms.state == T3_CAMPED && last.chan.rx_hz == RX(154), "P_CLEAR returns");

	// A private call to the radio
	camp(&ms, 300);
	g = grant(T3_OP_PV_GRANT, 895, 1, OWN, 4321u);
	send(&ms, 1, CC, &g);
	CHECK(ms.state == T3_FOLLOW && !ms.call.group && last.chan.rx_hz == RX(895), "private call followed");
	t3_csbk_t m = { .lb = true, .op = T3_OP_P_MAINT };
	m.u.maint.kind = T3_MAINT_DISCON;
	m.u.maint.dst = T3_ID_TSI;	// table 7.32: to the TS, from the radio ending the call
	m.u.maint.src = 1234u;
	adv(&ms, 100);
	send(&ms, 1, CC, &m);
	CHECK(ms.state == T3_FOLLOW, "P_MAINT from a stranger is ignored");
	m.u.maint.src = 4321u;
	send(&ms, 1, CC, &m);
	CHECK(ms.state == T3_CAMPED, "P_MAINT disconnect returns");

	// P_CLEAR to ALLMSI clears whatever we follow
	camp(&ms, 300);
	g = grant(T3_OP_TV_GRANT, 969, 1, TG_B, 4321u);
	send(&ms, 1, CC, &g);
	CHECK(ms.state == T3_FOLLOW, "TG_B followed");
	adv(&ms, 100);
	c.u.clear.dst = T3_ID_ALLMSI;
	c.u.clear.group = false;
	send(&ms, 1, CC, &c);
	CHECK(ms.state == T3_CAMPED, "P_CLEAR to ALLMSI returns");

	// All-call
	camp(&ms, 300);
	g = grant(T3_OP_TV_GRANT, 999, 2, T3_ID_ALLMSID, 4321u);
	send(&ms, 1, CC, &g);
	CHECK(ms.state == T3_FOLLOW, "all-call followed");
	adv(&ms, T3_INACTIVE_MS + 100);
	CHECK(ms.state == T3_CAMPED, "all-call ends");
}

static void test_payload(void)
{
	setup();
	t3_ms_t ms;
	hunt_to_ours(&ms);
	t3_csbk_t g = grant(T3_OP_TV_GRANT, 1093, 1, TG_A, 1234u);
	send(&ms, 1, CC, &g);
	CHECK(ms.state == T3_FOLLOW, "follows");
	adv(&ms, 100);

	// P_PROTECT for the call
	t3_csbk_t p = { .lb = true, .op = T3_OP_P_PROTECT };
	p.u.protect.kind = T3_PROTECT_DIS_PTT;
	p.u.protect.group = true;
	p.u.protect.dst = TG_A;
	send(&ms, 1, CC, &p);
	CHECK(ms.protect_dis, "DIS_PTT");
	p.u.protect.kind = T3_PROTECT_EN_PTT;
	send(&ms, 1, CC, &p);
	CHECK(!ms.protect_dis, "EN_PTT");

	// The payload channel's own late-entry grant keeps it; one for this call elsewhere moves it
	send(&ms, 1, CC, &g);
	CHECK(ms.state == T3_FOLLOW && ms.call.chan.rx_hz == RX(1093), "same grant: stay");
	t3_csbk_t other = grant(T3_OP_TV_GRANT, 159, 2, TG_B, 1u);
	send(&ms, 1, CC, &other);
	CHECK(ms.call.dst == TG_A && ms.call.chan.rx_hz == RX(1093), "another call's grant: stay");
	t3_csbk_t moved = grant(T3_OP_TV_GRANT, 159, 2, TG_A, 1234u);
	send(&ms, 1, CC, &moved);
	CHECK(ms.state == T3_FOLLOW && last.chan.rx_hz == RX(159) && last.slot == 2, "moved to 159 TS2");

	// Good CSBKs on the call's slot count as activity
	for (int i = 0; i < 60; i++) {
		adv(&ms, 60);
		send(&ms, 2, CC, &p);
	}
	CHECK(ms.state == T3_FOLLOW, "CSBKs keep the call");

	// A P_CLEAR that names another TSCC: return there, confirm before T_Nosig
	t3_csbk_t c = { .lb = true, .op = T3_OP_P_CLEAR };
	c.u.clear.group = true;
	c.u.clear.dst = TG_A;
	c.u.clear.lpcn = 160;
	send(&ms, 2, CC, &c);
	CHECK(ms.state == T3_CAMPED && last.chan.rx_hz == RX(160) && ms.tscc.rx_hz == RX(160), "to the new TSCC");
	adv(&ms, T3_NOSIG_MS + 100);
	CHECK(ms.state == T3_HUNT, "no Aloha there: hunt");
}

static void test_nosig(void)
{
	setup();
	t3_ms_t ms;
	hunt_to_ours(&ms);
	adv(&ms, T3_NOSIG_MS - 200);
	CHECK(ms.state == T3_CAMPED, "still camped");
	int n0 = nact;
	adv(&ms, 300);
	CHECK(ms.state == T3_HUNT && nact == n0 + 1 && last.chan.rx_hz == RX(154), "T_Nosig: the lost TSCC first");
	CHECK(ms.st.hunts == 2, "hunts %u", ms.st.hunts);
	adv(&ms, T3_DWELL_MS + 20);
	CHECK(last.chan.rx_hz == 452000000u, "then the list");
	// Grants and broadcasts keep T_Nosig too
	setup();
	hunt_to_ours(&ms);
	for (int i = 0; i < 200; i++) {
		adv(&ms, 60);
		t3_csbk_t g = grant(T3_OP_TV_GRANT, 159, 1, TG_OTHER, 1u);
		send(&ms, 2, CC, &g);
	}
	CHECK(ms.state == T3_CAMPED, "grants keep the TSCC");
	// Short LCs don't (they come from the CACH of any channel of the site)
	for (int i = 0; i < 200; i++) {
		adv(&ms, 60);
		t3_sysparms_t sp = { true, T3_MODEL_LARGE, (uint16_t)(t3_syscode_14(ours) & 0xFFF), true, (uint16_t)i };
		t3_ms_slc(&ms, T, t3_sysparms_build(&sp));
	}
	CHECK(ms.state != T3_CAMPED, "short LCs alone don't");
}

// The screen names another system heard while hunting (t3_ms_other): from its Aloha with PAR, from two alike short
// LCs without; for T3_OTHER_MS; never once camped
static void test_other(void)
{
	setup();
	t3_ms_t ms;
	uint16_t code = 0;
	t3_ms_start(&ms, &cfg, T);
	poll_all(&ms);
	CHECK(!t3_ms_other(&ms, T, &code), "nothing heard yet");
	adv(&ms, 100);
	t3_syscode_t s = { T3_MODEL_LARGE, 13, 7, T3_PAR_B };
	uint16_t other = t3_syscode_join(&s);
	aloha(&ms, CC, other, true);
	CHECK(ms.state == T3_HUNT && t3_ms_other(&ms, T, &code) && code == other, "Aloha: 0x%04x", code);
	adv(&ms, T3_OTHER_MS - 100);
	CHECK(t3_ms_other(&ms, T, &code), "still shown before T3_OTHER_MS");
	adv(&ms, 200);
	CHECK(!t3_ms_other(&ms, T, &code), "gone after T3_OTHER_MS");

	// Two alike short LCs: the code without PAR
	t3_sysparms_t sp = { true, s.model, (uint16_t)(t3_syscode_14(other) & 0xFFF), true, 5 };
	t3_ms_slc(&ms, T, t3_sysparms_build(&sp));
	poll_all(&ms);
	CHECK(!t3_ms_other(&ms, T, &code), "one short LC isn't trusted");
	adv(&ms, 60);
	t3_ms_slc(&ms, T, t3_sysparms_build(&sp));
	poll_all(&ms);
	CHECK(t3_ms_other(&ms, T, &code) && code == (other & ~3u), "short LC: 0x%04x", code);

	// Camping on ours clears it, and another system's Aloha heard while camped is not shown
	setup();
	hunt_to_ours(&ms);
	CHECK(ms.state == T3_CAMPED && !t3_ms_other(&ms, T, &code), "camped: not shown");
	aloha(&ms, CC, theirs, true);
	CHECK(!t3_ms_other(&ms, T, &code), "camped: another system's Aloha not shown");
}

// A trunking system of another kind (t3_ms_other_fid): its control channel sends CSBKs with its own FID and a good
// CRC, no ETSI Aloha. Two with one FID name it; a failed CRC, a single one or an Aloha of ours do not
static void other_fid_csbk(t3_ms_t *ms, uint8_t fid, bool crc_ok)
{
	uint8_t d[T3_CSBK_BYTES] = { 0x81, fid, 0x12, 0x13, 0x17, 0x18, 0x1b, 0x00, 0x00, 0x07 };
	t3_crc_append(d, T3_CRC_MASK_CSBK);
	d[11] ^= crc_ok ? 0u : 1u;
	rx(ms, 1, T3_DT_CSBK, CC, d);
	adv(ms, 60);
}

static void test_other_fid(void)
{
	setup();
	t3_ms_t ms;
	uint8_t fid = 0;
	uint16_t code;
	t3_ms_start(&ms, &cfg, T);
	poll_all(&ms);
	adv(&ms, 100);
	other_fid_csbk(&ms, 0x06, false);
	other_fid_csbk(&ms, 0x06, false);
	CHECK(!t3_ms_other_fid(&ms, T, &fid), "failed CRCs name nothing");
	other_fid_csbk(&ms, 0x06, true);
	CHECK(!t3_ms_other_fid(&ms, T, &fid), "one isn't trusted");
	other_fid_csbk(&ms, 0x10, true);
	CHECK(!t3_ms_other_fid(&ms, T, &fid), "two FIDs: not one system");
	other_fid_csbk(&ms, 0x10, true);
	CHECK(ms.state == T3_HUNT && t3_ms_other_fid(&ms, T, &fid) && fid == 0x10, "FID 0x%02x", fid);
	CHECK(!t3_ms_other(&ms, T, &code), "no C_SYScode");
	adv(&ms, T3_OTHER_MS + 100);
	CHECK(!t3_ms_other_fid(&ms, T, &fid), "gone after T3_OTHER_MS");

	// Our TSCC sending manufacturer CSBKs between its Alohas: camped, not shown
	setup();
	hunt_to_ours(&ms);
	other_fid_csbk(&ms, 0x10, true);
	other_fid_csbk(&ms, 0x10, true);
	CHECK(ms.state == T3_CAMPED && !t3_ms_other_fid(&ms, T, &fid), "camped: not shown");
}

static void test_slc(void)
{
	setup();
	t3_ms_t ms;
	t3_ms_start(&ms, &cfg, T);
	poll_all(&ms);
	adv(&ms, 100);
	// Another network's C_SYS_Parms: next candidate without waiting for the dwell
	t3_syscode_t s;
	t3_syscode_split(theirs, &s);
	t3_sysparms_t sp = { true, s.model, (uint16_t)(t3_syscode_14(theirs) & 0xFFF), true, 5 };
	t3_ms_slc(&ms, T, t3_sysparms_build(&sp));
	poll_all(&ms);
	CHECK(last.chan.rx_hz == 452000000u && ms.st.foreign_slc == 1, "one foreign short LC may be a corrupt word");
	adv(&ms, 60);
	t3_ms_slc(&ms, T, t3_sysparms_build(&sp));
	poll_all(&ms);
	CHECK(last.chan.rx_hz == 453000000u && ms.st.foreign_slc == 2, "two alike: next");
	// P_SYS_Parms is not a control channel's
	adv(&ms, 100);
	sp.control = false;
	t3_ms_slc(&ms, T, t3_sysparms_build(&sp));
	CHECK(last.chan.rx_hz == 453000000u, "P_SYS_Parms ignored");
	// Camped: the common slot counter
	setup();
	hunt_to_ours(&ms);
	sp = (t3_sysparms_t){ true, T3_MODEL_LARGE, (uint16_t)(t3_syscode_14(ours) & 0xFFF), true, 168 };
	t3_ms_slc(&ms, T, t3_sysparms_build(&sp));
	CHECK(ms.csc_valid && ms.csc == 168, "CSC %u", ms.csc);
}

static void test_learn(void)
{
	setup();
	t3_ms_t ms;
	// A channel the fixed plan doesn't cover (a table-only system): unmapped until announced
	cfg.plan.fixed.base_hz = 0;
	hunt_to_ours(&ms);
	t3_csbk_t g = grant(T3_OP_TV_GRANT, 2000, 1, TG_A, 1u);
	send(&ms, 1, CC, &g);
	CHECK(ms.state == T3_CAMPED && ms.st.unmapped == 1 && ms.st.last_unmapped == 2000, "unmapped");

	// C_BCAST Chan_Freq + BC_AP
	t3_csbk_t b = { .lb = false, .op = T3_OP_C_BCAST };
	b.u.bcast.type = T3_ANN_CHAN_FREQ;
	b.u.bcast.syscode = ours;
	b.u.bcast.parms2 = 2000;
	t3_cdef_t c = { 0, 0, 2000, 465000000u, 460000000u };
	adv(&ms, 60);
	send_mbc(&ms, 1, &b, &c);
	CHECK(ms.st.learned == 1 && ms.learned.n == 1, "learned %u", ms.st.learned);
	adv(&ms, T3_REGRANT_MS + 100);
	aloha(&ms, CC, ours, true);
	send(&ms, 1, CC, &g);
	CHECK(ms.state == T3_FOLLOW && last.chan.rx_hz == 460000000u && last.chan.tx_hz == 465000000u,
	      "learned channel followed: %u", last.chan.rx_hz);
	adv(&ms, T3_INACTIVE_MS + 100);
	CHECK(ms.state == T3_CAMPED, "back");

	// Same announcement again: not counted as new
	camp(&ms, 120);
	send_mbc(&ms, 1, &b, &c);
	CHECK(ms.st.learned == 1, "repeat not counted");

	// A grant with LPCN 4095 and CG_AP: the appended frequencies and color code
	camp(&ms, 120);
	t3_csbk_t h = grant(T3_OP_TV_GRANT, T3_LPCN_ABSOLUTE, 2, TG_B, 7u);
	h.lb = false;
	t3_cdef_t cg = { 3, 0, 2100, 466000000u, 461000000u };
	send_mbc(&ms, 1, &h, &cg);
	CHECK(ms.state == T3_FOLLOW && last.chan.rx_hz == 461000000u && last.chan.cc == 3 && last.slot == 2,
	      "CG_AP followed: %u cc %u", last.chan.rx_hz, last.chan.cc);
	// The payload channel's color code is CG_AP's
	t3_ms_activity(&ms, T, 2);
	t3_csbk_t cl = { .lb = true, .op = T3_OP_P_CLEAR };
	cl.u.clear.group = true;
	cl.u.clear.dst = TG_B;
	adv(&ms, 100);
	send(&ms, 2, CC, &cl);
	CHECK(ms.state == T3_FOLLOW, "P_CLEAR in the TSCC's color code isn't the payload's");
	send(&ms, 2, 3, &cl);
	CHECK(ms.state == T3_CAMPED, "P_CLEAR in the payload's color code");

	// A header whose continuation never comes doesn't act on the next one
	camp(&ms, T3_REGRANT_MS + 100);
	uint8_t d[T3_CSBK_BYTES];
	t3_csbk_build(&h, T3_CRC_MASK_MBC_HDR, d);
	rx(&ms, 1, T3_DT_MBC_HDR, CC, d);
	adv(&ms, 300);
	t3_cdef_build(&cg, h.op, d);
	rx(&ms, 1, T3_DT_MBC_CONT, CC, d);
	CHECK(ms.state == T3_CAMPED, "late continuation dropped");
	// A continuation with a bad CRC
	t3_csbk_build(&h, T3_CRC_MASK_MBC_HDR, d);
	rx(&ms, 1, T3_DT_MBC_HDR, CC, d);
	adv(&ms, 60);
	t3_cdef_build(&cg, h.op, d);
	d[4] ^= 0x80;
	rx(&ms, 1, T3_DT_MBC_CONT, CC, d);
	CHECK(ms.state == T3_CAMPED, "bad continuation dropped");

	// A learned system code: the first network heard, then another network is foreign
	setup();
	cfg.opts |= T3_OPT_LEARN_SYSCODE;
	cfg.auth.model = 0;
	cfg.auth.net = 0;
	cfg.plan.fixed.base_hz = 0;
	t3_ms_start(&ms, &cfg, T);
	poll_all(&ms);
	adv(&ms, 100);
	aloha(&ms, CC, ours, true);
	CHECK(ms.state == T3_CAMPED && ms.auth.net == 12, "learned the system: net %u", ms.auth.net);
	adv(&ms, 60);
	send_mbc(&ms, 1, &b, &c);
	CHECK(ms.learned.n == 1, "learned a channel");
	adv(&ms, 60);
	aloha(&ms, CC, theirs, true);
	CHECK(ms.st.foreign == 1 && ms.learned.n == 1, "another network after learning is foreign");
}

static void test_monitor_emergency(void)
{
	setup();
	t3_ms_t ms;
	cfg.opts |= T3_OPT_EMERGENCY_ANY;
	hunt_to_ours(&ms);
	t3_csbk_t g = grant(T3_OP_TV_GRANT, 159, 1, TG_OTHER, 1u);
	g.u.grant.emergency = true;
	send(&ms, 1, CC, &g);
	CHECK(ms.state == T3_FOLLOW && ms.call.emergency, "emergency on any talkgroup");

	setup();
	cfg.opts |= T3_OPT_MONITOR;
	cfg.opts &= (uint16_t)~T3_OPT_ALLCALL;
	hunt_to_ours(&ms);
	g = grant(T3_OP_TV_GRANT, 159, 1, T3_ID_ALLMSID, 1u);
	send(&ms, 1, CC, &g);
	CHECK(ms.state == T3_CAMPED, "all-call off");
	g = grant(T3_OP_TV_GRANT, 159, 1, T3_ID_REGI, 1u);
	send(&ms, 1, CC, &g);
	CHECK(ms.state == T3_CAMPED, "gateway address never followed");
	g = grant(T3_OP_BTV_GRANT, 159, 1, 777u, 1u);
	send(&ms, 1, CC, &g);
	CHECK(ms.state == T3_FOLLOW, "monitor follows any talkgroup");

	// Private calls off
	setup();
	cfg.opts &= (uint16_t)~T3_OPT_PRIVATE;
	hunt_to_ours(&ms);
	g = grant(T3_OP_PV_GRANT, 159, 1, OWN, 1u);
	send(&ms, 1, CC, &g);
	CHECK(ms.state == T3_CAMPED, "private calls off");
}

// ---- Adjacent sites, Vote Now, leaving a weak TSCC ----

static uint16_t site1;		// our network's site 1

// A C_BCAST from the TSCC: an Adjacent_Site or Vote_Now naming a TSCC (tables 7.71, 7.79)
static void announce(t3_ms_t *ms, uint8_t type, uint16_t code, uint8_t conf, uint8_t adj, uint16_t ch)
{
	t3_csbk_t p = { .lb = true, .op = T3_OP_C_BCAST };
	p.u.bcast.type = type;
	p.u.bcast.syscode = ours;
	p.u.bcast.reg = true;
	p.u.bcast.parms1 = t3_syscode_14(code);
	p.u.bcast.parms2 = ((uint32_t)conf << 19) | ((uint32_t)adj << 16) | ch;
	send(ms, 1, CC, &p);
}

// The tuned TSCC sends Alohas of `code` for dur, the radio measuring dbm (0: nothing)
static void camp_at(t3_ms_t *ms, uint32_t dur, uint16_t code, int16_t dbm)
{
	for (uint32_t t = 0; t < dur; t += 60) {
		adv(ms, 60);
		t3_ms_level(ms, T, dbm);
		if (code != 0) {
			aloha(ms, CC, code, true);
		}
	}
}

static void roam_setup(t3_ms_t *ms, int16_t home_dbm)
{
	setup();
	t3_syscode_t s = { T3_MODEL_LARGE, 12, 1, T3_PAR_AB };
	site1 = t3_syscode_join(&s);
	hunt_to_ours(ms);
	camp_at(ms, 600, ours, home_dbm);
}

static void test_adjacent(void)
{
	t3_ms_t ms;
	roam_setup(&ms, -90);
	CHECK(ms.level_dbm < -88 && ms.level_dbm > -92, "level averaged: %d", ms.level_dbm);
	announce(&ms, T3_ANN_ADJACENT, site1, 0, 0, 200);
	CHECK(ms.nadj == 1 && ms.adj[0].chan.rx_hz == RX(200) && ms.adj[0].sys14 == t3_syscode_14(site1),
	      "adjacent site kept: %u", ms.nadj);
	announce(&ms, T3_ANN_ADJACENT, site1, 0, 0, 200);
	CHECK(ms.nadj == 1, "the same one again is one entry");
	announce(&ms, T3_ANN_ADJACENT, theirs, 0, 0, 201);
	announce(&ms, T3_ANN_ADJACENT, site1, 0, 0, 154);
	announce(&ms, T3_ANN_ADJACENT, site1, 0, 0, 0);
	CHECK(ms.nadj == 1, "another network's, our own TSCC and channel 0 aren't kept: %u", ms.nadj);
	CHECK(ms.st.adj_refused == 1, "the other network's counted as refused: %u", ms.st.adj_refused);
	CHECK(ms.state == T3_CAMPED && last.chan.rx_hz == RX(154), "Adjacent_Site alone doesn't sample");
	// A lost TSCC is tried again first, then the adjacent site before the programmed list
	adv(&ms, T3_NOSIG_MS + 100);
	CHECK(ms.state == T3_HUNT && last.chan.rx_hz == RX(154), "the lost TSCC first");
	adv(&ms, T3_DWELL_MS + 20);
	CHECK(last.chan.rx_hz == RX(200), "then the adjacent site: %u", last.chan.rx_hz);
	adv(&ms, T3_DWELL_MS + 20);
	CHECK(last.chan.rx_hz == 452000000u, "then the list: %u", last.chan.rx_hz);
	// Another network's Aloha found in the hunt drops what was learned of this one
	adv(&ms, 100);
	aloha(&ms, CC, theirs, true);
	CHECK(ms.nadj == 1, "an unauthorized Aloha isn't a network change");
}

static void test_vote(void)
{
	t3_ms_t ms;
	// A stronger adjacent TSCC: sampled in the quiet frames, and moved to
	roam_setup(&ms, -90);
	announce(&ms, T3_ANN_VOTE_NOW, site1, 0, 0, 200);
	CHECK(ms.vote.on && last.chan.rx_hz == RX(200) && ms.st.votes == 1, "Vote Now samples it");
	camp_at(&ms, 300, site1, -70);
	adv(&ms, T3_VOTE_MS);
	CHECK(!ms.vote.on && ms.tscc.rx_hz == RX(200) && last.chan.rx_hz == RX(200) && ms.st.vote_moves == 1,
	      "moved to the stronger site: TSCC %u", ms.tscc.rx_hz);
	CHECK(!ms.confirmed && ms.state == T3_CAMPED, "until its Aloha confirms it");
	camp_at(&ms, 120, site1, -70);
	CHECK(ms.confirmed && ms.syscode == site1, "confirmed on site 1");
	CHECK(ms.nadj == 1 && ms.adj[0].chan.rx_hz == RX(154) && ms.adj[0].sys14 == t3_syscode_14(ours),
	      "the site left is the adjacent one now");

	// About as strong: back to our TSCC with its level, and not sampled again soon
	roam_setup(&ms, -90);
	announce(&ms, T3_ANN_VOTE_NOW, site1, 0, 0, 200);
	camp_at(&ms, 300, site1, -86);
	adv(&ms, T3_VOTE_MS);
	CHECK(!ms.vote.on && ms.tscc.rx_hz == RX(154) && last.chan.rx_hz == RX(154) && ms.st.vote_moves == 0,
	      "within the margin: back, on %u", last.chan.rx_hz);
	CHECK(ms.level_dbm < -88 && ms.adj[0].dbm > -88, "our level kept (%d), theirs noted (%d)", ms.level_dbm,
	      ms.adj[0].dbm);
	camp_at(&ms, 120, ours, -90);
	CHECK(ms.state == T3_CAMPED && ms.confirmed, "camped again");
	announce(&ms, T3_ANN_VOTE_NOW, site1, 0, 0, 200);
	CHECK(!ms.vote.on && ms.st.votes == 1, "not sampled again within T3_VOTE_AGAIN_MS");
	camp_at(&ms, T3_VOTE_AGAIN_MS, ours, -90);
	announce(&ms, T3_ANN_VOTE_NOW, site1, 0, 0, 200);
	CHECK(ms.vote.on && ms.st.votes == 2, "sampled again after it");

	// Silence there (no Aloha of the site named): back, whatever the level
	roam_setup(&ms, -90);
	announce(&ms, T3_ANN_VOTE_NOW, site1, 0, 0, 200);
	camp_at(&ms, 300, theirs, -60);
	adv(&ms, T3_VOTE_MS);
	CHECK(ms.tscc.rx_hz == RX(154) && last.chan.rx_hz == RX(154), "another network there: back");

	// Radiated priorities: a preferred site is taken though weaker, a less preferred one only to escape a weak TSCC
	roam_setup(&ms, -90);
	cfg.leave_dbm = -105;
	announce(&ms, T3_ANN_VOTE_NOW, site1, 3, 1, 200);
	camp_at(&ms, 300, site1, -98);
	adv(&ms, T3_VOTE_MS);
	CHECK(ms.tscc.rx_hz == RX(200), "a preferred site, weaker but above the threshold: moved");
	roam_setup(&ms, -90);
	cfg.leave_dbm = -105;
	announce(&ms, T3_ANN_VOTE_NOW, site1, 3, 1, 200);
	camp_at(&ms, 300, site1, -110);
	adv(&ms, T3_VOTE_MS);
	CHECK(ms.tscc.rx_hz == RX(154), "a preferred site below the threshold: not taken");
	roam_setup(&ms, -90);
	cfg.leave_dbm = -105;
	announce(&ms, T3_ANN_VOTE_NOW, site1, 1, 3, 200);
	camp_at(&ms, 300, site1, -60);
	adv(&ms, T3_VOTE_MS);
	CHECK(ms.tscc.rx_hz == RX(154), "a less preferred site, though much stronger: not taken");

	// T3_OPT_NO_ROAM: Vote Now is noted, never sampled
	roam_setup(&ms, -90);
	cfg.opts |= T3_OPT_NO_ROAM;
	announce(&ms, T3_ANN_VOTE_NOW, site1, 0, 0, 200);
	CHECK(!ms.vote.on && ms.nadj == 1, "no sampling with T3_OPT_NO_ROAM");
}

// Our TSCC, weak, until the radio leaves it (then it hears nothing of ours elsewhere)
static void camp_weak(t3_ms_t *ms, uint32_t dur)
{
	for (uint32_t t = 0; t < dur && ms->state == T3_CAMPED; t += 60) {
		camp_at(ms, 60, ours, -112);
	}
}

static void test_weak(void)
{
	t3_ms_t ms;
	roam_setup(&ms, -90);
	cfg.leave_dbm = -100;
	announce(&ms, T3_ANN_ADJACENT, site1, 0, 0, 200);
	camp_at(&ms, T3_WEAK_MS - 500, ours, -112);
	CHECK(ms.state == T3_CAMPED, "weak, not yet for T3_WEAK_MS");
	camp_weak(&ms, 1200);
	CHECK(ms.state == T3_HUNT && ms.st.weak_leaves == 1 && last.chan.rx_hz == RX(200),
	      "left for the adjacent site: %s on %u", t3_state_name(ms.state), last.chan.rx_hz);
	// That one is weak too: not confirmed, the hunt goes on
	camp_at(&ms, 180, site1, -115);
	CHECK(ms.state == T3_HUNT, "a TSCC below the threshold isn't confirmed while hunting away from a weak one");
	// Once a pass found nothing better, any TSCC will do
	setup();
	t3_syscode_t s = { T3_MODEL_LARGE, 12, 1, T3_PAR_AB };
	site1 = t3_syscode_join(&s);
	cfg.leave_dbm = -100;
	hunt_to_ours(&ms);
	announce(&ms, T3_ANN_ADJACENT, site1, 0, 0, 200);
	camp_weak(&ms, T3_WEAK_MS + 600);
	CHECK(ms.state == T3_HUNT && last.chan.rx_hz == RX(200), "hunting from the adjacent site");
	camp_at(&ms, 300, site1, -75);
	CHECK(ms.state == T3_CAMPED && ms.tscc.rx_hz == RX(200) && ms.syscode == site1, "a stronger one is taken");
	// No adjacent site and one programmed TSCC: nowhere to go, it stays
	setup();
	cfg.leave_dbm = -100;
	cfg.nctrl = 1;
	cfg.ctrl[0] = (t3_chan_t){ .lcn = 154, .cc = T3_CC_ANY, .rx_hz = RX(154), .tx_hz = RX(154) + 5000000u };
	t3_ms_start(&ms, &cfg, T);
	poll_all(&ms);
	camp_at(&ms, T3_WEAK_MS * 2, ours, -112);
	CHECK(ms.state == T3_CAMPED && ms.st.weak_leaves == 0, "nowhere else to look: stays");
}

// Two programmed TSCCs of ours, both heard below a -105 dBm threshold
static void weak_hold_setup(t3_ms_t *ms)
{
	setup();
	cfg.leave_dbm = -105;
	cfg.opts |= T3_OPT_LISTEN_ONLY;	// as on a site of another service: no access to wait on
	cfg.nctrl = 2;
	cfg.ctrl[0] = (t3_chan_t){ .lcn = 154, .cc = T3_CC_ANY, .rx_hz = RX(154), .tx_hz = RX(154) + 5000000u };
	cfg.ctrl[1] = (t3_chan_t){ .lcn = 155, .cc = T3_CC_ANY, .rx_hz = RX(155), .tx_hz = RX(155) + 5000000u };
	t3_ms_start(ms, &cfg, T);
	poll_all(ms);
}

// Every TSCC weak: a pass that finds nothing better holds the radio on the one it comes back to, longer each time,
// instead of leaving every T3_WEAK_MS and missing the grants sent meanwhile
static void test_weak_hold(void)
{
	t3_ms_t ms;
	weak_hold_setup(&ms);
	uint32_t camped_ms = 0;
	for (uint32_t t = 0; t < 60000; t += 60) {
		camp_at(&ms, 60, ours, -107);
		camped_ms += ms.state == T3_CAMPED ? 60u : 0u;
	}
	CHECK(ms.st.weak_leaves == 2 && ms.weak_hold_ms == 2u * T3_WEAK_HOLD_MS,
	      "two passes found nothing in a minute, the hold doubled: %u leaves, hold %u ms",
	      (unsigned)ms.st.weak_leaves, (unsigned)ms.weak_hold_ms);
	CHECK(camped_ms >= 50000u, "camped most of the minute: %u ms", (unsigned)camped_ms);
	for (uint32_t t = 0; t < 30u * 60000u; t += 60) {
		camp_at(&ms, 60, ours, -107);
	}
	CHECK(ms.weak_hold_ms == T3_WEAK_HOLD_MAX_MS && ms.st.weak_leaves <= 12u,
	      "the hold stops at its most: %u ms, %u leaves in 30 min", (unsigned)ms.weak_hold_ms,
	      (unsigned)ms.st.weak_leaves);
	// Clearly above the threshold ends the hold: the next weak spell is left after T3_WEAK_MS again
	camp_at(&ms, 600, ours, -90);
	CHECK(ms.state == T3_CAMPED && ms.weak_hold_ms == 0 && ms.weak_hold_until == 0, "strong: the hold cleared");
	uint32_t before = ms.st.weak_leaves;
	uint32_t waited = 0;
	for (; waited < 4u * T3_WEAK_MS && ms.st.weak_leaves == before; waited += 60) {
		camp_at(&ms, 60, ours, -107);
	}
	CHECK(ms.st.weak_leaves == before + 1 && waited > T3_WEAK_MS,
	      "weak again: left after T3_WEAK_MS (the level average falling first): %u ms", (unsigned)waited);
	// Just at the threshold doesn't clear it
	weak_hold_setup(&ms);
	for (uint32_t t = 0; t < 20000; t += 60) {
		camp_at(&ms, 60, ours, -107);
	}
	CHECK(ms.weak_hold_ms == T3_WEAK_HOLD_MS, "held after one pass: %u", (unsigned)ms.weak_hold_ms);
	camp_at(&ms, 600, ours, -104);
	CHECK(ms.weak_hold_ms == T3_WEAK_HOLD_MS, "a level just above the threshold keeps the hold");
	// A pass that confirms a TSCC just at the threshold ends the hold but keeps the backoff: weak again, the next pass
	// that finds nothing holds twice as long
	uint32_t spent = 0;
	for (; spent < 10u * 60000u && ms.state != T3_HUNT; spent += 60) {
		camp_at(&ms, 60, ours, -107);
	}
	CHECK(ms.state == T3_HUNT && ms.hunt_strict, "left after the hold: %u ms", (unsigned)spent);
	camp_at(&ms, 600, ours, -104);
	CHECK(ms.state == T3_CAMPED && ms.weak_hold_until == 0 && ms.weak_hold_ms == T3_WEAK_HOLD_MS,
	      "confirmed at the threshold: no hold, the backoff kept (%u ms)", (unsigned)ms.weak_hold_ms);
	for (spent = 0; spent < 60000u && ms.weak_hold_until == 0; spent += 60) {
		camp_at(&ms, 60, ours, -107);
	}
	CHECK(ms.weak_hold_ms == 2u * T3_WEAK_HOLD_MS && spent >= T3_WEAK_MS,
	      "weak again, nothing better: the hold doubled (%u ms) after %u ms", (unsigned)ms.weak_hold_ms, (unsigned)spent);
	// A pass after the one that found nothing better hears no TSCC at all (a fade): the hold is still due
	weak_hold_setup(&ms);
	for (spent = 0; spent < 60000u && !ms.hunt_strict; spent += 60) {
		camp_at(&ms, 60, ours, -107);
	}
	for (spent = 0; spent < 60000u && ms.hunt_strict; spent += 60) {
		camp_at(&ms, 60, 0, -107);
	}
	camp_at(&ms, 3u * T3_DWELL_MS, 0, -107);	// past the end of the next pass too
	CHECK(ms.state == T3_HUNT && !ms.hunt_strict && ms.weak_hold_ms == 0, "silent pass: hunting");
	camp_at(&ms, 600, ours, -107);
	CHECK(ms.state == T3_CAMPED && ms.weak_hold_ms == T3_WEAK_HOLD_MS && ms.weak_hold_until != 0,
	      "back on a weak TSCC after a silent pass: held (%u ms)", (unsigned)ms.weak_hold_ms);
	// The hold over, the deadline is dropped: left set, it would read as ahead again 2^31 ms on
	camp_at(&ms, T3_WEAK_HOLD_MS + 600, ours, -104);
	CHECK(ms.state == T3_CAMPED && ms.weak_hold_until == 0 && ms.weak_hold_ms == T3_WEAK_HOLD_MS,
	      "hold over: deadline cleared, the backoff kept");
	// The held TSCC lost (T_Nosig): the search ends the hold, the backoff stays
	weak_hold_setup(&ms);
	for (spent = 0; spent < 60000u && ms.weak_hold_until == 0; spent += 60) {
		camp_at(&ms, 60, ours, -107);
	}
	CHECK(ms.state == T3_CAMPED && ms.weak_hold_until != 0, "held before the loss");
	camp_at(&ms, T3_NOSIG_MS + 600, 0, -107);
	CHECK(ms.state == T3_HUNT && ms.weak_hold_until == 0 && ms.weak_hold_ms == T3_WEAK_HOLD_MS,
	      "TSCC lost: the hold ended, the backoff kept (%u ms)", (unsigned)ms.weak_hold_ms);
}

// The level average reaches a steady level to the dB: 1 dB below the threshold counts as weak
static void test_level_average(void)
{
	t3_ms_t ms;
	weak_hold_setup(&ms);
	camp_at(&ms, 600, ours, -100);
	camp_at(&ms, 3000, ours, -106);
	CHECK(ms.level_dbm == -106, "falling to a steady -106: %d", ms.level_dbm);
	camp_at(&ms, 3000, ours, -100);
	CHECK(ms.level_dbm == -100, "rising to a steady -100: %d", ms.level_dbm);
}

static void test_stop(void)
{
	setup();
	t3_ms_t ms;
	hunt_to_ours(&ms);
	t3_ms_stop(&ms);
	int n0 = nact;
	adv(&ms, T3_NOSIG_MS * 2);
	aloha(&ms, CC, ours, true);
	CHECK(ms.state == T3_OFF && nact == n0, "stopped engine is silent");
}

int main(void)
{
	test_hunt();
	test_settle_crc_cc();
	test_accept_bad_crc();
	test_follow();
	test_payload();
	test_nosig();
	test_slc();
	test_other();
	test_other_fid();
	test_learn();
	test_monitor_emergency();
	test_adjacent();
	test_vote();
	test_weak();
	test_weak_hold();
	test_level_average();
	test_stop();
	printf("t3MsTest: %d checks, %d failures\n", checks, failures);
	return failures != 0;
}
