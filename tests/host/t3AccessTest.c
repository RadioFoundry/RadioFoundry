// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The DMR Tier III mobile station engine, access side (core/dmr/trunk/t3_ms, CONFIG_DMR_TRUNK_TX): random access,
// registration, call requests, cancel, answers to C_AHOY / P_AHOY, P_PROTECT and the end of a call, against a TSCC
// simulated slot by slot with PDUs built from the standard's layouts.
//
//   make -C tests/host t3AccessTest && (cd tests/host && ./t3AccessTest)
#include <stdio.h>
#include <string.h>
#include "dmr/trunk/t3_ms.h"

static int failures;
static int checks;

#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

#define OWN		3101276u
#define CALLER		4321u
#define TG_A		5278505u	// listened to, the talk group
#define TG_B		5310449u	// listened to
#define TG_OTHER	5275843u	// not listened to
#define CC		1
#define TSCC_LCN	154

#define RX(lcn)		(450000000u + ((uint32_t)(lcn) - 1u) * 12500u)

static uint32_t T;
static t3_act_t tuned;			// the last TUNE
static int ntune;
#define SENT_MAX	64
static t3_csbk_t sent[SENT_MAX];	// what the radio sent, parsed
static t3_act_t sent_act[SENT_MAX];
static uint32_t sent_ms[SENT_MAX];
static int nsent;
static int bad_sent;			// a SEND whose CRC or parse failed

// The TSCC's state the simulator broadcasts: its Aloha, the AT bits
static t3_aloha_t al;
static bool at_known = true, at_busy;
static bool aloha_on = true;

static uint16_t ours;
static t3_cfg_t cfg;

static void poll_all(t3_ms_t *ms)
{
	t3_act_t a;
	while (t3_ms_poll(ms, T, &a)) {
		if (a.type == T3_ACT_TUNE) {
			tuned = a;
			ntune++;
		} else if (a.type == T3_ACT_SEND && nsent < SENT_MAX) {
			if (!t3_crc_ok(a.burst, T3_CRC_MASK_CSBK) || !t3_csbk_parse(a.burst, &sent[nsent])) {
				bad_sent++;
			}
			sent_act[nsent] = a;
			sent_ms[nsent] = T;
			nsent++;
		}
	}
}

static void rx(t3_ms_t *ms, uint8_t slot, const t3_csbk_t *p)
{
	uint8_t d[T3_CSBK_BYTES];
	CHECK(t3_csbk_build(p, T3_CRC_MASK_CSBK, d), "build op %u", p->op);
	t3_ms_rx(ms, T, slot, T3_DT_CSBK, CC, d);
	poll_all(ms);
}

static void rx_mbc(t3_ms_t *ms, uint8_t slot, const t3_csbk_t *hdr, const t3_cdef_t *c)
{
	uint8_t d[T3_CSBK_BYTES];
	CHECK(t3_csbk_build(hdr, T3_CRC_MASK_MBC_HDR, d), "build header op %u", hdr->op);
	t3_ms_rx(ms, T, slot, T3_DT_MBC_HDR, CC, d);
	T += 60;
	t3_cdef_build(c, hdr->op, d);
	t3_ms_rx(ms, T, slot, T3_DT_MBC_CONT, CC, d);
	poll_all(ms);
}

static void send_aloha(t3_ms_t *ms)
{
	t3_csbk_t p = { .lb = true, .op = T3_OP_C_ALOHA };
	p.u.aloha = al;
	rx(ms, 1, &p);
}

// One TDMA frame: the CACH and burst of slot 1 (an Aloha when aloha_on), then slot 2. On a payload channel the
// Aloha is left out
static void frame(t3_ms_t *ms)
{
	T += 30;
	t3_ms_slot(ms, T, 1, at_known, at_busy);
	poll_all(ms);
	if (aloha_on && ms->state != T3_FOLLOW) {
		send_aloha(ms);
	}
	T += 30;
	t3_ms_slot(ms, T, 2, at_known, at_busy);
	poll_all(ms);
	if (ms->state == T3_FOLLOW) {
		t3_ms_activity(ms, T, ms->call.slot);
	}
}

static void frames(t3_ms_t *ms, int n)
{
	for (int i = 0; i < n; i++) {
		frame(ms);
	}
}

static t3_csbk_t ack(uint8_t op, uint8_t reason, uint32_t dst, uint32_t src)
{
	t3_csbk_t p = { .lb = true, .op = op };
	p.u.ack.reason = reason;
	p.u.ack.dst = dst;
	p.u.ack.src = src;
	return p;
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

static t3_csbk_t ahoy(uint8_t kind, bool group, uint32_t dst, uint32_t src)
{
	t3_csbk_t p = { .lb = true, .op = T3_OP_C_AHOY };
	p.u.ahoy.kind = kind;
	p.u.ahoy.group = group;
	p.u.ahoy.kind_flag = group;
	p.u.ahoy.dst = dst;
	p.u.ahoy.src = src;
	return p;
}

static void setup(uint16_t opts, bool reg)
{
	memset(&cfg, 0, sizeof(cfg));
	t3_syscode_t s = { T3_MODEL_LARGE, 12, 5, T3_PAR_AB };
	ours = t3_syscode_join(&s);
	cfg.own_id = OWN;
	cfg.auth.model = T3_MODEL_LARGE;
	cfg.auth.net = 12;
	cfg.auth.category = T3_PAR_A;
	cfg.dmrla = 3;
	cfg.plan.fixed = (t3_fixed_plan_t){ 455000000u, 1, 3, false, 5000000u };
	cfg.ctrl[0] = (t3_chan_t){ TSCC_LCN, RX(TSCC_LCN), RX(TSCC_LCN) + 5000000u, T3_CC_ANY };
	cfg.nctrl = 1;
	cfg.tg[0] = (t3_tg_t){ TG_A, T3_TG_LISTEN | T3_TG_TALK };
	cfg.tg[1] = (t3_tg_t){ TG_B, T3_TG_LISTEN };
	cfg.tg[2] = (t3_tg_t){ TG_OTHER, 0 };
	cfg.ntg = 3;
	cfg.opts = opts;
	cfg.seed = 12345;
	memset(&al, 0, sizeof(al));
	al.syscode = ours;
	al.reg = reg;
	al.nrand_wait = 6;	// 6 frames, then the next try
	al.backoff = 6;		// up to 8 frames
	al.version = 2;
	at_known = true;
	at_busy = false;
	aloha_on = true;
	T = 1000;
	ntune = 0;
	nsent = 0;
	bad_sent = 0;
}

static void camp(t3_ms_t *ms)
{
	t3_ms_start(ms, &cfg, T);
	poll_all(ms);
	frames(ms, 3);
	CHECK(ms->state == T3_CAMPED, "camped, state %s", t3_state_name(ms->state));
}

// Camp and register: answer the registration request with Reg_Accepted
static void camp_registered(t3_ms_t *ms)
{
	camp(ms);
	for (int i = 0; i < 10 && nsent == 0; i++) {
		frame(ms);
	}
	CHECK(nsent == 1 && sent[0].op == T3_OP_C_RAND && sent[0].u.rand.kind == T3_SK_REGISTRATION,
	      "registration request sent");
	t3_csbk_t a = ack(T3_OP_C_ACKD, T3_RC_REG_ACCEPTED, OWN, T3_ID_REGI);
	rx(ms, 1, &a);
	CHECK(ms->reg == T3_REG_OK && ms->ax.result == T3_RES_DONE, "registered, reg %s", t3_reg_name(ms->reg));
	nsent = 0;
}

static void test_register(void)
{
	t3_ms_t ms;
	setup(T3_OPT_PRIVATE, true);
	camp(&ms);
	CHECK(ms.reg == T3_REG_NEEDED, "registration needed");
	CHECK(!t3_ms_request(&ms, T, TG_A, true, false), "no call before registering");
	frames(&ms, 2);
	CHECK(nsent == 1 && bad_sent == 0, "one request, sent %d", nsent);
	const t3_rand_t *q = &sent[0].u.rand;
	CHECK(sent[0].op == T3_OP_C_RAND && q->kind == T3_SK_REGISTRATION && (q->options & T3_SO_REG_REGISTER) &&
	      q->extra == (T3_TRG_SYSCODE << 2) && q->dst == ours && q->src == OWN,
	      "registration C_RAND: kind %u options %02x extra %u dst %06x src %u", q->kind, q->options, q->extra,
	      q->dst, q->src);
	CHECK(sent_act[0].slot == 1 && !sent_act[0].payload, "on the TSCC's slot");
	CHECK(ms.ax.state == T3_AX_WAIT_RESP, "waiting for the answer");

	// More signalling to follow, then accepted
	t3_csbk_t w = ack(T3_OP_C_ACKD, T3_RC_WAIT, OWN, T3_ID_REGI);
	rx(&ms, 1, &w);
	CHECK(ms.ax.state == T3_AX_ACKED, "C_WACKD: waiting");
	frames(&ms, 30);
	CHECK(nsent == 1, "no retry after a progress acknowledgement");
	t3_csbk_t other = ack(T3_OP_C_ACKD, T3_RC_REG_ACCEPTED, OWN + 1u, T3_ID_REGI);
	rx(&ms, 1, &other);
	CHECK(ms.reg != T3_REG_OK, "another radio's acknowledgement is ignored");
	t3_csbk_t a = ack(T3_OP_C_ACKD, T3_RC_REG_ACCEPTED, OWN, T3_ID_REGI);
	rx(&ms, 1, &a);
	CHECK(ms.reg == T3_REG_OK && ms.ax.result == T3_RES_DONE && ms.ax.state == T3_AX_IDLE, "registered");
	frames(&ms, 200);
	CHECK(nsent == 1 && ms.reg == T3_REG_OK, "stays registered without asking again");

	// Registering to the talk group (subscription)
	setup(T3_OPT_PRIVATE | T3_OPT_REG_TG, true);
	camp(&ms);
	frames(&ms, 2);
	CHECK(nsent == 1 && sent[0].u.rand.extra == (T3_TRG_TALKGROUP << 2) && sent[0].u.rand.dst == TG_A,
	      "subscription to the talk group");
	a = ack(T3_OP_C_ACKD, T3_RC_REG_SUBSCRIPTION, OWN, T3_ID_REGI);
	rx(&ms, 1, &a);
	CHECK(ms.reg == T3_REG_OK, "subscription accepted registers");

	// Reg = 0: nothing to do
	setup(T3_OPT_PRIVATE, false);
	camp(&ms);
	frames(&ms, 50);
	CHECK(ms.reg == T3_REG_NOT_NEEDED && nsent == 0, "Reg = 0: no registration");

	// Listen only never transmits
	setup(T3_OPT_PRIVATE | T3_OPT_LISTEN_ONLY, true);
	camp(&ms);
	frames(&ms, 50);
	CHECK(ms.reg == T3_REG_LISTEN && nsent == 0, "listen only: no registration");
	CHECK(!t3_ms_request(&ms, T, TG_A, true, false), "listen only: no calls");
}

static void test_no_answer(void)
{
	t3_ms_t ms;
	setup(T3_OPT_PRIVATE, true);
	camp(&ms);
	for (int i = 0; i < 400 && ms.ax.result == T3_RES_PENDING; i++) {
		frame(&ms);
	}
	uint32_t t_end = T;
	CHECK(nsent == T3_NRAND_NR, "NRand_NR tries, got %d", nsent);
	bool spaced = true;
	for (int i = 1; i < nsent; i++) {
		uint32_t gap = sent_ms[i] - sent_ms[i - 1];
		// NWait + 1 frames for the answer, then 0..7 frames of backoff (table 6.3, Backoff 6: 8)
		if (gap < 7u * 60u || gap > 15u * 60u) {
			spaced = false;
			printf("  gap %u ms\n", gap);
		}
	}
	CHECK(spaced, "tries spaced by the wait and the backoff");
	CHECK(ms.ax.result == T3_RES_NO_ANSWER && ms.reg == T3_REG_NEEDED, "no answer: %s", t3_res_name(ms.ax.result));
	int n = nsent;
	while (T - t_end < T3_REG_RETRY_MS - 1000u) {
		frame(&ms);
	}
	CHECK(nsent == n, "quiet until the retry time");
	frames(&ms, 40);
	CHECK(nsent > n, "registration tried again later");
}

static void test_refused_denied(void)
{
	t3_ms_t ms;
	setup(T3_OPT_PRIVATE, true);
	camp(&ms);
	frames(&ms, 2);
	t3_csbk_t n = ack(T3_OP_C_ACKD, T3_RC_REG_DENIED, OWN, T3_ID_REGI);
	rx(&ms, 1, &n);
	CHECK(ms.reg == T3_REG_DENIED && ms.state == T3_HUNT && ms.ax.result == T3_RES_REFUSED, "denied: hunt");
	int k = nsent;
	frames(&ms, 600);
	CHECK(ms.state == T3_CAMPED && ms.reg == T3_REG_DENIED && nsent == k, "back on the TSCC: never asks again");
	CHECK(!t3_ms_request(&ms, T, TG_A, true, false), "no calls when denied");
	// Another area of the network is a fresh start
	t3_syscode_t s = { T3_MODEL_LARGE, 12, 5 | 0x80, T3_PAR_AB };	// SYS_AREA: SITE's top 3 bits
	al.syscode = t3_syscode_join(&s);
	frames(&ms, 3);
	CHECK(ms.reg != T3_REG_DENIED && nsent > k, "another area: registers, reg %s", t3_reg_name(ms.reg));

	setup(T3_OPT_PRIVATE, true);
	camp(&ms);
	frames(&ms, 2);
	n = ack(T3_OP_C_ACKD, T3_RC_REG_REFUSED, OWN, T3_ID_REGI);
	rx(&ms, 1, &n);
	CHECK(ms.reg == T3_REG_NEEDED && ms.state == T3_HUNT && ms.ax.result == T3_RES_REFUSED, "refused: hunt");
	frames(&ms, 600);
	CHECK(ms.state == T3_CAMPED && ms.reg == T3_REG_NEEDED, "refused: back, still needed");
}

static void test_permission(void)
{
	t3_ms_t ms;
	// The AT bit: busy or not decoded, no request
	setup(T3_OPT_PRIVATE, true);
	at_busy = true;
	camp(&ms);
	frames(&ms, 30);
	CHECK(nsent == 0, "AT busy: wait");
	at_busy = false;
	at_known = false;
	frames(&ms, 30);
	CHECK(nsent == 0, "AT unknown: wait");
	at_known = true;
	frames(&ms, 2);
	CHECK(nsent == 1, "AT free: sent");

	// The Mask (6.2.2): 24 bits naming another radio bars this one; matching the low bits lets it through
	setup(T3_OPT_PRIVATE, true);
	al.mask = 24;
	al.ms = OWN + 1u;
	camp(&ms);
	frames(&ms, 30);
	CHECK(nsent == 0, "mask 24, another radio: barred");
	al.mask = 4;
	al.ms = (OWN & 0xFu) | 0x100u;
	frames(&ms, 2);
	CHECK(nsent == 1, "mask 4, low bits ours: sent");

	// Service Function 3 lets only registration through
	setup(T3_OPT_PRIVATE, true);
	al.service_fn = 3;
	camp_registered(&ms);
	CHECK(t3_ms_request(&ms, T, TG_A, true, false), "request accepted locally");
	frames(&ms, 50);
	CHECK(nsent == 0, "service function 3: no call request");
	frames(&ms, (int)(T3_TRAND_TC_MS / 60u));
	CHECK(ms.ax.result == T3_RES_TIMEOUT && ms.ax.state == T3_AX_IDLE, "TRand_TC: %s", t3_res_name(ms.ax.result));
	// An emergency ignores it
	CHECK(t3_ms_request(&ms, T, TG_A, true, true), "emergency request");
	frames(&ms, 2);
	CHECK(nsent == 1 && (sent[0].u.rand.options & T3_SO_EMERGENCY), "emergency goes through");

	// An Aloha must be heard on this TSCC before the first request
	setup(T3_OPT_PRIVATE, true);
	aloha_on = false;
	t3_ms_start(&ms, &cfg, T);
	poll_all(&ms);
	frames(&ms, 50);
	CHECK(nsent == 0 && ms.state == T3_HUNT, "no Aloha, nothing sent");
}

static void test_group_call(void)
{
	t3_ms_t ms;
	setup(T3_OPT_PRIVATE, true);
	camp_registered(&ms);
	CHECK(t3_ms_request(&ms, T, TG_A, true, false), "group call request");
	CHECK(!t3_ms_request(&ms, T, TG_B, true, false), "one request at a time");
	frames(&ms, 2);
	CHECK(nsent == 1, "sent");
	const t3_rand_t *q = &sent[0].u.rand;
	CHECK(q->kind == T3_SK_TG_VOICE && q->dst == TG_A && q->src == OWN && q->options == 0, "TG voice C_RAND");
	t3_csbk_t w = ack(T3_OP_C_ACKD, T3_RC_QUEUED_BUSY, OWN, TG_A);
	rx(&ms, 1, &w);
	CHECK(ms.ax.state == T3_AX_ACKED && ms.ax.progress == T3_QACK, "queued");
	// Someone else's call on a talkgroup we follow: not while asking
	t3_csbk_t g = grant(T3_OP_TV_GRANT, 895, 2, TG_B, CALLER);
	rx(&ms, 1, &g);
	CHECK(ms.state == T3_CAMPED, "another call doesn't take the radio away while it asks");
	g = grant(T3_OP_TV_GRANT, 159, 2, TG_A, OWN);
	rx(&ms, 1, &g);
	CHECK(ms.state == T3_FOLLOW && ms.call.ours && ms.ax.result == T3_RES_GRANTED, "granted: following our call");
	CHECK(tuned.chan.rx_hz == RX(159) && tuned.slot == 2 && tuned.payload, "on the payload channel");
	frames(&ms, 2);
	CHECK(t3_ms_can_talk(&ms), "may talk");

	// P_PROTECT on the payload channel
	t3_csbk_t p = { .lb = true, .op = T3_OP_P_PROTECT };
	p.u.protect.kind = T3_PROTECT_DIS_PTT;
	p.u.protect.group = true;
	p.u.protect.dst = TG_A;
	rx(&ms, 2, &p);
	CHECK(!t3_ms_can_talk(&ms), "DIS_PTT");
	p.u.protect.kind = T3_PROTECT_EN_PTT;
	rx(&ms, 2, &p);
	CHECK(t3_ms_can_talk(&ms), "EN_PTT");
	p.u.protect.kind = T3_PROTECT_EN_PTT_ONE_MS;
	p.u.protect.group = false;
	p.u.protect.dst = CALLER;
	rx(&ms, 2, &p);
	CHECK(!t3_ms_can_talk(&ms), "EN_PTT_ONE_MS for another radio disables ours");
	p.u.protect.dst = OWN;
	rx(&ms, 2, &p);
	CHECK(t3_ms_can_talk(&ms), "EN_PTT_ONE_MS for ours");
	p.u.protect.kind = T3_PROTECT_DIS_PTT;
	p.u.protect.dst = T3_ID_ALLMSID;
	rx(&ms, 2, &p);
	CHECK(!t3_ms_can_talk(&ms), "DIS_PTT to every radio");
	p.u.protect.kind = T3_PROTECT_EN_PTT;
	rx(&ms, 2, &p);
	p.u.protect.kind = T3_PROTECT_ILLEGALLY_PARKED;
	p.u.protect.group = true;
	p.u.protect.dst = TG_A;
	rx(&ms, 2, &p);
	CHECK(ms.state == T3_FOLLOW, "ILLEGALLY_PARKED naming our call: stay");

	// The radio started it: it ends it with N_Maint P_MAINT, then goes back
	nsent = 0;
	int t0 = ntune;
	t3_ms_end_call(&ms, T);
	CHECK(!t3_ms_can_talk(&ms), "no talking while ending");
	frames(&ms, 6);
	CHECK(nsent == T3_N_MAINT && bad_sent == 0, "N_Maint P_MAINT, got %d", nsent);
	bool ok = true;
	for (int i = 0; i < nsent; i++) {
		ok = ok && sent[i].op == T3_OP_P_MAINT && sent[i].u.maint.kind == T3_MAINT_DISCON &&
		     sent[i].u.maint.dst == T3_ID_TSI && sent[i].u.maint.src == OWN && sent_act[i].slot == 2 &&
		     sent_act[i].payload;
	}
	CHECK(ok, "P_MAINT DISCON to TSI from us on the call's slot");
	CHECK(nsent >= 2 && sent_ms[1] - sent_ms[0] == 60u, "consecutive frames");
	CHECK(ms.state == T3_CAMPED && ntune > t0 && tuned.chan.rx_hz == RX(TSCC_LCN), "back on the TSCC");
}

static void test_group_heard(void)
{
	t3_ms_t ms;
	setup(T3_OPT_PRIVATE, true);
	camp_registered(&ms);
	t3_csbk_t g = grant(T3_OP_TV_GRANT, 895, 1, TG_B, CALLER);
	rx(&ms, 1, &g);
	CHECK(ms.state == T3_FOLLOW && !ms.call.ours, "following a call heard");
	frames(&ms, 2);
	CHECK(t3_ms_can_talk(&ms), "a member may talk in a group call");
	// P_AHOY radio check to the call's talkgroup, then to us
	t3_csbk_t h = ahoy(T3_SK_REGISTRATION, true, TG_B, T3_ID_TSI);
	rx(&ms, 1, &h);
	frames(&ms, 1);
	CHECK(nsent == 1 && sent[0].op == T3_OP_P_ACKU && sent[0].u.ack.reason == T3_RC_MS_ACCEPTED &&
	      sent[0].u.ack.dst == T3_ID_TSI && sent[0].u.ack.src == OWN && sent_act[0].payload &&
	      sent_act[0].slot == 1, "P_ACKU to a talkgroup radio check");
	h = ahoy(T3_SK_REGISTRATION, true, TG_OTHER, T3_ID_TSI);
	rx(&ms, 1, &h);
	frames(&ms, 1);
	CHECK(nsent == 1, "another talkgroup's check: no answer");
	// A recipient leaves without sending, and late-entry grants for it don't pull it back
	t3_ms_end_call(&ms, T);
	poll_all(&ms);
	CHECK(ms.state == T3_CAMPED && nsent == 1, "left quietly");
	frames(&ms, 50);
	g.u.grant.late_entry = true;
	rx(&ms, 1, &g);
	CHECK(ms.state == T3_CAMPED, "late entry for the call left: ignored");
	g.u.grant.late_entry = false;
	rx(&ms, 1, &g);
	CHECK(ms.state == T3_FOLLOW, "a new call on it: followed");
	frames(&ms, 2);

	// Selective clear of the talkgroup
	h = ahoy(T3_SK_CANCEL, true, TG_B, T3_ID_TSI);
	rx(&ms, 1, &h);
	frames(&ms, 1);
	CHECK(nsent == 2 && sent[1].op == T3_OP_P_ACKU && sent[1].u.ack.reason == T3_RC_MESSAGE_ACCEPTED,
	      "selective clear acknowledged");
	CHECK(ms.state == T3_CAMPED, "and left");

	// ILLEGALLY_PARKED for another call: leave
	frames(&ms, 50);
	rx(&ms, 1, &g);
	frames(&ms, 2);
	CHECK(ms.state == T3_FOLLOW, "followed again");
	t3_csbk_t p = { .lb = true, .op = T3_OP_P_PROTECT };
	p.u.protect.kind = T3_PROTECT_ILLEGALLY_PARKED;
	p.u.protect.group = true;
	p.u.protect.dst = TG_OTHER;
	rx(&ms, 1, &p);
	CHECK(ms.state == T3_CAMPED, "ILLEGALLY_PARKED for another call: leave");

	// An all-call: listen only, the PTT stays off
	frames(&ms, 50);
	cfg.opts |= T3_OPT_ALLCALL;
	g = grant(T3_OP_TV_GRANT, 969, 1, T3_ID_ALLMSID, CALLER);
	rx(&ms, 1, &g);
	frames(&ms, 2);
	CHECK(ms.state == T3_FOLLOW && !t3_ms_can_talk(&ms), "all-call: no PTT");
}

static void test_private_call(void)
{
	t3_ms_t ms;
	setup(T3_OPT_PRIVATE, true);
	camp_registered(&ms);
	CHECK(t3_ms_request(&ms, T, CALLER, false, false), "private call request");
	frames(&ms, 2);
	CHECK(nsent == 1 && sent[0].u.rand.kind == T3_SK_IND_VOICE && sent[0].u.rand.dst == CALLER, "IND voice");
	t3_csbk_t a = ack(T3_OP_C_ACKD, T3_RC_MS_ALERTING, OWN, CALLER);
	rx(&ms, 1, &a);
	CHECK(ms.ax.state == T3_AX_ACKED, "alerting: still waiting");
	t3_csbk_t g = grant(T3_OP_PV_GRANT, 999, 1, CALLER, OWN);
	rx(&ms, 1, &g);
	CHECK(ms.state == T3_FOLLOW && ms.call.ours && !ms.call.group && ms.ax.result == T3_RES_GRANTED,
	      "private call granted");
	nsent = 0;
	t3_ms_end_call(&ms, T);
	frames(&ms, 6);
	CHECK(nsent == T3_N_MAINT && ms.state == T3_CAMPED, "ended with P_MAINT");

	// Called: the TSCC checks we're there (OACSU), then grants; the called party also ends with P_MAINT
	nsent = 0;
	t3_csbk_t h = ahoy(T3_SK_IND_VOICE, false, OWN, CALLER);
	rx(&ms, 2, &h);
	frames(&ms, 1);
	CHECK(nsent == 1 && sent[0].op == T3_OP_C_ACKU && sent[0].u.ack.reason == T3_RC_MS_ACCEPTED &&
	      sent[0].u.ack.dst == CALLER && sent_act[0].slot == 2 && !sent_act[0].payload,
	      "availability check answered on its slot");
	g = grant(T3_OP_PV_GRANT, 999, 1, OWN, CALLER);
	rx(&ms, 1, &g);
	CHECK(ms.state == T3_FOLLOW && !ms.call.ours, "called: following");
	nsent = 0;
	t3_ms_end_call(&ms, T);
	frames(&ms, 6);
	CHECK(nsent == T3_N_MAINT, "a private call's called party ends it with P_MAINT too");

	// Refused
	frames(&ms, 50);
	CHECK(t3_ms_request(&ms, T, CALLER, false, false), "again");
	frames(&ms, 2);
	a = ack(T3_OP_C_ACKD, T3_RC_MS_AWAY, OWN, CALLER);
	rx(&ms, 1, &a);
	CHECK(ms.ax.result == T3_RES_REFUSED && ms.ax.reason == T3_RC_MS_AWAY, "NACK: refused, reason kept");

	// Call back accepted
	CHECK(t3_ms_request(&ms, T, CALLER, false, false), "again");
	frames(&ms, 2);
	a = ack(T3_OP_C_ACKD, T3_RC_CALLBACK, OWN, CALLER);
	rx(&ms, 1, &a);
	CHECK(ms.ax.result == T3_RES_DONE, "call back: done");

	// TP_Timer after a wait
	CHECK(t3_ms_request(&ms, T, CALLER, false, false), "again");
	frames(&ms, 2);
	a = ack(T3_OP_C_ACKD, T3_RC_WAIT, OWN, CALLER);
	rx(&ms, 1, &a);
	frames(&ms, (int)(T3_TP_MS / 60u) - 10);
	CHECK(ms.ax.state == T3_AX_ACKED, "waiting within TP_Timer");
	rx(&ms, 1, &a);
	frames(&ms, 20);
	CHECK(ms.ax.state == T3_AX_ACKED, "a further wait restarts it");
	frames(&ms, (int)(T3_TP_MS / 60u));
	CHECK(ms.ax.result == T3_RES_TIMEOUT, "TP_Timer: %s", t3_res_name(ms.ax.result));
}

static void test_cancel(void)
{
	t3_ms_t ms;
	setup(T3_OPT_PRIVATE, true);
	camp_registered(&ms);
	at_busy = true;
	CHECK(t3_ms_request(&ms, T, TG_A, true, false), "request");
	frames(&ms, 3);
	t3_ms_cancel(&ms, T);
	at_busy = false;
	frames(&ms, 20);
	CHECK(nsent == 0 && ms.ax.result == T3_RES_CANCELLED, "withdrawn before sending");

	CHECK(t3_ms_request(&ms, T, CALLER, false, false), "request");
	frames(&ms, 2);
	CHECK(nsent == 1, "sent");
	t3_ms_cancel(&ms, T);
	frames(&ms, 2);
	CHECK(nsent == 2 && sent[1].u.rand.kind == T3_SK_CANCEL && sent[1].u.rand.dst == CALLER &&
	      sent[1].u.rand.src == OWN, "cancel request");
	t3_csbk_t a = ack(T3_OP_C_ACKD, T3_RC_MESSAGE_ACCEPTED, OWN, CALLER);
	rx(&ms, 1, &a);
	CHECK(ms.ax.result == T3_RES_DONE && ms.ax.svc == T3_SVC_CANCEL, "cancelled at the TSCC");
}

static void test_ahoy(void)
{
	t3_ms_t ms;
	setup(T3_OPT_PRIVATE, true);
	camp_registered(&ms);
	t3_csbk_t h = ahoy(T3_SK_REGISTRATION, false, OWN, T3_ID_TSI);
	rx(&ms, 1, &h);
	frames(&ms, 1);
	CHECK(nsent == 1 && sent[0].op == T3_OP_C_ACKU && sent[0].u.ack.reason == T3_RC_MS_ACCEPTED &&
	      sent[0].u.ack.dst == T3_ID_TSI && sent[0].u.ack.src == OWN, "radio check answered");
	h = ahoy(T3_SK_REGISTRATION, false, OWN, T3_ID_AUTHI);
	rx(&ms, 1, &h);
	frames(&ms, 1);
	CHECK(nsent == 2 && sent[1].u.ack.reason == T3_RC_MS_NOT_SUPPORTED, "authentication refused");
	h = ahoy(T3_SK_CANCEL, false, OWN, CALLER);
	rx(&ms, 1, &h);
	frames(&ms, 1);
	CHECK(nsent == 3 && sent[2].u.ack.reason == T3_RC_MS_ACCEPTED, "call cancel acknowledged");
	h = ahoy(T3_SK_REGISTRATION, false, OWN + 1u, T3_ID_TSI);
	rx(&ms, 1, &h);
	frames(&ms, 3);
	CHECK(nsent == 3, "another radio's check: nothing");

	setup(T3_OPT_PRIVATE | T3_OPT_LISTEN_ONLY, true);
	camp(&ms);
	h = ahoy(T3_SK_REGISTRATION, false, OWN, T3_ID_TSI);
	rx(&ms, 1, &h);
	frames(&ms, 3);
	CHECK(nsent == 0, "listen only: no answer");
}

static void test_deregister(void)
{
	t3_ms_t ms;
	setup(T3_OPT_PRIVATE, true);
	camp_registered(&ms);
	CHECK(t3_ms_deregister(&ms, T), "de-registration starts");
	frames(&ms, 2);
	CHECK(nsent == 1 && sent[0].u.rand.kind == T3_SK_REGISTRATION && sent[0].u.rand.options == 0,
	      "de-registration C_RAND");
	t3_csbk_t a = ack(T3_OP_C_ACKD, T3_RC_REG_ACCEPTED, OWN, T3_ID_REGI);
	rx(&ms, 1, &a);
	CHECK(ms.ax.result == T3_RES_DONE && ms.deregistered, "de-registered");
	frames(&ms, 200);
	CHECK(nsent == 1, "doesn't register again");
	CHECK(!t3_ms_deregister(&ms, T), "nothing to de-register");
}

static void test_receive_rules(void)
{
	t3_ms_t ms;
	// A call the radio made is followed even on a talkgroup it doesn't listen to, and a private call it made
	setup(T3_OPT_PRIVATE, true);
	camp_registered(&ms);
	t3_csbk_t g = grant(T3_OP_TV_GRANT, 895, 1, TG_OTHER, OWN);
	rx(&ms, 1, &g);
	CHECK(ms.state == T3_FOLLOW && ms.call.ours, "our call on an unlisted talkgroup");
	frames(&ms, 2);
	t3_csbk_t c = { .lb = true, .op = T3_OP_P_CLEAR };
	c.u.clear.group = true;
	c.u.clear.dst = TG_OTHER;
	rx(&ms, 1, &c);
	frames(&ms, 50);
	g = grant(T3_OP_PV_GRANT, 895, 1, CALLER, OWN);
	rx(&ms, 1, &g);
	CHECK(ms.state == T3_FOLLOW && !ms.call.group, "private call we made");
	frames(&ms, 2);

	// P_CLEAR as an MBC with the next TSCC's frequencies (LPCN 0xFFF)
	t3_csbk_t h = { .lb = false, .op = T3_OP_P_CLEAR };
	h.u.clear.lpcn = T3_LPCN_ABSOLUTE;
	h.u.clear.dst = CALLER;
	t3_cdef_t cd = { CC, 0, 777, 446000000u, 441000000u };
	rx_mbc(&ms, 1, &h, &cd);
	CHECK(ms.state == T3_CAMPED && tuned.chan.rx_hz == 441000000u && ms.tscc.rx_hz == 441000000u,
	      "P_CLEAR MBC: to the new TSCC, rx %u", tuned.chan.rx_hz);

	// C_BCAST sets the backoff
	frames(&ms, 5);
	t3_csbk_t b = { .lb = true, .op = T3_OP_C_BCAST };
	b.u.bcast.type = T3_ANN_CALL_TIMERS;
	b.u.bcast.syscode = ours;
	b.u.bcast.reg = true;
	b.u.bcast.backoff = 11;
	rx(&ms, 1, &b);
	CHECK(ms.aloha.backoff == 11, "C_BCAST backoff kept");
}

// The gateway's one-carrier site: the TSCC on TS1, its payload channel on TS2 of the same carrier. The radio knows
// only the control channel (with its LCN), no plan and no Chan_Freq yet. It registers with its talk group and gets
// Reg_Subscription (0x65); a group call is granted on the TSCC's own LCN, slot 2; the payload slot starts with idles
// only (Site Timeslot Sync 0: four CACH before the first sync) and keeps sending idles through the 3 s hang; the
// call ends with P_CLEAR to ALLMSI, LPCN 0, four times
static void test_one_carrier_site(void)
{
	t3_ms_t ms;
	setup(T3_OPT_PRIVATE | T3_OPT_REG_TG, true);
	cfg.plan.fixed.base_hz = 0;
	cfg.ctrl[0].lcn = 7;
	camp(&ms);
	for (int i = 0; i < 10 && nsent == 0; i++) {
		frame(&ms);
	}
	const t3_rand_t *q = &sent[0].u.rand;
	CHECK(nsent == 1 && q->kind == T3_SK_REGISTRATION && q->dst == TG_A && (q->extra >> 2) == T3_TRG_TALKGROUP &&
	      (q->options & T3_SO_REG_REGISTER), "registration with the talk group (TRGT_ADR_CNTS 01b)");
	t3_csbk_t a = ack(T3_OP_C_ACKD, T3_RC_REG_SUBSCRIPTION, OWN, T3_ID_REGI);
	rx(&ms, 1, &a);
	CHECK(ms.reg == T3_REG_OK, "Reg_Subscription counts as registered, reg %s", t3_reg_name(ms.reg));
	nsent = 0;

	CHECK(t3_ms_request(&ms, T, TG_A, true, false), "group call request");
	frames(&ms, 2);
	CHECK(nsent == 1 && sent[0].u.rand.kind == T3_SK_TG_VOICE, "TG voice C_RAND");
	t3_csbk_t g = grant(T3_OP_TV_GRANT, 7, 2, TG_A, OWN);
	rx(&ms, 1, &g);
	T += 60;
	rx(&ms, 1, &g);		// granted twice
	CHECK(ms.state == T3_FOLLOW && ms.call.ours && ms.ax.result == T3_RES_GRANTED, "granted");
	CHECK(tuned.payload && tuned.slot == 2 && tuned.chan.rx_hz == RX(TSCC_LCN) &&
	      tuned.chan.tx_hz == RX(TSCC_LCN) + 5000000u, "the TSCC's own carrier, slot 2, from the control channel's LCN");
	// Idles only, through a call and the site's 3 s hang
	frames(&ms, 100);
	CHECK(ms.state == T3_FOLLOW && t3_ms_can_talk(&ms), "still on the payload slot through idles");
	// The late-entry grant repeats on the TSCC slot don't matter while following
	g.u.grant.late_entry = true;
	rx(&ms, 1, &g);
	CHECK(ms.state == T3_FOLLOW, "late-entry repeat ignored");
	int t0 = ntune;
	t3_csbk_t c = { .lb = true, .op = T3_OP_P_CLEAR };
	c.u.clear.lpcn = T3_LPCN_NULL;
	c.u.clear.group = true;
	c.u.clear.dst = T3_ID_ALLMSI;
	c.u.clear.src = T3_ID_TSI;
	for (int i = 0; i < 4; i++) {
		rx(&ms, 2, &c);
		frame(&ms);
	}
	CHECK(ms.state == T3_CAMPED && ntune == t0 + 1 && tuned.chan.rx_hz == RX(TSCC_LCN) && tuned.slot == 0 &&
	      !tuned.payload, "P_CLEAR to ALLMSI, LPCN 0: back to the TSCC once");
	// A late-entry repeat just after the clear doesn't pull it back
	rx(&ms, 1, &g);
	CHECK(ms.state == T3_CAMPED, "not followed again at once");
	frames(&ms, 20);
	CHECK(ms.state == T3_CAMPED && ms.reg == T3_REG_OK, "camped and registered after the call");
}

int main(void)
{
	test_register();
	test_no_answer();
	test_refused_denied();
	test_permission();
	test_group_call();
	test_group_heard();
	test_private_call();
	test_cancel();
	test_ahoy();
	test_deregister();
	test_receive_rules();
	test_one_carrier_site();
	printf("t3AccessTest: %d checks, %d failures\n", checks, failures);
	return failures != 0;
}
