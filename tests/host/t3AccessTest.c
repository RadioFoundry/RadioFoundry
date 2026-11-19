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

#define OWN		2345678u
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

// The last short data upload the radio sent
static uint8_t up_hdr[T3_CSBK_BYTES];
static uint8_t up_blocks[T3_UDT_BLOCKS_MAX][T3_UDT_BLOCK];
static uint8_t nup_blocks, up_slot;
static int nup;

static void poll_all(t3_ms_t *ms)
{
	t3_act_t a;
	while (t3_ms_poll(ms, T, &a)) {
		if (a.type == T3_ACT_TUNE) {
			tuned = a;
			ntune++;
		} else if (a.type == T3_ACT_SEND && a.udt) {
			memcpy(up_hdr, a.burst, T3_CSBK_BYTES);
			nup_blocks = a.nblocks;
			memcpy(up_blocks, a.blocks, (size_t)a.nblocks * T3_UDT_BLOCK);
			up_slot = a.slot;
			nup++;
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
	cfg.ctrl[0] = (t3_chan_t){ .lcn = TSCC_LCN, .rx_hz = RX(TSCC_LCN), .tx_hz = RX(TSCC_LCN) + 5000000u,
				    .cc = T3_CC_ANY };
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
	nup = 0;
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

	// More signaling to follow, then accepted
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

	// A registration answer must be a registration reason (table 7.41)
	setup(T3_OPT_PRIVATE, true);
	camp(&ms);
	frames(&ms, 2);
	a = ack(T3_OP_C_ACKD, T3_RC_MESSAGE_ACCEPTED, OWN, T3_ID_REGI);
	rx(&ms, 1, &a);
	CHECK(ms.reg != T3_REG_OK && ms.ax.state != T3_AX_IDLE, "Message_Accepted doesn't register");

	// Registering to the talk group (subscription), the only one listened to: no power save with it (6.4.4.1.13.1)
	setup(T3_OPT_PRIVATE | T3_OPT_REG_TG, true);
	cfg.tg[1].flags = 0;
	cfg.ps_rq = 2;
	camp(&ms);
	frames(&ms, 2);
	CHECK(nsent == 1 && sent[0].u.rand.extra == (T3_TRG_TALKGROUP << 2) && sent[0].u.rand.dst == TG_A &&
	      ((sent[0].u.rand.options >> 1) & 7u) == 0u, "subscription to the talk group, PS_RQ 000");
	a = ack(T3_OP_C_ACKD, T3_RC_REG_SUBSCRIPTION, OWN, T3_ID_REGI);
	a.u.ack.info = 2;
	rx(&ms, 1, &a);
	CHECK(ms.reg == T3_REG_OK && ms.reg_tg_ok == 0 && ms.ps_offset == 0,
	      "Reg_Subscription/Attachment: registered, the talkgroup refused, no wake frame");
	setup(T3_OPT_PRIVATE | T3_OPT_REG_TG, true);
	cfg.tg[1].flags = 0;
	camp(&ms);
	frames(&ms, 2);
	a = ack(T3_OP_C_ACKD, T3_RC_REG_ACCEPTED, OWN, T3_ID_REGI);
	rx(&ms, 1, &a);
	CHECK(ms.reg == T3_REG_OK && ms.reg_tg_ok == 0x40, "Reg_Accepted: the talkgroup taken");

	// One talkgroup listened to and none to talk on: it is the one subscribed to, not the system code
	setup(T3_OPT_PRIVATE | T3_OPT_REG_TG, true);
	cfg.tg[0].flags = 0;
	camp(&ms);
	frames(&ms, 2);
	CHECK(nsent == 1 && sent[0].u.rand.extra == (T3_TRG_TALKGROUP << 2) && sent[0].u.rand.dst == TG_B,
	      "the listened talkgroup subscribed: extra %u dst %u", sent[0].u.rand.extra, sent[0].u.rand.dst);

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

static t3_csbk_t reg_list_ahoy(uint8_t blocks)
{
	t3_csbk_t h = { .lb = true, .op = T3_OP_C_AHOY };
	h.u.ahoy.kind = T3_SK_REGISTRATION;
	h.u.ahoy.kind_flag = true;
	h.u.ahoy.blocks = blocks;
	h.u.ahoy.dst = OWN;
	h.u.ahoy.src = T3_ID_TATTSI;
	return h;
}

static uint32_t addr_at(const uint8_t *d, unsigned i)
{
	const uint8_t *p = d + 1u + 3u * i;
	return (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
}

// Registration with a talkgroup subscription list (6.4.4.1.13.2): two or more listened to go as a list to TATTSI,
// uploaded as UDT Address Format when the TSCC asks; the acknowledgement says which were taken
static void test_reg_list(void)
{
	t3_ms_t ms;
	setup(T3_OPT_PRIVATE | T3_OPT_REG_TG, true);
	camp(&ms);
	frames(&ms, 2);
	const t3_rand_t *q = &sent[0].u.rand;
	CHECK(nsent == 1 && q->kind == T3_SK_REGISTRATION && q->dst == T3_ID_TATTSI &&
	      q->extra == ((T3_TRG_TATTSI << 2) | 0u) && (q->options & T3_SO_REG_REGISTER) && ms.reg_tgs == 2,
	      "list request: dst 0x%06x extra 0x%x tgs %u", (unsigned)q->dst, q->extra, ms.reg_tgs);
	t3_csbk_t w = ack(T3_OP_C_ACKD, T3_RC_WAIT, OWN, T3_ID_REGI);
	rx(&ms, 1, &w);
	nup = 0;
	t3_csbk_t h = reg_list_ahoy(1);
	rx(&ms, 1, &h);
	frames(&ms, 1);
	t3_udt_t u;
	CHECK(nup == 1 && nup_blocks == 1 && t3_udt_parse(up_hdr, &u), "list uploaded: %d, %u blocks", nup, nup_blocks);
	CHECK(u.op == T3_OP_C_UDTHU && u.format == T3_UDTF_ADDRESS && u.dst == T3_ID_TATTSI && u.src == OWN && u.sf &&
	      !u.a && !u.group && u.pad == 0 && u.blocks == 1, "list header: format %u sf %u a %u", u.format, u.sf, u.a);
	CHECK(up_blocks[0][0] == 0 && addr_at(up_blocks[0], 0) == TG_A && addr_at(up_blocks[0], 1) == TG_B &&
	      addr_at(up_blocks[0], 2) == 0 && t3_udt_crc_ok((const uint8_t (*)[T3_UDT_BLOCK])up_blocks, 1),
	      "list: TG_A, TG_B, ADRNULL, CRC");
	t3_csbk_t a = ack(T3_OP_C_ACKD, T3_RC_REG_SUBSCRIPTION, OWN, T3_ID_REGI);
	a.u.ack.info = 0x40;	// only the first taken
	rx(&ms, 1, &a);
	CHECK(ms.reg == T3_REG_OK && ms.reg_tg_ok == 0x40 && ms.ax.result == T3_RES_DONE, "registered, mask 0x%02x",
	      ms.reg_tg_ok);

	// Five: two blocks, seven address slots
	setup(T3_OPT_PRIVATE | T3_OPT_REG_TG, true);
	static const uint32_t ids[5] = { 101, 102, 103, 104, 105 };
	for (unsigned i = 0; i < 5; i++) {
		cfg.tg[i] = (t3_tg_t){ ids[i], T3_TG_LISTEN | (i == 0 ? T3_TG_TALK : 0) };
	}
	cfg.ntg = 5;
	camp(&ms);
	frames(&ms, 2);
	CHECK(nsent == 1 && sent[0].u.rand.extra == ((T3_TRG_TATTSI << 2) | 1u), "five: one block appended more");
	nup = 0;
	h = reg_list_ahoy(2);
	rx(&ms, 1, &h);
	frames(&ms, 1);
	CHECK(nup == 1 && nup_blocks == 2 && t3_udt_parse(up_hdr, &u) && u.blocks == 2, "five: two blocks");
	uint8_t d[24];
	memcpy(d, up_blocks[0], 12);
	memcpy(d + 12, up_blocks[1], 12);
	bool ok = true;
	for (unsigned i = 0; i < 7; i++) {
		ok = ok && addr_at(d, i) == (i < 5 ? ids[i] : 0u);
	}
	CHECK(ok && t3_udt_crc_ok((const uint8_t (*)[T3_UDT_BLOCK])up_blocks, 2), "five: addresses across the blocks");
	a.u.ack.info = 0x7F;	// all taken: every bit set, past the five sent too
	rx(&ms, 1, &a);
	CHECK(ms.reg_tg_ok == 0x7C, "all taken, the five bits kept: 0x%02x", ms.reg_tg_ok);

	// Not ours or nothing asked: nothing uploaded
	nup = 0;
	h = reg_list_ahoy(1);
	rx(&ms, 1, &h);
	frames(&ms, 2);
	CHECK(nup == 0, "registered: no upload");

	// Eight listened to: the first seven go
	setup(T3_OPT_PRIVATE | T3_OPT_REG_TG, true);
	for (unsigned i = 0; i < 8; i++) {
		cfg.tg[i] = (t3_tg_t){ 200u + i, T3_TG_LISTEN };
	}
	cfg.ntg = 8;
	camp(&ms);
	frames(&ms, 2);
	CHECK(nsent == 1 && ms.reg_tgs == 7, "eight: seven in the list (%u)", ms.reg_tgs);
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
		// NWait + 1 frames for the answer (and the decision at the next one), then 0..7 frames of backoff (table
		// 6.3, Backoff 6: 8)
		if (gap < 8u * 60u || gap > 16u * 60u) {
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

// The random access log (the debug link's t3.ra*): every burst's time and slot, and what answered it how late
static void test_ra_log(void)
{
	t3_ms_t ms;
	setup(T3_OPT_PRIVATE, true);
	camp(&ms);
	frames(&ms, 2);
	CHECK(nsent == 1 && ms.st.ra_tries == 1, "one request");
	CHECK(ms.st.ra_log[0].tx_ms == sent_ms[0] && ms.st.ra_log[0].tx_slot == 1 &&
	      ms.st.ra_log[0].frames == T3_RA_NONE && ms.st.ra_log[0].rx_ms == 0, "logged, no answer yet");
	frames(&ms, 3);
	t3_csbk_t a = ack(T3_OP_C_ACKD, T3_RC_REG_ACCEPTED, OWN, T3_ID_REGI);
	rx(&ms, 1, &a);
	CHECK(ms.reg == T3_REG_OK, "registered");
	CHECK(ms.st.ra_log[0].rx_ms == T - sent_ms[0] && ms.st.ra_log[0].rx_slot == 1 &&
	      ms.st.ra_log[0].op == T3_OP_C_ACKD && ms.st.ra_log[0].frames * 60u <= ms.st.ra_log[0].rx_ms && ms.st.ra_log[0].rx_ms < ms.st.ra_log[0].frames * 60u + 60u,
	      "answered %u ms, %u frames after", ms.st.ra_log[0].rx_ms, ms.st.ra_log[0].frames);
	frames(&ms, 50);
	CHECK(ms.st.ra_log[0].op == T3_OP_C_ACKD, "later PDUs don't touch it");

	// No answer: every try logged unanswered, the oldest overwritten past T3_RA_LOG
	setup(T3_OPT_PRIVATE, true);
	camp(&ms);
	for (int i = 0; i < 400 && ms.ax.result == T3_RES_PENDING; i++) {
		frame(&ms);
	}
	CHECK(ms.st.ra_tries == (uint32_t)nsent && nsent > 0, "tries counted");
	bool none = true;
	for (uint32_t i = 0; i < T3_RA_LOG && i < ms.st.ra_tries; i++) {
		uint32_t n = ms.st.ra_tries - 1u - i;
		none = none && ms.st.ra_log[n % T3_RA_LOG].frames == T3_RA_NONE && ms.st.ra_log[n % T3_RA_LOG].tx_ms == sent_ms[n];
	}
	CHECK(none, "the last tries, each unanswered at its time");
}

// The answer window (NRand_Wait + 1 frames, 6.2.2) runs from the burst on air: a radio that held the burst back
// (its stack waiting for the slot timing after a retune) still gets the whole window after it; a burst it dropped is
// tried again after the backoff alone
static void test_ra_on_air(void)
{
	t3_ms_t ms;
	setup(T3_OPT_PRIVATE, true);
	camp(&ms);
	frames(&ms, 2);
	CHECK(nsent == 1 && ms.ax.state == T3_AX_WAIT_RESP && ms.ax.to_air, "request handed over");
	uint32_t handed = sent_ms[0];
	frames(&ms, 3);		// held by the radio: the window counted from the hand-over is nearly out
	t3_ms_sent(&ms, T, true);
	CHECK(ms.st.ra_log[0].air_ms == T - handed && !ms.ax.to_air, "on air %u ms after", ms.st.ra_log[0].air_ms);
	frames(&ms, 6);		// the answer in the window's last frame, 10 frames after the hand-over
	CHECK(nsent == 1 && ms.ax.state == T3_AX_WAIT_RESP, "still waiting, no second try (%d sent)", nsent);
	t3_csbk_t a = ack(T3_OP_C_ACKD, T3_RC_REG_ACCEPTED, OWN, T3_ID_REGI);
	rx(&ms, 1, &a);
	CHECK(ms.reg == T3_REG_OK && nsent == 1, "the late answer taken");
	t3_ms_sent(&ms, T, true);
	CHECK(ms.reg == T3_REG_OK && ms.ax.state == T3_AX_IDLE, "a report with nothing pending changes nothing");

	// An answer in the window's last frame, NWait + 1 = 7 frames after the burst's (6.2.1.1.5), is taken
	setup(T3_OPT_PRIVATE, true);
	camp(&ms);
	for (int i = 0; i < 4 && nsent == 0; i++) {
		frame(&ms);
	}
	CHECK(nsent == 1, "request in frame F");
	t3_ms_sent(&ms, T, true);
	frames(&ms, 6);		// F+1..F+6
	T += 30;		// F+7: its slot 1 tick, then the answer in that slot
	t3_ms_slot(&ms, T, 1, at_known, at_busy);
	poll_all(&ms);
	CHECK(nsent == 1 && ms.ax.state == T3_AX_WAIT_RESP, "no retry before F+7's answer (%d sent)", nsent);
	rx(&ms, 1, &a);
	CHECK(ms.reg == T3_REG_OK && nsent == 1 && ms.st.ra_log[0].frames == 7, "taken, %u frames after",
	      ms.st.ra_log[0].frames);

	// Dropped: the backoff starts at the next frame
	setup(T3_OPT_PRIVATE, true);
	camp(&ms);
	frames(&ms, 2);
	CHECK(nsent == 1, "request handed over");
	t3_ms_sent(&ms, T, false);
	frames(&ms, 1);
	CHECK(ms.ax.state == T3_AX_WAIT_SLOT && ms.ax.tries == 1, "backing off at once");
	for (int i = 0; i < 12 && nsent == 1; i++) {
		frame(&ms);
	}
	CHECK(nsent == 2 && sent_ms[1] - sent_ms[0] <= 9u * 60u, "tried again within the backoff, %u ms", sent_ms[1] - sent_ms[0]);
}

// DMRLA 0 is reserved (361-4 table 6.5: 1 at least): unset, every site is its own registration area, so a radio
// that moves to another site of the network registers there
static void test_area_unset(void)
{
	t3_ms_t ms;
	setup(T3_OPT_PRIVATE, true);
	cfg.dmrla = 0;
	camp_registered(&ms);
	t3_syscode_t s;
	t3_syscode_split(al.syscode, &s);
	s.site ^= 1u;	// the next site, the same area for any DMRLA short of the whole SITE field
	al.syscode = t3_syscode_join(&s);
	frames(&ms, 3);
	CHECK(ms.reg == T3_REG_NEEDED || nsent > 0, "another site with DMRLA unset: registers, reg %s",
	      t3_reg_name(ms.reg));
	frames(&ms, 10);
	CHECK(nsent >= 1 && sent[0].op == T3_OP_C_RAND && sent[0].u.rand.kind == T3_SK_REGISTRATION,
	      "registration request sent there");

	// A DMRLA that groups the two sites: one area, no new registration
	setup(T3_OPT_PRIVATE, true);
	cfg.dmrla = 3;
	camp_registered(&ms);
	t3_syscode_split(al.syscode, &s);
	s.site ^= 1u;
	al.syscode = t3_syscode_join(&s);
	frames(&ms, 20);
	CHECK(ms.reg == T3_REG_OK && nsent == 0, "the same area: stays registered, reg %s", t3_reg_name(ms.reg));
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
	CHECK(t3_ms_reg_refused(&ms), "refused: shown while hunting");
	frames(&ms, 600);
	CHECK(ms.state == T3_CAMPED && ms.reg == T3_REG_NEEDED, "refused: back, still needed");
	CHECK(t3_ms_reg_refused(&ms), "refused: shown while registering again");
	nsent = 0;
	for (int i = 0; i < 2000 && nsent == 0; i++) {
		frame(&ms);
	}
	CHECK(nsent > 0 && sent[nsent - 1].op == T3_OP_C_RAND && sent[nsent - 1].u.rand.kind == T3_SK_REGISTRATION,
	      "refused: asks again");
	n = ack(T3_OP_C_ACKD, T3_RC_REG_ACCEPTED, OWN, T3_ID_REGI);
	rx(&ms, 1, &n);
	CHECK(ms.reg == T3_REG_OK && !t3_ms_reg_refused(&ms), "accepted: no longer shown, reg %s", t3_reg_name(ms.reg));
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

	// Service Function 2 (no payload channel): packet data waits; 1 (payload channel): it goes (table 6.1)
	setup(T3_OPT_PRIVATE, true);
	al.service_fn = 2;
	camp_registered(&ms);
	CHECK(t3_ms_pdata(&ms, T, CALLER, false), "packet data accepted locally");
	frames(&ms, 30);
	CHECK(nsent == 0, "service function 2: no packet data request");
	al.service_fn = 1;
	frames(&ms, 2);
	CHECK(nsent == 1 && sent[0].op == T3_OP_C_RAND, "service function 1: packet data request sent");

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
	frames(&ms, (int)(T3_PROTECT_ONE_MS / 60u) + 2);
	CHECK(t3_ms_can_talk(&ms), "only while they come (6.6.2.3.2.3)");
	rx(&ms, 2, &p);
	CHECK(!t3_ms_can_talk(&ms), "again for another radio");
	p.u.protect.dst = OWN;
	rx(&ms, 2, &p);
	CHECK(t3_ms_can_talk(&ms), "EN_PTT_ONE_MS for ours");
	p.u.protect.kind = T3_PROTECT_DIS_PTT;
	p.u.protect.dst = T3_ID_ALLMSID;
	rx(&ms, 2, &p);
	CHECK(!t3_ms_can_talk(&ms), "DIS_PTT to every radio");
	p.u.protect.kind = T3_PROTECT_EN_PTT;
	rx(&ms, 2, &p);
	CHECK(t3_ms_can_talk(&ms), "EN_PTT to every radio");
	p.u.protect.kind = T3_PROTECT_DIS_PTT;
	p.u.protect.dst = CALLER;
	rx(&ms, 2, &p);
	CHECK(t3_ms_can_talk(&ms), "DIS_PTT to another radio: not ours");
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
	// The voice service forms (tables 6.42, 6.43): MS_Accepted from the radio's ID (6.6.2.3.2.1)
	h = ahoy(T3_SK_TG_VOICE, true, TG_B, T3_ID_TSI);
	rx(&ms, 1, &h);
	frames(&ms, 1);
	CHECK(nsent == 2 && sent[1].op == T3_OP_P_ACKU && sent[1].u.ack.reason == T3_RC_MS_ACCEPTED &&
	      sent[1].u.ack.dst == T3_ID_TSI && sent[1].u.ack.src == OWN, "talkgroup voice radio check: 0x%02x",
	      sent[1].u.ack.reason);
	h = ahoy(T3_SK_IND_VOICE, false, OWN, T3_ID_TSI);
	rx(&ms, 1, &h);
	frames(&ms, 1);
	CHECK(nsent == 3 && sent[2].op == T3_OP_P_ACKU && sent[2].u.ack.reason == T3_RC_MS_ACCEPTED &&
	      sent[2].u.ack.dst == T3_ID_TSI, "individual voice radio check: 0x%02x", sent[2].u.ack.reason);
	h = ahoy(T3_SK_IND_VOICE, false, OWN, CALLER);
	rx(&ms, 1, &h);
	frames(&ms, 1);
	CHECK(nsent == 4 && sent[3].u.ack.reason == T3_RC_MS_NOT_SUPPORTED, "not from TSI: not a radio check");
	// An authentication challenge here is answered with P_ACKU (6.6.2.3.2.2)
	cfg.auth_key_set = true;
	h = ahoy(T3_SK_REGISTRATION, false, OWN, 0x123456u);
	rx(&ms, 1, &h);
	frames(&ms, 1);
	CHECK(nsent == 5 && sent[4].op == T3_OP_P_ACKU && sent[4].u.ack.reason == T3_RC_MS_AUTH_RESPONSE &&
	      sent_act[4].payload, "payload channel challenge answered");
	cfg.auth_key_set = false;
	CHECK(ms.state == T3_FOLLOW, "still on the call");
	nsent = 0;
	// A recipient leaves without sending, and late-entry grants for it don't pull it back
	t3_ms_end_call(&ms, T);
	poll_all(&ms);
	CHECK(ms.state == T3_CAMPED && nsent == 0, "left quietly");
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
	CHECK(nsent == 1 && sent[0].op == T3_OP_P_ACKU && sent[0].u.ack.reason == T3_RC_MS_ACCEPTED,
	      "selective clear acknowledged with MS_Accepted (6.6.2.3.2.7): 0x%02x", sent[0].u.ack.reason);
	CHECK(ms.state == T3_CAMPED, "and left");

	// Within T3_REGRANT_MS of leaving, a set-up grant (Late_Entry 0) is a new call: followed (6.6.2.4.1)
	frames(&ms, 2);
	rx(&ms, 1, &g);
	frames(&ms, 2);
	CHECK(ms.state == T3_FOLLOW, "a set-up grant just after leaving: followed");
	// ILLEGALLY_PARKED for another call: leave
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

	// Called: the TSCC checks we're there (OACSU), then grants; the called party also ends with P_MAINT. A TSCC on
	// both slots (an Aloha on slot 2 too): the answer goes on the slot the C_AHOY came on
	nsent = 0;
	t3_csbk_t al2 = { .lb = true, .op = T3_OP_C_ALOHA };
	al2.u.aloha = al;
	rx(&ms, 2, &al2);
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

	// Message_Accepted to a group call request: not an answer that ends it; the grant still comes
	CHECK(t3_ms_request(&ms, T, TG_A, true, false), "group call");
	frames(&ms, 2);
	a = ack(T3_OP_C_ACKD, T3_RC_MESSAGE_ACCEPTED, OWN, TG_A);
	rx(&ms, 1, &a);
	CHECK(ms.ax.state == T3_AX_ACKED && ms.state == T3_CAMPED, "message accepted: still waiting for the grant");
	g = grant(T3_OP_TV_GRANT, 999, 2, TG_A, OWN);
	rx(&ms, 1, &g);
	CHECK(ms.state == T3_FOLLOW && ms.call.ours && ms.ax.result == T3_RES_GRANTED, "then granted: follows");
	t3_ms_end_call(&ms, T);
	frames(&ms, 6);
	// ... and with no grant, TP_Timer ends it as unanswered
	CHECK(t3_ms_request(&ms, T, TG_A, true, false), "group call again");
	frames(&ms, 2);
	rx(&ms, 1, &a);
	frames(&ms, (int)(T3_TP_MS / 60u) + 20);
	CHECK(ms.ax.state == T3_AX_IDLE && ms.ax.result != T3_RES_DONE && ms.ax.result != T3_RES_GRANTED,
	      "message accepted, no grant: not done, result %s", t3_res_name(ms.ax.result));

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
	CHECK(nsent == 0 && ms.ax.result == T3_RES_CANCELED, "withdrawn before sending");

	CHECK(t3_ms_request(&ms, T, CALLER, false, false), "request");
	frames(&ms, 2);
	CHECK(nsent == 1, "sent");
	t3_ms_cancel(&ms, T);
	frames(&ms, 2);
	CHECK(nsent == 2 && sent[1].u.rand.kind == T3_SK_CANCEL && sent[1].u.rand.dst == CALLER &&
	      sent[1].u.rand.src == OWN, "cancel request");
	t3_csbk_t a = ack(T3_OP_C_ACKD, T3_RC_MESSAGE_ACCEPTED, OWN, CALLER);
	rx(&ms, 1, &a);
	CHECK(ms.ax.result == T3_RES_DONE && ms.ax.svc == T3_SVC_CANCEL, "canceled at the TSCC");
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
	// A talkgroup radio check (6.4.12 b): a member answers from its own ID; another talkgroup's isn't ours
	h = ahoy(T3_SK_REGISTRATION, true, TG_B, T3_ID_TSI);
	rx(&ms, 1, &h);
	frames(&ms, 1);
	CHECK(nsent == 4 && sent[3].op == T3_OP_C_ACKU && sent[3].u.ack.reason == T3_RC_MS_ACCEPTED &&
	      sent[3].u.ack.dst == T3_ID_TSI && sent[3].u.ack.src == OWN, "talkgroup radio check answered");
	h = ahoy(T3_SK_REGISTRATION, true, TG_OTHER, T3_ID_TSI);
	rx(&ms, 1, &h);
	frames(&ms, 3);
	CHECK(nsent == 4, "another talkgroup's check: nothing");

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

// A one-carrier site whose control channel row was typed with a site tool's number: tools that count each timeslot as
// a channel call LPCN 311 "623" (slot 1) and "624" (slot 2). The grant names 311; the row is found under that
// numbering, either slot. A row that matches neither way: the radio's own call ends at once as unmapped, not at
// TP_Timer, and says which channel the grant named
static void test_per_slot_lcn(void)
{
	static const uint16_t rows[] = { 624, 623 };
	for (unsigned i = 0; i < sizeof rows / sizeof rows[0]; i++) {
		t3_ms_t ms;
		setup(T3_OPT_PRIVATE, true);
		cfg.plan.fixed.base_hz = 0;
		cfg.ctrl[0].lcn = rows[i];
		camp_registered(&ms);
		CHECK(t3_ms_request(&ms, T, TG_A, true, false), "row %u: group call request", rows[i]);
		frames(&ms, 2);
		t3_csbk_t g = grant(T3_OP_TV_GRANT, 311, 2, TG_A, OWN);
		rx(&ms, 1, &g);
		CHECK(ms.state == T3_FOLLOW && ms.call.ours && ms.ax.result == T3_RES_GRANTED && ms.st.unmapped == 0,
		      "row %u: grant to LPCN 311 followed", rows[i]);
		CHECK(tuned.payload && tuned.slot == 2 && tuned.chan.rx_hz == RX(TSCC_LCN) && tuned.chan.lcn == 311,
		      "row %u: the control channel's carrier, named 311", rows[i]);
	}

	t3_ms_t ms;
	setup(T3_OPT_PRIVATE, true);
	cfg.plan.fixed.base_hz = 0;
	cfg.ctrl[0].lcn = 600;
	camp_registered(&ms);
	// Someone else's call on an unknown channel: counted, nothing else
	t3_csbk_t g = grant(T3_OP_TV_GRANT, 311, 2, TG_A, CALLER);
	rx(&ms, 1, &g);
	CHECK(ms.state == T3_CAMPED && ms.st.unmapped == 1 && ms.st.last_unmapped == 311, "unmapped: counted");
	CHECK(t3_ms_request(&ms, T, TG_A, true, false), "group call request");
	frames(&ms, 2);
	g = grant(T3_OP_TV_GRANT, 311, 2, TG_A, OWN);
	rx(&ms, 1, &g);
	CHECK(ms.state == T3_CAMPED && ms.ax.state == T3_AX_IDLE && ms.ax.result == T3_RES_UNMAPPED &&
	      ms.st.last_unmapped == 311, "our grant unmapped: ends now, result %s", t3_res_name(ms.ax.result));
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
	cfg.tg[1].flags = 0;
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

// A status delivered by C_AHOY: 5 most significant bits in Service_Options_Mirror, 2 least in Appended_Blocks
static t3_csbk_t status_ahoy(uint8_t v, bool group, uint32_t dst, uint32_t src)
{
	t3_csbk_t p = ahoy(T3_SK_STATUS, group, dst, src);
	p.u.ahoy.options = (uint8_t)((group ? 0x40u : 0u) | (v >> 2));
	p.u.ahoy.blocks = (uint8_t)(v & 3u);
	return p;
}

// Send until the C_RAND goes out
static bool sent_rand(t3_ms_t *ms)
{
	for (int i = 0; i < 20 && nsent == 0; i++) {
		frame(ms);
	}
	return nsent >= 1 && sent[nsent - 1].op == T3_OP_C_RAND;
}

// Broadcasts (6.6.1.7, table 7.52): a call to an all-call ID or to a talkgroup set T3_TG_BROADCAST carries the broadcast
// option; the radio talks on its own; a broadcast or all-call someone else made keeps its PTT off
static void test_broadcast(void)
{
	t3_ms_t ms;
	setup(T3_OPT_PRIVATE | T3_OPT_ALLCALL, true);
	cfg.tg[1].flags |= T3_TG_BROADCAST;
	camp_registered(&ms);

	CHECK(t3_ms_request(&ms, T, T3_ID_ALLMSID, true, false), "all-call request");
	CHECK(sent_rand(&ms), "sent");
	const t3_rand_t *q = &sent[nsent - 1].u.rand;
	CHECK(q->kind == T3_SK_TG_VOICE && q->dst == T3_ID_ALLMSID && q->options == T3_SO_BROADCAST,
	      "all-call: TG voice, broadcast option, options 0x%02x", q->options);
	t3_csbk_t g = grant(T3_OP_BTV_GRANT, 159, 2, T3_ID_ALLMSID, OWN);
	rx(&ms, 1, &g);
	frames(&ms, 2);
	CHECK(ms.state == T3_FOLLOW && ms.call.ours && t3_ms_can_talk(&ms), "our all-call: may talk");
	t3_ms_end_call(&ms, T);
	frames(&ms, 6);
	CHECK(ms.state == T3_CAMPED, "back");

	nsent = 0;
	CHECK(t3_ms_request(&ms, T, TG_B, true, true), "broadcast talkgroup, emergency");
	CHECK(sent_rand(&ms), "sent");
	q = &sent[nsent - 1].u.rand;
	CHECK(q->dst == TG_B && q->options == (T3_SO_BROADCAST | T3_SO_EMERGENCY), "options 0x%02x", q->options);
	t3_csbk_t a = ack(T3_OP_C_ACKD, T3_RC_TRANSIENT_REFUSED, OWN, TG_B);
	rx(&ms, 1, &a);

	nsent = 0;
	CHECK(t3_ms_request(&ms, T, TG_A, true, false), "a plain talkgroup");
	CHECK(sent_rand(&ms), "sent");
	CHECK(sent[nsent - 1].u.rand.options == 0, "no broadcast option");
	a = ack(T3_OP_C_ACKD, T3_RC_TRANSIENT_REFUSED, OWN, TG_A);
	rx(&ms, 1, &a);

	// Heard: another radio's broadcast to a talkgroup we follow, and its all-call
	g = grant(T3_OP_BTV_GRANT, 159, 2, TG_A, CALLER);
	rx(&ms, 1, &g);
	frames(&ms, 2);
	CHECK(ms.state == T3_FOLLOW && !ms.call.ours && !t3_ms_can_talk(&ms), "a broadcast heard: no PTT");
	t3_csbk_t c = { .lb = true, .op = T3_OP_P_CLEAR };
	c.u.clear.dst = TG_A;
	c.u.clear.group = true;
	rx(&ms, 2, &c);
	frames(&ms, 2);
	g = grant(T3_OP_BTV_GRANT, 159, 1, T3_ID_ALLMSID, CALLER);
	rx(&ms, 1, &g);
	frames(&ms, 2);
	CHECK(ms.state == T3_FOLLOW && !t3_ms_can_talk(&ms), "an all-call heard: no PTT");
}

// Stun and revive without authentication (6.4.9.1): a C_AHOY from STUNI, Service_Kind supplementary, the flag 0 to
// stun and 1 to revive, answered C_ACKU Message_Accepted. Stunned, the radio asks for and takes no user service; a
// radio check and the revive still work
static void test_stun(void)
{
	t3_ms_t ms;
	setup(T3_OPT_PRIVATE | T3_OPT_STUN, true);
	camp_registered(&ms);
	t3_csbk_t h = ahoy(T3_SK_SUPPLEMENTARY, false, OWN, T3_ID_STUNI);
	h.u.ahoy.kind_flag = false;
	rx(&ms, 1, &h);
	frames(&ms, 2);
	CHECK(ms.stunned && ms.stuns == 1, "stunned");
	CHECK(nsent == 1 && sent[0].op == T3_OP_C_ACKU && sent[0].u.ack.reason == T3_RC_MESSAGE_ACCEPTED &&
	      sent[0].u.ack.dst == T3_ID_STUNI, "stun acknowledged");
	nsent = 0;
	CHECK(!t3_ms_request(&ms, T, TG_A, true, false) && ms.ax.result == T3_RES_NOT_ALLOWED, "no calls");
	CHECK(!t3_ms_status(&ms, T, TG_A, true, 3), "no status");
	t3_csbk_t g = grant(T3_OP_TV_GRANT, 159, 2, TG_A, CALLER);
	rx(&ms, 1, &g);
	frames(&ms, 2);
	CHECK(ms.state == T3_CAMPED, "no call followed");
	t3_csbk_t a = ahoy(T3_SK_IND_VOICE, false, OWN, CALLER);
	rx(&ms, 1, &a);
	frames(&ms, 2);
	CHECK(nsent == 0, "an availability check isn't answered");
	a = ahoy(T3_SK_REGISTRATION, false, OWN, T3_ID_TSI);
	rx(&ms, 1, &a);
	frames(&ms, 2);
	CHECK(nsent == 1 && sent[0].op == T3_OP_C_ACKU, "a radio check is");
	nsent = 0;
	h.u.ahoy.kind_flag = true;
	rx(&ms, 1, &h);
	frames(&ms, 2);
	CHECK(!ms.stunned && ms.revives == 1 && nsent == 1 && sent[0].u.ack.reason == T3_RC_MESSAGE_ACCEPTED, "revived");
	CHECK(t3_ms_request(&ms, T, TG_A, true, false), "calls again");
	// Another radio's stun isn't ours
	t3_ms_end_call(&ms, T);
	h = ahoy(T3_SK_SUPPLEMENTARY, false, CALLER, T3_ID_STUNI);
	rx(&ms, 1, &h);
	CHECK(!ms.stunned, "another radio's stun");
}

// Another channel of the system (t3_ms_groups_changed): with T3_OPT_REG_TG the radio registers again with the new
// talkgroups once they have stayed T3_REG_TG_DELAY_MS (the knob turned through channels registers once); changed during
// a registration, it registers again after it; without the option nothing is sent
static void test_groups_changed(void)
{
	t3_ms_t ms;
	setup(T3_OPT_PRIVATE | T3_OPT_REG_TG, true);
	cfg.tg[1].flags = 0;
	camp_registered(&ms);
	cfg.tg[0].flags = 0;
	cfg.tg[1].flags = T3_TG_LISTEN | T3_TG_TALK;
	t3_ms_groups_changed(&ms, T);
	frames(&ms, 10);
	CHECK(nsent == 0, "nothing before the delay");
	t3_ms_groups_changed(&ms, T);		// turned again: the delay starts over
	frames(&ms, 12);
	CHECK(nsent == 0, "nothing before the delay from the last change");
	for (int i = 0; i < 30 && nsent == 0; i++) {
		frame(&ms);
	}
	CHECK(nsent == 1 && sent[0].op == T3_OP_C_RAND && sent[0].u.rand.kind == T3_SK_REGISTRATION &&
	      sent[0].u.rand.extra == (T3_TRG_TALKGROUP << 2) && sent[0].u.rand.dst == TG_B,
	      "registered again with the new talkgroup (sent %d)", nsent);
	t3_csbk_t a = ack(T3_OP_C_ACKD, T3_RC_REG_ACCEPTED, OWN, T3_ID_REGI);
	rx(&ms, 1, &a);
	CHECK(ms.reg == T3_REG_OK, "registered");

	// Changed while that registration runs: once it is answered, the radio registers again with the newer
	nsent = 0;
	cfg.tg[1].flags = 0;
	cfg.tg[0].flags = T3_TG_LISTEN | T3_TG_TALK;
	t3_ms_groups_changed(&ms, T);
	for (int i = 0; i < 30 && nsent == 0; i++) {
		frame(&ms);
	}
	CHECK(nsent == 1 && sent[0].u.rand.dst == TG_A, "to TG_A");
	cfg.tg[0].flags = 0;
	cfg.tg[1].flags = T3_TG_LISTEN | T3_TG_TALK;
	t3_ms_groups_changed(&ms, T);
	rx(&ms, 1, &a);
	CHECK(ms.reg == T3_REG_NEEDED, "the answer to a stale list: needed again (reg %s)", t3_reg_name(ms.reg));
	for (int i = 0; i < 30 && nsent == 1; i++) {
		frame(&ms);
	}
	CHECK(nsent == 2 && sent[1].u.rand.dst == TG_B, "then to TG_B (sent %d)", nsent);

	// Registered to the system code: the talkgroups are only followed, nothing sent
	setup(T3_OPT_PRIVATE, true);
	camp_registered(&ms);
	cfg.tg[0].flags = T3_TG_LISTEN;
	cfg.tg[1].flags = T3_TG_LISTEN | T3_TG_TALK;
	t3_ms_groups_changed(&ms, T);
	frames(&ms, 40);
	CHECK(nsent == 0 && ms.reg == T3_REG_OK, "without T3_OPT_REG_TG nothing sent");
}

// Without T3_OPT_STUN (an open system's radios) a stun is answered MSNot_Supported and nothing changes, with K or not
static void test_stun_off(void)
{
	for (int with_k = 0; with_k < 2; with_k++) {
		t3_ms_t ms;
		setup(T3_OPT_PRIVATE, true);
		cfg.auth_key_set = with_k != 0;
		camp_registered(&ms);
		nsent = 0;
		t3_csbk_t h = ahoy(T3_SK_SUPPLEMENTARY, false, OWN, T3_ID_STUNI);
		h.u.ahoy.kind_flag = false;
		rx(&ms, 1, &h);
		frames(&ms, 2);
		CHECK(!ms.stunned && ms.stuns == 0 && nsent == 1 && sent[0].op == T3_OP_C_ACKU &&
		      sent[0].u.ack.reason == T3_RC_MS_NOT_SUPPORTED && sent[0].u.ack.dst == T3_ID_STUNI,
		      "stun refused without the option (K %d)", with_k);
		CHECK(t3_ms_request(&ms, T, TG_A, true, false), "still calls (K %d)", with_k);
	}
}

// Authentication (6.4.8): a challenge during registration is answered C_ACKU Authentication_Response with the RC4
// result as the target, and the registration waits on for its answer; a challenge on its own is answered too; without
// K a challenge is not supported
static void test_auth(void)
{
	t3_ms_t ms;
	setup(T3_OPT_PRIVATE, true);
	cfg.auth_key_set = true;
	for (int i = 0; i < T3_AUTH_KEY_LEN; i++) {
		cfg.auth_key[i] = (uint8_t)(i + 1);
	}
	camp(&ms);
	for (int i = 0; i < 10 && nsent == 0; i++) {
		frame(&ms);
	}
	CHECK(nsent == 1 && sent[0].u.rand.kind == T3_SK_REGISTRATION, "registration request");
	nsent = 0;
	t3_csbk_t h = ahoy(T3_SK_REGISTRATION, false, OWN, 0x7A17C0u);
	rx(&ms, 1, &h);
	frames(&ms, 2);
	CHECK(nsent == 1 && sent[0].op == T3_OP_C_ACKU && sent[0].u.ack.reason == T3_RC_MS_AUTH_RESPONSE &&
	      sent[0].u.ack.dst == 0xE8CA1Du && sent[0].u.ack.src == OWN, "the response, %06x",
	      (unsigned)sent[0].u.ack.dst);
	CHECK(ms.ax.state == T3_AX_ACKED && ms.ax.svc == T3_SVC_REGISTER && ms.auths == 1, "registration goes on");
	nsent = 0;
	frames(&ms, 10);
	CHECK(nsent == 0, "no new request meanwhile");
	t3_csbk_t a = ack(T3_OP_C_ACKD, T3_RC_REG_ACCEPTED, OWN, T3_ID_REGI);
	rx(&ms, 1, &a);
	CHECK(ms.reg == T3_REG_OK, "registered");

	// A poll on its own, with a PSN
	cfg.psn = 0x010203u;
	nsent = 0;
	rx(&ms, 1, &h);
	frames(&ms, 2);
	CHECK(nsent == 1 && sent[0].u.ack.dst == 0xE9C81Eu, "with the PSN, %06x", (unsigned)sent[0].u.ack.dst);

	// Without K
	cfg.auth_key_set = false;
	nsent = 0;
	rx(&ms, 1, &h);
	frames(&ms, 2);
	CHECK(nsent == 1 && sent[0].u.ack.reason == T3_RC_MS_NOT_SUPPORTED, "no key: not supported");
	// A radio check still is one (TSI is above the challenge range)
	nsent = 0;
	cfg.auth_key_set = true;
	h = ahoy(T3_SK_REGISTRATION, false, OWN, T3_ID_TSI);
	rx(&ms, 1, &h);
	frames(&ms, 2);
	CHECK(nsent == 1 && sent[0].u.ack.reason == T3_RC_MS_ACCEPTED, "radio check");
}

// Mass registration (6.4.5.1): an applicable C_BCAST MassReg starts a registration at a random time inside
// Reg_Window; not applicable (the mask's bits differ) does nothing; another one inside the window is ignored;
// Reg_Window 0 cancels
static t3_csbk_t mass_reg(uint8_t window, uint8_t mask, uint32_t addr)
{
	t3_csbk_t b = { .lb = true, .op = T3_OP_C_BCAST };
	b.u.bcast.type = T3_ANN_MASS_REG;
	b.u.bcast.syscode = ours;
	b.u.bcast.reg = true;
	b.u.bcast.parms1 = (uint16_t)((window << 5) | mask);
	b.u.bcast.parms2 = addr;
	return b;
}

static int reg_requests_within(t3_ms_t *ms, int nframes)
{
	int n = 0;
	for (int i = 0; i < nframes; i++) {
		nsent = 0;
		frame(ms);
		if (nsent == 1 && sent[0].op == T3_OP_C_RAND && sent[0].u.rand.kind == T3_SK_REGISTRATION) {
			n++;
			t3_csbk_t a = ack(T3_OP_C_ACKD, T3_RC_REG_ACCEPTED, OWN, T3_ID_REGI);
			rx(ms, 1, &a);
		}
	}
	return n;
}

static void test_mass_reg(void)
{
	t3_ms_t ms;
	setup(T3_OPT_PRIVATE, true);
	camp_registered(&ms);
	t3_csbk_t b = mass_reg(3, 24, OWN ^ 1u);	// 2 s, another MS
	rx(&ms, 1, &b);
	CHECK(!ms.massreg && reg_requests_within(&ms, 50) == 0, "another MS's: nothing");
	b = mass_reg(3, 24, OWN);
	rx(&ms, 1, &b);
	CHECK(ms.massreg && ms.massregs == 1, "own address: taken");
	b = mass_reg(3, 0, 0);
	rx(&ms, 1, &b);
	CHECK(ms.massregs == 1, "a second inside the window: ignored");
	CHECK(reg_requests_within(&ms, 40) == 1 && ms.reg == T3_REG_OK && !ms.massreg, "one registration inside 2 s");
	b = mass_reg(2, 4, OWN & 0xFu);		// the low 4 bits match
	rx(&ms, 1, &b);
	CHECK(ms.massreg, "mask 4: taken");
	b = mass_reg(0, 0, 0);
	rx(&ms, 1, &b);
	CHECK(!ms.massreg && reg_requests_within(&ms, 30) == 0, "Reg_Window 0 cancels");
}

// C_MOVE (6.3.3.1 e): another MS's is ignored; one for the radio sends it to the named TSCC, which it uses only once
// an Aloha confirms it; an MBC names the frequencies
static t3_csbk_t move(uint8_t mask, uint32_t ms_addr, uint16_t lpcn)
{
	t3_csbk_t m = { .lb = true, .op = T3_OP_C_MOVE };
	m.u.move.mask = mask;
	m.u.move.ms = ms_addr;
	m.u.move.lpcn = lpcn;
	m.u.move.backoff = 6;
	return m;
}

static void test_move(void)
{
	t3_ms_t ms;
	setup(T3_OPT_PRIVATE, true);
	camp_registered(&ms);

	// The PDU round trip
	uint8_t b[T3_CSBK_BYTES];
	t3_csbk_t m = move(24, OWN, 200), d;
	t3_csbk_build(&m, T3_CRC_MASK_CSBK, b);
	CHECK(t3_csbk_parse(b, &d) && d.op == T3_OP_C_MOVE && d.u.move.mask == 24 && d.u.move.ms == OWN &&
	      d.u.move.lpcn == 200 && d.u.move.backoff == 6, "C_MOVE round trip");

	m = move(24, OWN ^ 2u, 200);
	rx(&ms, 1, &m);
	CHECK(ms.moves == 0 && ms.tscc.rx_hz == RX(TSCC_LCN), "another MS's: ignored");
	m = move(0, 0, 200);	// every radio
	rx(&ms, 1, &m);
	CHECK(ms.moves == 1 && ms.state == T3_CAMPED && !ms.confirmed && tuned.chan.rx_hz == RX(200) &&
	      ms.tscc.rx_hz == RX(200), "moved to LCN 200, rx %u", tuned.chan.rx_hz);
	nsent = 0;
	frames(&ms, 3);
	CHECK(ms.confirmed && ms.state == T3_CAMPED, "confirmed by an Aloha there");

	// An MBC: the frequencies in the appended block
	t3_csbk_t h = move(24, OWN, T3_LPCN_ABSOLUTE);
	h.lb = false;
	t3_cdef_t cd = { .cdeftype = 0, .chan = 0x123, .tx_hz = 446000000u, .rx_hz = 441500000u, .cc = 3 };
	rx_mbc(&ms, 1, &h, &cd);
	CHECK(ms.moves == 2 && tuned.chan.rx_hz == 441500000u && ms.tscc.cc == 3, "MBC move, rx %u",
	      tuned.chan.rx_hz);
}

// Power save (6.4.7): PowerSave_RQ goes in the registration; the PowerSave_Offset of Reg_Accepted names the wake
// frame. After T_Awake the radio sleeps outside its frames (two CSC steps early); traffic for it wakes it
static void test_power_save(void)
{
	t3_ms_t ms;
	setup(T3_OPT_PRIVATE, true);
	cfg.ps_rq = 2;		// 1:4
	camp(&ms);
	for (int i = 0; i < 10 && nsent == 0; i++) {
		frame(&ms);
	}
	CHECK(nsent == 1 && sent[0].u.rand.kind == T3_SK_REGISTRATION && ((sent[0].u.rand.options >> 1) & 7u) == 2u,
	      "PowerSave_RQ 2 asked, options %02x", sent[0].u.rand.options);
	t3_csbk_t a = ack(T3_OP_C_ACKD, T3_RC_REG_ACCEPTED, OWN, T3_ID_REGI);
	a.u.ack.info = 2;
	rx(&ms, 1, &a);
	CHECK(ms.reg == T3_REG_OK && ms.ps_rq == 2 && ms.ps_offset == 2, "wake frame 2 of 4");
	t3_sysparms_t sp = { true, T3_MODEL_LARGE, (uint16_t)(t3_syscode_14(ours) & 0xFFF), true, 0 };
	t3_ms_slc(&ms, T, t3_sysparms_build(&sp));
	uint32_t t0 = T;
	CHECK(!ms.asleep, "awake right after registering");
	frames(&ms, 40);	// T_Awake (2 s) runs out
	int wrong = 0, asleep = 0;
	for (int i = 0; i < 64; i++) {
		frame(&ms);
		uint32_t step = ((T - t0 + 120u) / 120u) & 511u;	// CSC 0's slots ended as it was heard at t0
		bool awake = ((step >> 2) & 3u) == 2u || ((step & 3u) >= 2u && (((step >> 2) + 1u) & 3u) == 2u);
		wrong += ms.asleep == awake;
		asleep += ms.asleep;
	}
	CHECK(wrong == 0 && asleep > 30 && ms.sleeps >= 2, "asleep outside the wake frame: %d wrong, %d asleep", wrong,
	      asleep);
	CHECK(ms.state == T3_CAMPED, "T_Nosig counts only awake time");
	// The receiver goes off a little after the engine sleeps: an Aloha heard between two polls of a sleep must not move
	// the last valid PDU past now (read as T_Nosig long ago: a hunt). The level read with the receiver off is no signal
	int hunts = (int)ms.st.hunts, heard = 0;
	for (int i = 0; i < 64; i++) {
		T += 30;
		t3_ms_slot(&ms, T, 1, at_known, at_busy);
		poll_all(&ms);
		bool asleep = ms.asleep;
		T += 10;
		send_aloha(&ms);
		t3_ms_level(&ms, T, asleep ? -125 : -60);
		heard += asleep;
		T += 20;
		t3_ms_slot(&ms, T, 2, at_known, at_busy);
		poll_all(&ms);
	}
	CHECK(heard > 10 && ms.state == T3_CAMPED && (int)ms.st.hunts == hunts, "a PDU heard asleep: %d, hunts %u",
	      heard, (unsigned)ms.st.hunts - (unsigned)hunts);
	CHECK(ms.level_dbm >= -61, "the level asleep not taken: %d dBm", ms.level_dbm);
	// An AHOY to the radio wakes it for T_Awake
	t3_csbk_t h = ahoy(T3_SK_IND_VOICE, false, OWN, 1234u);
	rx(&ms, 1, &h);
	frame(&ms);
	CHECK(!ms.asleep, "woken by traffic");
	// A poll to the sleeping radio, in its wake frame: answered (no reply set: Not_Supported)
	frames(&ms, 80);
	bool was = false;
	for (int i = 0; i < 80 && !(was && !ms.asleep); i++) {
		was = ms.asleep;
		frame(&ms);
	}
	CHECK(was && !ms.asleep, "woken for the wake frame");
	nsent = 0;
	h = ahoy(T3_SK_UDT_POLL, false, OWN, 1234u);
	h.u.ahoy.options = T3_UDTF_ISO8;
	rx(&ms, 1, &h);
	frames(&ms, 2);
	CHECK(nsent == 1 && sent[0].op == T3_OP_C_ACKU && sent[0].u.ack.reason == T3_RC_MS_NOT_SUPPORTED,
	      "a poll in the wake frame answered: %d sent, asleep %d", nsent, ms.asleep);

	// Offset 0: registered, no power save
	setup(T3_OPT_PRIVATE, true);
	cfg.ps_rq = 3;
	nsent = 0;
	camp(&ms);
	for (int i = 0; i < 10 && nsent == 0; i++) {
		frame(&ms);
	}
	a.u.ack.info = 0;
	rx(&ms, 1, &a);
	t3_ms_slc(&ms, T, t3_sysparms_build(&sp));
	frames(&ms, 100);
	CHECK(ms.reg == T3_REG_OK && ms.ps_offset == 0 && ms.sleeps == 0, "offset 0: never asleep");
}

static void udt_down(t3_ms_t *ms, uint8_t slot, const t3_udt_t *h, const uint8_t *data, uint8_t len, bool corrupt);

// The radio authenticates the TSCC (6.4.9.2, 6.4.10): with K, a stun, revive or kill C_AHOY is acknowledged by a
// C_ACKVIT carrying the radio's challenge; the TSCC's right answer (C_ACKD Authentication_Response) is acted on and
// acknowledged MS_Accepted, a wrong one refused (Recipient_Refused) and ignored. Kill only with T3_OPT_KILL
// The composite TSCC's CACH while its slot is lent to a call (5.3.2, table 7.34)
static void lent_slc(t3_ms_t *ms, bool composite)
{
	t3_sysparms_t sp = { false, T3_MODEL_LARGE, (uint16_t)(t3_syscode_14(ours) & 0xFFF), composite, 0 };
	t3_ms_slc(ms, T, t3_sysparms_build(&sp));
}

// A one-carrier site lends its TSCC's slot to a call when no payload channel is free (5.3.2)
static void test_composite(void)
{
	setup(T3_OPT_PRIVATE, false);
	t3_ms_t ms;
	camp(&ms);
	// Lent: P_SYS_Parms "composite" on the CACH, no Aloha. The radio waits for the TSCC, past T_Nosig, and asks
	// nothing meanwhile
	aloha_on = false;
	lent_slc(&ms, true);
	int t0 = ntune;
	for (int i = 0; i < 200; i++) {		// 12 s
		if (i == 180) {
			CHECK(t3_ms_request(&ms, T, TG_A, true, false), "call request");
		}
		frame(&ms);
		if (i % 2 == 0) {
			lent_slc(&ms, true);
		}
	}
	CHECK(ms.state == T3_CAMPED && ntune == t0 && ms.st.lent == 101, "waits on the lent TSCC (lent %u)",
	      ms.st.lent);
	CHECK(nsent == 0, "no random access while lent, %d sent", nsent);
	// Back with an Aloha: the request goes
	aloha_on = true;
	frames(&ms, 4);
	CHECK(nsent == 1 && sent[0].op == T3_OP_C_RAND, "request after the Aloha, %d sent", nsent);
	// The grant names the TSCC's own channel and slot: the call is there, and P_CLEAR to channel 0 returns to the
	// TSCC on the same carrier
	t3_csbk_t g = grant(T3_OP_TV_GRANT, TSCC_LCN, 1, TG_A, OWN);
	rx(&ms, 1, &g);
	CHECK(ms.state == T3_FOLLOW && tuned.chan.rx_hz == RX(TSCC_LCN) && tuned.slot == 1 && tuned.payload,
	      "on the TSCC's own slot");
	frames(&ms, 3);
	t3_csbk_t c = { .lb = true, .op = T3_OP_P_CLEAR };
	c.u.clear.group = true;
	c.u.clear.dst = TG_A;
	c.u.clear.lpcn = T3_LPCN_NULL;
	rx(&ms, 1, &c);
	CHECK(ms.state == T3_CAMPED && tuned.chan.rx_hz == RX(TSCC_LCN) && !tuned.payload, "back on the TSCC");

	// A normal payload channel's P_SYS_Parms (type 0) doesn't hold the radio: T_Nosig, then hunt
	setup(T3_OPT_PRIVATE, false);
	camp(&ms);
	aloha_on = false;
	for (int i = 0; i < 200 && ms.state == T3_CAMPED; i++) {
		frame(&ms);
		if (i % 2 == 0) {
			lent_slc(&ms, false);
		}
	}
	CHECK(ms.state == T3_HUNT && ms.st.lent == 0, "type 0: hunt");
	// Nor does another system's composite one
	setup(T3_OPT_PRIVATE, false);
	camp(&ms);
	aloha_on = false;
	t3_syscode_t o = { T3_MODEL_LARGE, 13, 5, T3_PAR_AB };
	t3_sysparms_t sp = { false, T3_MODEL_LARGE, (uint16_t)(t3_syscode_14(t3_syscode_join(&o)) & 0xFFF), true, 0 };
	for (int i = 0; i < 200 && ms.state == T3_CAMPED; i++) {
		frame(&ms);
		t3_ms_slc(&ms, T, t3_sysparms_build(&sp));
	}
	CHECK(ms.state == T3_HUNT && ms.st.lent == 0, "another system's: hunt");
	// The lent mark ends: back to T_Nosig once the short LCs stop
	setup(T3_OPT_PRIVATE, false);
	camp(&ms);
	aloha_on = false;
	lent_slc(&ms, true);
	for (int i = 0; i < 200 && ms.state == T3_CAMPED; i++) {
		frame(&ms);
	}
	CHECK(ms.state == T3_HUNT, "nothing after the lent mark: hunt");
}

// A modem without the CACH's short LC can't hear P_SYS_Parms "composite": a call's bursts on the TSCC's slot in its
// color code say the slot is lent instead (5.3.2), until a short LC is heard there. Bursts on the other slot, in another
// color code, or after a short LC don't hold the radio
static void test_composite_no_slc(void)
{
	static const struct {
		uint8_t slot, cc;
		bool slc_first, holds;
		const char *what;
	} cases[] = {
		{ 1, CC, false, true, "TSCC slot: lent" },
		{ 2, CC, false, false, "the other slot" },
		{ 1, CC + 1, false, false, "another color code" },
		{ 1, CC, true, false, "a short LC heard first" },
	};
	for (unsigned k = 0; k < sizeof cases / sizeof cases[0]; k++) {
		setup(T3_OPT_PRIVATE, false);
		t3_ms_t ms;
		camp(&ms);
		if (cases[k].slc_first) {
			t3_sysparms_t sp = { true, T3_MODEL_LARGE, (uint16_t)(t3_syscode_14(ours) & 0xFFF), false, 0 };
			t3_ms_slc(&ms, T, t3_sysparms_build(&sp));
		}
		aloha_on = false;
		int t0 = ntune;
		for (int i = 0; i < 200 && ms.state == T3_CAMPED; i++) {	// 12 s, past T_Nosig
			if (i == 180) {
				CHECK(t3_ms_request(&ms, T, TG_A, true, false), "%s: call request", cases[k].what);
			}
			frame(&ms);
			t3_ms_call_burst(&ms, T, cases[k].slot, cases[k].cc);
		}
		if (cases[k].holds) {
			CHECK(ms.state == T3_CAMPED && ntune == t0 && ms.st.lent_calls == 200 && nsent == 0,
			      "%s: waits, no access (lent_calls %u, %d sent)", cases[k].what, ms.st.lent_calls, nsent);
			aloha_on = true;
			frames(&ms, 4);
			CHECK(nsent == 1 && sent[0].op == T3_OP_C_RAND, "%s: request after the Aloha", cases[k].what);
		} else {
			CHECK(ms.state == T3_HUNT && ms.st.lent_calls == 0, "%s: hunt", cases[k].what);
		}
	}
}

// A radio that missed a call's grant (switched on during it) and waits on a lent composite TSCC: in the lent call's
// hang time the site repeats the live group calls' grants there with Late_Entry 1 (6.6.2.4.1). The radio follows one
// for its talk groups, the call on the payload slot or the lent call itself, and not one for another talk group
static void test_composite_late_entry(void)
{
	static const struct {
		uint8_t slot;
		uint32_t dst;
		bool follows;
		const char *what;
	} cases[] = {
		{ 2, TG_A, true, "the payload slot's call" },
		{ 1, TG_B, true, "the lent call" },
		{ 2, TG_OTHER, false, "another talk group's" },
	};
	for (unsigned k = 0; k < sizeof cases / sizeof cases[0]; k++) {
		setup(T3_OPT_PRIVATE, false);
		t3_ms_t ms;
		camp(&ms);
		aloha_on = false;
		for (int i = 0; i < 10; i++) {
			frame(&ms);
			lent_slc(&ms, true);
		}
		CHECK(ms.state == T3_CAMPED && ms.st.lent > 0, "%s: lent", cases[k].what);
		t3_csbk_t g = grant(T3_OP_TV_GRANT, TSCC_LCN, cases[k].slot, cases[k].dst, 1000001u);
		g.u.grant.late_entry = true;
		rx(&ms, 1, &g);
		if (cases[k].follows) {
			CHECK(ms.state == T3_FOLLOW && tuned.chan.rx_hz == RX(TSCC_LCN) && tuned.slot == cases[k].slot &&
			      tuned.payload && ms.call.dst == cases[k].dst, "%s: followed", cases[k].what);
		} else {
			CHECK(ms.state == T3_CAMPED && !tuned.payload, "%s: stays", cases[k].what);
			// Nor does the grant end the lent mark: a call request still waits for the TSCC
			nsent = 0;
			CHECK(t3_ms_request(&ms, T, TG_A, true, false), "%s: call request", cases[k].what);
			frames(&ms, 4);		// 240 ms, inside T3_LENT_MS of the last short LC: no new one needed
			CHECK(nsent == 0 && ms.state == T3_CAMPED, "%s: still lent, %d sent", cases[k].what, nsent);
		}
	}
}

static void test_acvit(void)
{
	t3_ms_t ms;
	setup(T3_OPT_PRIVATE | T3_OPT_KILL | T3_OPT_STUN, true);
	cfg.auth_key_set = true;
	for (int i = 0; i < 16; i++) {
		cfg.auth_key[i] = (uint8_t)(i + 1);
	}
	camp_registered(&ms);
	nsent = 0;
	t3_csbk_t h = ahoy(T3_SK_SUPPLEMENTARY, false, OWN, T3_ID_STUNI);
	rx(&ms, 1, &h);
	frames(&ms, 1);
	CHECK(nsent == 1 && sent[0].op == T3_OP_C_ACKVIT && sent[0].u.ahoy.kind == T3_SK_SUPPLEMENTARY &&
	      !sent[0].u.ahoy.kind_flag && sent[0].u.ahoy.src == OWN && sent[0].u.ahoy.dst == ms.acvit_challenge &&
	      sent[0].u.ahoy.dst <= T3_AUTH_CHALLENGE_MAX, "stun: C_ACKVIT with the challenge (op %u)", sent[0].op);
	CHECK(!ms.stunned, "not stunned before the TSCC answers");
	uint32_t ok = t3_auth_response(cfg.auth_key, ms.acvit_challenge, 0);
	t3_csbk_t a = ack(T3_OP_C_ACKD, T3_RC_AUTH_RESPONSE, OWN, ok ^ 1u);
	rx(&ms, 1, &a);
	frames(&ms, 1);
	CHECK(nsent == 2 && sent[1].op == T3_OP_C_ACKU && sent[1].u.ack.reason == T3_RC_RECIPIENT_REFUSED &&
	      sent[1].u.ack.dst == T3_ID_STUNI && !ms.stunned && ms.tscc_auth_fails == 1, "a wrong answer refused");
	a = ack(T3_OP_C_ACKD, T3_RC_AUTH_RESPONSE, OWN, ok);
	rx(&ms, 1, &a);
	frames(&ms, 1);
	CHECK(nsent == 3 && sent[2].u.ack.reason == T3_RC_MS_ACCEPTED && sent[2].u.ack.dst == T3_ID_STUNI &&
	      ms.stunned && ms.stuns == 1, "the right one: stunned");
	rx(&ms, 1, &a);
	frames(&ms, 1);
	CHECK(nsent == 4 && sent[3].u.ack.reason == T3_RC_MS_ACCEPTED && ms.stuns == 1, "repeated: answered, once");
	// The repeated C_AHOY gets the same challenge
	uint32_t c = ms.acvit_challenge;
	rx(&ms, 1, &h);
	frames(&ms, 1);
	CHECK(nsent == 5 && sent[4].op == T3_OP_C_ACKVIT && sent[4].u.ahoy.dst == c, "same challenge again");

	// Revive (Service_Kind_Flag 1)
	h.u.ahoy.kind_flag = true;
	rx(&ms, 1, &h);
	frames(&ms, 1);
	CHECK(nsent == 6 && sent[5].op == T3_OP_C_ACKVIT && sent[5].u.ahoy.kind_flag, "revive: C_ACKVIT, flag 1");
	a = ack(T3_OP_C_ACKD, T3_RC_AUTH_RESPONSE, OWN, t3_auth_response(cfg.auth_key, ms.acvit_challenge, 0));
	rx(&ms, 1, &a);
	frames(&ms, 1);
	CHECK(!ms.stunned && ms.revives == 1 && sent[6].u.ack.reason == T3_RC_MS_ACCEPTED, "revived");

	// Kill: to KILLI; afterwards nothing is heard or asked
	t3_csbk_t k = ahoy(T3_SK_SUPPLEMENTARY, false, OWN, T3_ID_KILLI);
	rx(&ms, 1, &k);
	frames(&ms, 1);
	CHECK(nsent == 8 && sent[7].op == T3_OP_C_ACKVIT && !sent[7].u.ahoy.kind_flag, "kill: C_ACKVIT");
	a = ack(T3_OP_C_ACKD, T3_RC_AUTH_RESPONSE, OWN, t3_auth_response(cfg.auth_key, ms.acvit_challenge, 0));
	rx(&ms, 1, &a);
	frames(&ms, 1);
	CHECK(nsent == 9 && sent[8].u.ack.reason == T3_RC_MS_ACCEPTED && sent[8].u.ack.dst == T3_ID_KILLI && ms.killed &&
	      ms.kills == 1, "killed");
	t3_csbk_t r = ahoy(T3_SK_REGISTRATION, false, OWN, T3_ID_TSI);
	rx(&ms, 1, &r);
	frames(&ms, 2);
	CHECK(nsent == 9 && !t3_ms_request(&ms, T, CALLER, false, false), "killed: deaf and dumb");

	// Without T3_OPT_KILL (or without K): not supported
	setup(T3_OPT_PRIVATE, true);
	cfg.auth_key_set = true;
	camp_registered(&ms);
	nsent = 0;
	rx(&ms, 1, &k);
	frames(&ms, 1);
	CHECK(nsent == 1 && sent[0].op == T3_OP_C_ACKU && sent[0].u.ack.reason == T3_RC_MS_NOT_SUPPORTED &&
	      !ms.killed, "kill not allowed: not supported");
}

// Ambient Listening (6.6.2.6): the ALS radio check is accepted with Message_Accepted (only with T3_OPT_ALS, else
// MSNot_Supported); the caller's private grant in T_ALS_REQUEST_LIFE_SPAN is an ALS call the radio transmits on until
// T_ALS; late, or from someone else, it is an ordinary call. Asking for one sets ALS_SERV in the C_RAND
static void test_als(void)
{
	t3_ms_t ms;
	setup(T3_OPT_PRIVATE, true);
	camp_registered(&ms);
	nsent = 0;
	t3_csbk_t h = ahoy(T3_SK_IND_VOICE, false, OWN, CALLER);
	h.u.ahoy.als = true;
	rx(&ms, 1, &h);
	frames(&ms, 1);
	CHECK(nsent == 1 && sent[0].op == T3_OP_C_ACKU && sent[0].u.ack.reason == T3_RC_MS_NOT_SUPPORTED &&
	      sent[0].u.ack.dst == CALLER, "ALS off: not supported");

	setup(T3_OPT_PRIVATE | T3_OPT_ALS, true);
	cfg.als_s = 12;
	camp_registered(&ms);
	nsent = 0;
	rx(&ms, 1, &h);
	frames(&ms, 1);
	CHECK(nsent == 1 && sent[0].u.ack.reason == T3_RC_MESSAGE_ACCEPTED, "ALS radio check accepted: 0x%02x",
	      sent[0].u.ack.reason);
	t3_csbk_t g = grant(T3_OP_PV_GRANT, 159, 1, OWN, CALLER);
	rx(&ms, 1, &g);
	CHECK(ms.state == T3_FOLLOW && ms.call.als && ms.als_calls == 1, "the grant is an ALS call");
	frames(&ms, 2);
	CHECK(t3_ms_als_tx(&ms, T) && !t3_ms_als_over(&ms, T), "transmit");
	uint32_t at = ms.call.als_end;
	CHECK(at - ms.call.start_ms == 12000u, "T_ALS from the config");
	CHECK(!t3_ms_als_tx(&ms, at) && t3_ms_als_over(&ms, at), "T_ALS over");
	t3_ms_end_call(&ms, at);
	frames(&ms, 12);
	CHECK(ms.state == T3_CAMPED, "the radio ended it");

	// Too late: an ordinary private call
	rx(&ms, 1, &h);
	frames(&ms, 1);
	frames(&ms, T3_ALS_REQUEST_MS / 60u + 2u);
	rx(&ms, 1, &g);
	CHECK(ms.state == T3_FOLLOW && !ms.call.als && !t3_ms_als_tx(&ms, T), "after the life span: not ALS: state %d als %d sent %d ax %d", ms.state, ms.call.als, nsent, ms.ax.state);
	frames(&ms, 2);
	t3_ms_end_call(&ms, T);
	frames(&ms, 12);
	// From another caller: not ALS
	rx(&ms, 1, &h);
	frames(&ms, 1);
	g = grant(T3_OP_PV_GRANT, 159, 2, OWN, CALLER + 1u);	// not the call just left (T3_REGRANT_MS)
	rx(&ms, 1, &g);
	CHECK(ms.state == T3_FOLLOW && !ms.call.als, "another caller: not ALS: state %d als %d", ms.state, ms.call.als);
	frames(&ms, 2);
	t3_ms_end_call(&ms, T);
	frames(&ms, 12);

	// An emergency one: T_ALS_E
	rx(&ms, 1, &h);
	frames(&ms, 1);
	g = grant(T3_OP_PV_GRANT, 159, 1, OWN, CALLER);
	g.u.grant.emergency = true;
	rx(&ms, 1, &g);
	CHECK(ms.call.als && ms.call.als_end - ms.call.start_ms == T3_ALS_E_S * 1000u, "T_ALS_E: state %d als %d", ms.state, ms.call.als);
	frames(&ms, 2);
	t3_ms_end_call(&ms, T);
	frames(&ms, 12);

	// Asking: C_RAND individual voice with ALS_SERV
	nsent = 0;
	CHECK(t3_ms_request_als(&ms, T, CALLER, false) && sent_rand(&ms), "ALS asked");
	CHECK(sent[0].u.rand.kind == T3_SK_IND_VOICE && (sent[0].u.rand.extra & 0x2u) && sent[0].u.rand.options == 0,
	      "ALS_SERV set: extra %u", sent[0].u.rand.extra);
	t3_csbk_t n = ack(T3_OP_C_ACKD, T3_RC_MS_AWAY, OWN, CALLER);
	rx(&ms, 1, &n);
	nsent = 0;
	CHECK(t3_ms_request(&ms, T, CALLER, false, false) && sent_rand(&ms) && !(sent[0].u.rand.extra & 0x2u),
	      "an ordinary call has no ALS_SERV");
}

// Payload interrupt (6.4.14.3.3): from a call another MS talks on, status 125 to the talker on the TSCC (the talkgroup,
// or the other MS), then back to the call whatever the answer. ALS termination (6.4.14.3.4.3): status 123 to the MS
// the radio listens to, then it stays on the TSCC
static void test_interrupt(void)
{
	t3_ms_t ms;
	setup(T3_OPT_PRIVATE, true);
	camp_registered(&ms);
	CHECK(!t3_ms_interrupt(&ms, T, false) && ms.ax.result == T3_RES_NOT_ALLOWED, "camped: nothing to interrupt");

	t3_csbk_t g = grant(T3_OP_TV_GRANT, 159, 1, TG_A, CALLER);
	rx(&ms, 1, &g);
	frames(&ms, 2);
	CHECK(ms.state == T3_FOLLOW && tuned.payload, "following a talkgroup call");
	t3_act_t call_tune = tuned;
	CHECK(!t3_ms_interrupt(&ms, T, true), "status 123 only in an ALS call the radio asked for");
	nsent = 0;
	int nt = ntune;
	CHECK(t3_ms_interrupt(&ms, T, false), "interrupt starts");
	CHECK(sent_rand(&ms), "status request sent");
	CHECK(ms.state == T3_CAMPED && ntune == nt + 1 && !tuned.payload, "on the TSCC to ask");
	const t3_rand_t *q = &sent[0].u.rand;
	CHECK(q->kind == T3_SK_STATUS && q->options == (0x40 | (125 >> 2)) && (q->extra & 3u) == (125 & 3) &&
	      q->dst == TG_A && q->src == OWN, "status 125 to the talkgroup: options 0x%02x extra %u", q->options,
	      q->extra);
	CHECK(ms.own_status != 125, "not the radio's own status");
	// Another call's grant meanwhile doesn't take the radio
	t3_csbk_t o = grant(T3_OP_TV_GRANT, 160, 2, TG_B, CALLER + 1u);
	rx(&ms, 1, &o);
	CHECK(ms.state == T3_CAMPED, "another grant ignored while asking");
	t3_csbk_t a = ack(T3_OP_C_ACKD, T3_RC_MESSAGE_ACCEPTED, OWN, TG_A);
	rx(&ms, 1, &a);
	CHECK(ms.ax.result == T3_RES_DONE && ms.state == T3_FOLLOW && tuned.payload &&
	      tuned.chan.rx_hz == call_tune.chan.rx_hz && tuned.slot == call_tune.slot && ms.call.dst == TG_A,
	      "accepted: back in the call, result %s", t3_res_name(ms.ax.result));

	// Refused: back all the same
	frames(&ms, 2);
	nsent = 0;
	CHECK(t3_ms_interrupt(&ms, T, false) && sent_rand(&ms), "again");
	a = ack(T3_OP_C_ACKD, T3_RC_NOT_SUPPORTED, OWN, TG_A);
	rx(&ms, 1, &a);
	CHECK(ms.ax.result == T3_RES_REFUSED && ms.state == T3_FOLLOW && tuned.payload, "refused: back in the call");

	// An individual call: to the other MS; no answer, back all the same
	t3_ms_end_call(&ms, T);
	frames(&ms, 12);
	g = grant(T3_OP_PV_GRANT, 159, 2, OWN, CALLER + 2u);
	rx(&ms, 1, &g);
	frames(&ms, 2);
	CHECK(ms.state == T3_FOLLOW && !ms.call.group, "following a private call");
	nsent = 0;
	CHECK(t3_ms_interrupt(&ms, T, false) && sent_rand(&ms), "interrupt in a private call");
	q = &sent[0].u.rand;
	CHECK(q->dst == CALLER + 2u && !(q->options & 0x40u), "status 125 to the other MS: dst %u options 0x%02x",
	      (unsigned)q->dst, q->options);
	for (int i = 0; i < 400 && ms.ax.state != T3_AX_IDLE; i++) {
		frame(&ms);
	}
	CHECK(ms.ax.state == T3_AX_IDLE && ms.ax.result != T3_RES_DONE && ms.state == T3_FOLLOW && tuned.payload,
	      "no answer: back in the call, result %s", t3_res_name(ms.ax.result));

	// An ALS call the radio asked for: status 123, then it stays on the TSCC
	t3_ms_end_call(&ms, T);
	frames(&ms, 12);
	nsent = 0;
	CHECK(t3_ms_request_als(&ms, T, CALLER, false) && sent_rand(&ms), "ALS asked");
	g = grant(T3_OP_PV_GRANT, 159, 1, CALLER, OWN);
	rx(&ms, 1, &g);
	frames(&ms, 2);
	CHECK(ms.state == T3_FOLLOW && ms.call.als_ours && !ms.call.als, "listening to an ALS call");
	nsent = 0;
	CHECK(t3_ms_interrupt(&ms, T, true) && sent_rand(&ms), "ALS end starts");
	q = &sent[0].u.rand;
	CHECK(q->kind == T3_SK_STATUS && q->options == (123 >> 2) && (q->extra & 3u) == (123 & 3) && q->dst == CALLER,
	      "status 123 to the ALS MS: options 0x%02x extra %u", q->options, q->extra);
	a = ack(T3_OP_C_ACKD, T3_RC_MESSAGE_ACCEPTED, OWN, CALLER);
	rx(&ms, 1, &a);
	frames(&ms, 2);
	CHECK(ms.ax.result == T3_RES_DONE && ms.state == T3_CAMPED && !tuned.payload, "ALS ended: stays on the TSCC");

	// Listen only: never
	setup(T3_OPT_PRIVATE | T3_OPT_LISTEN_ONLY, true);
	camp(&ms);
	g = grant(T3_OP_TV_GRANT, 159, 1, TG_A, CALLER);
	rx(&ms, 1, &g);
	CHECK(ms.state == T3_FOLLOW && !t3_ms_interrupt(&ms, T, false) && ms.state == T3_FOLLOW, "listen only: no interrupt");
}

// UDT short data polling (6.6.5): asking (C_RAND kind UDT poll, POL_FMT, blocks less one; the C_AHOY to the polled MS is
// progress; the data from it ends it, acknowledged MS_Accepted) and answering (the reply in the format asked, cut to the
// blocks asked; none, or another format: C_NACKU)
static void test_sdpoll(void)
{
	t3_ms_t ms;
	static const uint8_t reply[] = "position 33.5N 86.8W, all well";
	setup(T3_OPT_PRIVATE, true);
	cfg.poll_data = reply;
	cfg.poll_len = (uint8_t)(sizeof(reply) - 1u);
	cfg.poll_format = T3_UDTF_ISO8;
	camp_registered(&ms);

	CHECK(!t3_ms_sdata_poll(&ms, T, CALLER, T3_UDTF_ISO8, 0) && ms.ax.result == T3_RES_NOT_ALLOWED, "0 blocks");
	CHECK(!t3_ms_sdata_poll(&ms, T, CALLER, T3_UDTF_ISO8, 5), "5 blocks");
	nsent = 0;
	CHECK(t3_ms_sdata_poll(&ms, T, CALLER, T3_UDTF_ISO8, 2) && sent_rand(&ms), "poll asked");
	const t3_rand_t *q = &sent[0].u.rand;
	CHECK(q->kind == T3_SK_UDT_POLL && q->options == T3_UDTF_ISO8 && (q->extra & 3u) == 1u && q->dst == CALLER,
	      "C_RAND: kind %u options 0x%02x extra %u", q->kind, q->options, q->extra);
	t3_csbk_t h = ahoy(T3_SK_UDT_POLL, false, CALLER, OWN);
	h.u.ahoy.options = T3_UDTF_ISO8;
	h.u.ahoy.blocks = 1;
	rx(&ms, 1, &h);
	CHECK(ms.ax.state == T3_AX_ACKED && ms.ax.result == T3_RES_PENDING, "our poll under way");
	static const uint8_t got[] = "hello";
	t3_udt_t d = { .op = T3_OP_C_UDTHD, .format = T3_UDTF_ISO8, .dst = OWN, .src = CALLER };
	nsent = 0;
	udt_down(&ms, 1, &d, got, 5, false);
	frames(&ms, 1);
	CHECK(ms.ax.result == T3_RES_DONE && ms.sd_rx.len >= 5 && memcmp(ms.sd_rx.data, got, 5) == 0 &&
	      ms.sd_rx.src == CALLER, "polled data in: %s", t3_res_name(ms.ax.result));
	CHECK(nsent == 1 && sent[0].op == T3_OP_C_ACKU && sent[0].u.ack.reason == T3_RC_MS_ACCEPTED &&
	      sent[0].u.ack.dst == CALLER, "final acknowledgement");

	// Polled: one block asked, so the reply is cut to 10 octets
	nup = 0;
	h = ahoy(T3_SK_UDT_POLL, false, OWN, CALLER);
	h.u.ahoy.options = T3_UDTF_ISO8;
	h.u.ahoy.blocks = 0;
	rx(&ms, 1, &h);
	frames(&ms, 1);
	t3_udt_t u;
	CHECK(nup == 1 && nup_blocks == 1 && t3_udt_parse(up_hdr, &u) && u.op == T3_OP_C_UDTHU && u.dst == CALLER &&
	      u.src == OWN && u.format == T3_UDTF_ISO8 && u.blocks == 1, "reply uploaded: %d", nup);
	CHECK(ms.ax.state == T3_AX_IDLE, "no access of its own");
	// Four blocks asked: all of it, in three
	nup = 0;
	h.u.ahoy.blocks = 3;
	h.u.ahoy.src = T3_ID_SDMI;
	rx(&ms, 1, &h);
	frames(&ms, 1);
	CHECK(nup == 1 && t3_udt_parse(up_hdr, &u) && u.blocks == 3 && u.dst == T3_ID_SDMI, "gateway poll: %u blocks",
	      u.blocks);
	// Another format: refused
	nup = 0;
	nsent = 0;
	h.u.ahoy.options = T3_UDTF_NMEA;
	rx(&ms, 1, &h);
	frames(&ms, 1);
	CHECK(nup == 0 && nsent == 1 && sent[0].op == T3_OP_C_ACKU && sent[0].u.ack.reason == T3_RC_MS_NOT_SUPPORTED,
	      "other format: not supported");
	cfg.poll_len = 0;
	nsent = 0;
	h.u.ahoy.options = T3_UDTF_ISO8;
	rx(&ms, 1, &h);
	frames(&ms, 1);
	CHECK(nup == 0 && nsent == 1 && sent[0].u.ack.reason == T3_RC_MS_NOT_SUPPORTED, "no reply set: not supported");
	cfg.poll_data = NULL;
}

// MS_Not_Registered (6.4.4.1.12): the TSCC lost the radio's record; it registers again. A registration demand (a
// MassReg to its address, Mask 24) does the same
static void test_not_registered(void)
{
	t3_ms_t ms;
	setup(T3_OPT_PRIVATE, true);
	camp_registered(&ms);
	nsent = 0;
	CHECK(t3_ms_request(&ms, T, TG_A, true, false) && sent_rand(&ms), "request");
	t3_csbk_t a = ack(T3_OP_C_ACKD, T3_RC_MS_NOT_REGISTERED, OWN, TG_A);
	rx(&ms, 1, &a);
	CHECK(ms.reg == T3_REG_NEEDED && ms.ax.svc == T3_SVC_REGISTER, "refused, registration needed: reg %d svc %d",
	      ms.reg, ms.ax.svc);
	nsent = 0;
	CHECK(sent_rand(&ms) && sent[0].u.rand.kind == T3_SK_REGISTRATION, "registers again");
	CHECK(!t3_ms_request(&ms, T, TG_A, true, false), "nothing else meanwhile");
	a = ack(T3_OP_C_ACKD, T3_RC_REG_ACCEPTED, OWN, T3_ID_TSI);
	rx(&ms, 1, &a);
	CHECK(ms.reg == T3_REG_OK, "registered: reg %d", ms.reg);
	// A registration demand to this address
	int before = (int)ms.massregs;
	t3_csbk_t m = mass_reg(2, 24, OWN);
	rx(&ms, 1, &m);
	nsent = 0;
	CHECK(ms.massregs == (uint32_t)before + 1u && sent_rand(&ms) && sent[0].u.rand.kind == T3_SK_REGISTRATION,
	      "registration demand answered");
	// To another address: not this radio
	a = ack(T3_OP_C_ACKD, T3_RC_REG_ACCEPTED, OWN, T3_ID_TSI);
	rx(&ms, 1, &a);
	m = mass_reg(2, 24, OWN + 1u);
	rx(&ms, 1, &m);
	CHECK(ms.massregs == (uint32_t)before + 1u, "another MS's demand ignored");
}

// Call diversion (6.6.7): set to an MS (C_RAND kind 1000b, DIVONOFF 1, target MSI), the address uploaded when DIVERTI
// asks (Address format, UDT_DIV, to MSI), accepted; clear (to TSI), clear incoming (to DIVERTI); a call answered with
// the callee's diverted address ends DIVERTED
static void test_divert(void)
{
	t3_ms_t ms;
	setup(T3_OPT_PRIVATE, true);
	camp_registered(&ms);
	nsent = 0;
	nup = 0;
	CHECK(t3_ms_divert(&ms, T, T3_DIV_VOICE | T3_DIV_SDATA, 1234567u, false), "diversion starts");
	CHECK(sent_rand(&ms), "diversion request sent");
	const t3_rand_t *q = &sent[0].u.rand;
	CHECK(q->kind == T3_SK_DIVERT && q->options == (T3_SO_DIV_SET | T3_DIV_VOICE | T3_DIV_SDATA) &&
	      q->dst == T3_ID_MSI && q->src == OWN && (q->extra & 3u) == 0, "C_RAND diversion: options 0x%02x dst %06x",
	      q->options, (unsigned)q->dst);
	t3_csbk_t h = ahoy(T3_SK_DIVERT, false, OWN, T3_ID_DIVERTI);
	rx(&ms, 1, &h);
	frames(&ms, 1);
	t3_udt_t u;
	CHECK(nup == 1 && nup_blocks == 1 && t3_udt_parse(up_hdr, &u), "address uploaded: %d", nup);
	CHECK(u.op == T3_OP_C_UDTHU && !u.group && u.opt && !u.sf && !u.a && u.format == T3_UDTF_ADDRESS &&
	      u.dst == T3_ID_MSI && u.src == OWN && u.blocks == 1, "C_UDTHU: dst %06x format %u", (unsigned)u.dst,
	      u.format);
	CHECK(up_blocks[0][0] == 0 && up_blocks[0][1] == 0x12 && up_blocks[0][2] == 0xD6 && up_blocks[0][3] == 0x87 &&
	      up_blocks[0][4] == 0 && t3_udt_crc_ok((const uint8_t (*)[T3_UDT_BLOCK])up_blocks, 1), "ADDRESS1");
	t3_csbk_t a = ack(T3_OP_C_ACKD, T3_RC_MESSAGE_ACCEPTED, OWN, T3_ID_MSI);
	rx(&ms, 1, &a);
	CHECK(ms.ax.result == T3_RES_DONE, "diversion set: %s", t3_res_name(ms.ax.result));

	// To a talkgroup, refused for that destination (0x23)
	nsent = 0;
	CHECK(t3_ms_divert(&ms, T, T3_DIV_ALL, TG_A, true), "diversion to a talkgroup");
	CHECK(sent_rand(&ms) && sent[0].u.rand.dst == T3_ID_GPI, "target GPI");
	rx(&ms, 1, &h);
	frames(&ms, 1);
	CHECK(nup == 2 && t3_udt_parse(up_hdr, &u) && u.dst == T3_ID_GPI && !u.group, "address to GPI");
	a = ack(T3_OP_C_ACKD, T3_RC_TRANSIENT_REFUSED, OWN, T3_ID_GPI);
	rx(&ms, 1, &a);
	CHECK(ms.ax.result == T3_RES_REFUSED && ms.ax.reason == T3_RC_TRANSIENT_REFUSED, "refused");

	// Clear our own (TSI), then every incoming one (DIVERTI): single part, no upload
	nsent = 0;
	CHECK(t3_ms_divert_clear(&ms, T, T3_DIV_ALL, false), "clear");
	CHECK(sent_rand(&ms) && sent[0].u.rand.dst == T3_ID_TSI && sent[0].u.rand.options == T3_DIV_ALL &&
	      sent[0].u.rand.kind == T3_SK_DIVERT, "clear to TSI, DIVONOFF 0");
	rx(&ms, 1, &h);
	frames(&ms, 1);
	CHECK(nup == 2, "nothing uploaded for a clear");
	a = ack(T3_OP_C_ACKD, T3_RC_MESSAGE_ACCEPTED, OWN, T3_ID_TSI);
	rx(&ms, 1, &a);
	CHECK(ms.ax.result == T3_RES_DONE, "cleared");
	nsent = 0;
	CHECK(t3_ms_divert_clear(&ms, T, T3_DIV_VOICE, true), "clear incoming");
	CHECK(sent_rand(&ms) && sent[0].u.rand.dst == T3_ID_DIVERTI && sent[0].u.rand.options == T3_DIV_VOICE,
	      "clear incoming to DIVERTI");
	a = ack(T3_OP_C_ACKD, T3_RC_MESSAGE_ACCEPTED, OWN, T3_ID_DIVERTI);
	rx(&ms, 1, &a);
	CHECK(ms.ax.result == T3_RES_DONE, "incoming cleared");
	CHECK(!t3_ms_divert(&ms, T, 0, 1234567u, false) && ms.ax.result == T3_RES_NOT_ALLOWED, "no services");
	CHECK(!t3_ms_divert(&ms, T, T3_DIV_VOICE, 0, false) && ms.ax.result == T3_RES_NOT_ALLOWED, "no address");

	// A private call answered diverted (table 6.65: from MSI, supplementary, ADDRESS1)
	nsent = 0;
	CHECK(t3_ms_request(&ms, T, CALLER, false, false) && sent_rand(&ms), "private call asked");
	static const uint8_t to[] = { 0, 0x12, 0xD6, 0x87, 0, 0, 0, 0, 0, 0 };
	t3_udt_t d = { .op = T3_OP_C_UDTHD, .sf = true, .format = T3_UDTF_ADDRESS, .dst = OWN, .src = T3_ID_MSI };
	udt_down(&ms, 1, &d, to, sizeof(to), false);
	frames(&ms, 1);
	CHECK(ms.ax.result == T3_RES_DIVERTED && ms.ax.divert_to == 1234567u && !ms.ax.divert_group &&
	      ms.ax.state == T3_AX_IDLE && ms.state == T3_CAMPED, "diverted: %s to %u", t3_res_name(ms.ax.result),
	      (unsigned)ms.ax.divert_to);
	CHECK(ms.sd_rx.count == 0, "not short data");
	// Without a request of ours it is nothing
	udt_down(&ms, 1, &d, to, sizeof(to), false);
	frames(&ms, 1);
	CHECK(ms.sd_rx.count == 0 && ms.ax.result == T3_RES_DIVERTED, "ignored when idle");
}

// DGNA (6.6.8): a C_DGNAHD download to the radio sets its dynamic talkgroups (address mode, One-key flag), names one
// or adds the 16th (alias mode), deletes them; it is answered MS_Accepted and a DGNA talkgroup's call is followed
static void test_dgna(void)
{
	t3_ms_t ms;
	setup(T3_OPT_PRIVATE, true);
	camp_registered(&ms);
	t3_udt_t u = { .op = T3_OP_C_DGNAHD, .a = true, .format = T3_UDTF_ADDRESS, .dst = OWN, .src = 0xFFFED2u };
	uint8_t d[10] = { 1, 0, 0x01, 0xF4, 0, 0x01, 0xF5, 0, 0, 0 };	// OK, 500, 501, ADRNULL
	nsent = 0;
	udt_down(&ms, 1, &u, d, sizeof(d), false);
	frames(&ms, 2);
	CHECK(ms.ndgna == 2 && ms.dgna[0] == 500 && ms.dgna[1] == 501 && ms.dgna_onekey == 500 && ms.dgnas == 1,
	      "address mode: %u talkgroups, One-key %u", ms.ndgna, (unsigned)ms.dgna_onekey);
	CHECK(nsent == 1 && sent[0].op == T3_OP_C_ACKU && sent[0].u.ack.reason == T3_RC_MS_ACCEPTED, "answered");
	CHECK(t3_ms_wants_group(&ms, 501, false) && !t3_ms_wants_group(&ms, 502, false), "DGNA groups heard");
	t3_csbk_t g = grant(T3_OP_TV_GRANT, 159, 1, 501, 1234u);
	rx(&ms, 1, &g);
	CHECK(ms.state == T3_FOLLOW && ms.call.dst == 501, "a DGNA group's call followed");
	frames(&ms, 2);
	t3_csbk_t c = { .lb = true, .op = T3_OP_P_CLEAR };
	c.u.clear.group = true;
	c.u.clear.dst = 501;
	rx(&ms, 1, &c);
	frames(&ms, 3);
	CHECK(ms.state == T3_CAMPED, "back");

	// Alias mode: the 16th talkgroup with "Ops"
	u.format = T3_UDTF_DGNA_ALIAS;
	uint8_t al[10] = { 0, 0, 0x03, 0x09, 0, 'O', 0, 'p', 0, 's' };	// 777
	udt_down(&ms, 1, &u, al, sizeof(al), false);
	CHECK(ms.dgna_alias_tg == 777 && ms.dgna_named == 777 && ms.dgna_alias_len == 3 && ms.dgna_alias[2] == 's' &&
	      ms.dgna_onekey == 500 && t3_ms_wants_group(&ms, 777, false), "alias mode: the 16th, len %u",
	      ms.dgna_alias_len);
	// Naming one of the list keeps the 16th
	al[2] = 0x01;
	al[3] = 0xF5;
	udt_down(&ms, 1, &u, al, sizeof(al), false);
	CHECK(ms.dgna_alias_tg == 777 && ms.dgna_named == 501, "alias names 501");

	// To a talkgroup: not a DGNA for the radio
	u.format = T3_UDTF_ADDRESS;
	u.group = true;
	u.dst = TG_A;
	uint8_t none[10] = { 0 };
	udt_down(&ms, 1, &u, none, sizeof(none), false);
	CHECK(ms.ndgna == 2, "group-addressed ignored");
	// Delete all of the list
	u.group = false;
	u.dst = OWN;
	udt_down(&ms, 1, &u, none, sizeof(none), false);
	CHECK(ms.ndgna == 0 && ms.dgna_onekey == 0 && ms.dgna_alias_tg == 777 && !t3_ms_wants_group(&ms, 500, false),
	      "list deleted");
}

static void test_status(void)
{
	t3_ms_t ms;
	setup(T3_OPT_PRIVATE, true);
	camp_registered(&ms);

	// To an MS (table 6.57): delivered to it, then its acceptance mirrored
	CHECK(t3_ms_status(&ms, T, CALLER, false, 37), "status to an MS starts");
	CHECK(sent_rand(&ms), "status request sent");
	const t3_rand_t *q = &sent[0].u.rand;
	CHECK(q->kind == T3_SK_STATUS && q->options == (37 >> 2) && (q->extra & 3u) == (37 & 3) && q->dst == CALLER &&
	      q->src == OWN, "C_RAND status 37: options 0x%02x extra %u", q->options, q->extra);
	t3_csbk_t h = status_ahoy(37, false, CALLER, OWN);
	rx(&ms, 1, &h);
	CHECK(ms.ax.state == T3_AX_ACKED && ms.ax.result == T3_RES_PENDING, "our status going out: progress");
	CHECK(ms.status_rx.count == 0, "not a status for us");
	t3_csbk_t a = ack(T3_OP_C_ACKD, T3_RC_MS_ACCEPTED, OWN, CALLER);
	rx(&ms, 1, &a);
	CHECK(ms.ax.result == T3_RES_DONE && ms.own_status == 37, "delivered, result %s", t3_res_name(ms.ax.result));

	// To a talkgroup: G/I set; the TSCC's Message_Accepted ends it
	nsent = 0;
	CHECK(t3_ms_status(&ms, T, TG_A, true, 5), "status to a talkgroup starts");
	CHECK(sent_rand(&ms) && sent[0].u.rand.options == (0x40 | (5 >> 2)) && (sent[0].u.rand.extra & 3u) == 1u,
	      "talkgroup status: options 0x%02x", sent[0].u.rand.options);
	// The TSCC sends it to the talkgroup the radio listens to: progress, not a status heard
	h = status_ahoy(5, true, TG_A, OWN);
	rx(&ms, 1, &h);
	CHECK(ms.ax.state == T3_AX_ACKED && ms.status_rx.count == 0, "own talkgroup status: not heard, count %u",
	      (unsigned)ms.status_rx.count);
	a = ack(T3_OP_C_ACKD, T3_RC_MESSAGE_ACCEPTED, OWN, TG_A);
	rx(&ms, 1, &a);
	CHECK(ms.ax.result == T3_RES_DONE && ms.own_status == 5, "talkgroup status done");

	// A poll: status 127 to an MS only; the answer's Response_Info is its status
	CHECK(!t3_ms_status(&ms, T, TG_A, true, T3_STATUS_POLL) && ms.ax.result == T3_RES_NOT_ALLOWED,
	      "no poll to a talkgroup");
	CHECK(!t3_ms_status(&ms, T, CALLER, false, 128), "no status past 127");
	nsent = 0;
	CHECK(t3_ms_status(&ms, T, CALLER, false, T3_STATUS_POLL), "poll starts");
	CHECK(sent_rand(&ms) && sent[0].u.rand.options == 0x1F && (sent[0].u.rand.extra & 3u) == 3u, "poll request");
	a = ack(T3_OP_C_ACKD, T3_RC_STATUS_ACCEPTED, OWN, CALLER);
	a.u.ack.info = 12;
	rx(&ms, 1, &a);
	CHECK(ms.ax.result == T3_RES_DONE && ms.ax.info == 12 && ms.own_status == 5, "poll answered: info %u",
	      ms.ax.info);

	// Received: to the radio, answered MS_Accepted
	nsent = 0;
	h = status_ahoy(100, false, OWN, CALLER);
	rx(&ms, 1, &h);
	frames(&ms, 1);
	CHECK(ms.status_rx.count == 1 && ms.status_rx.value == 100 && ms.status_rx.src == CALLER && !ms.status_rx.group,
	      "status 100 heard: %u", ms.status_rx.value);
	CHECK(nsent == 1 && sent[0].op == T3_OP_C_ACKU && sent[0].u.ack.reason == T3_RC_MS_ACCEPTED &&
	      sent[0].u.ack.dst == CALLER, "status acknowledged");
	// A poll: Accepted for the Status Polling Service with the radio's last status
	h = status_ahoy(T3_STATUS_POLL, false, OWN, CALLER);
	rx(&ms, 1, &h);
	frames(&ms, 1);
	CHECK(nsent == 2 && sent[1].u.ack.reason == T3_RC_MS_POLL_ACCEPTED && sent[1].u.ack.info == 5,
	      "poll answered: reason 0x%02x info %u", sent[1].u.ack.reason, sent[1].u.ack.info);
	// To a talkgroup it listens to: kept, not answered; to one it doesn't: ignored
	h = status_ahoy(9, true, TG_B, CALLER);
	rx(&ms, 1, &h);
	frames(&ms, 2);
	CHECK(ms.status_rx.count == 3 && ms.status_rx.group && ms.status_rx.dst == TG_B && nsent == 2,
	      "talkgroup status kept, no answer");
	// The TSCC's repeat isn't another status; the same one later is
	rx(&ms, 1, &h);
	frames(&ms, 2);
	CHECK(ms.status_rx.count == 3, "a repeat isn't counted: %u", (unsigned)ms.status_rx.count);
	T += T3_REPEAT_MS;
	rx(&ms, 1, &h);
	frames(&ms, 2);
	CHECK(ms.status_rx.count == 4, "the same status later is counted: %u", (unsigned)ms.status_rx.count);
	h = status_ahoy(9, true, TG_OTHER, CALLER);
	rx(&ms, 1, &h);
	h = status_ahoy(9, false, OWN + 1u, CALLER);
	rx(&ms, 1, &h);
	frames(&ms, 2);
	CHECK(ms.status_rx.count == 4 && nsent == 2, "others' statuses ignored");

	// Listen only: kept, never answered, none sent
	setup(T3_OPT_PRIVATE | T3_OPT_LISTEN_ONLY, true);
	camp(&ms);
	h = status_ahoy(3, false, OWN, CALLER);
	rx(&ms, 1, &h);
	frames(&ms, 3);
	CHECK(ms.status_rx.count == 1 && nsent == 0, "listen only: kept, no answer");
	CHECK(!t3_ms_status(&ms, T, CALLER, false, 3), "listen only: no status sent");
}

// The TSCC's download of short data: a UDT header and its blocks in consecutive frames
static void udt_down(t3_ms_t *ms, uint8_t slot, const t3_udt_t *h, const uint8_t *data, uint8_t len, bool corrupt)
{
	uint8_t blk[T3_UDT_BLOCKS_MAX][T3_UDT_BLOCK];
	uint8_t pad = 0;
	t3_udt_t u = *h;
	u.blocks = t3_udt_pack(data, len, 0xFF, blk, &pad);
	u.pad = pad;
	if (corrupt) {
		blk[0][1] ^= 4u;
	}
	uint8_t d[T3_CSBK_BYTES];
	t3_udt_build(&u, d);
	t3_ms_rx(ms, T, slot, T3_DT_DATA_HDR, CC, d);
	for (uint8_t i = 0; i < u.blocks; i++) {
		T += 60;
		t3_ms_rx(ms, T, slot, T3_DT_R12, CC, blk[i]);
	}
	poll_all(ms);
}

// Packet data (6.6.3): the radio asks for a data channel and follows PD_GRANT / TD_GRANT; to it, the TSCC checks it is
// there first. The data service sends on the call (the radio side), PTT doesn't
static void test_pdata(void)
{
	t3_ms_t ms;
	setup(T3_OPT_PRIVATE, true);
	camp_registered(&ms);

	// To an MS: C_RAND 0010b (table 6.50), no options
	CHECK(t3_ms_pdata(&ms, T, CALLER, false), "packet data request");
	CHECK(sent_rand(&ms), "request sent");
	const t3_rand_t *q = &sent[0].u.rand;
	CHECK(q->kind == T3_SK_IND_DATA && q->options == 0 && q->extra == 0 && q->dst == CALLER && q->src == OWN,
	      "C_RAND packet data: kind %u options %u", q->kind, q->options);
	CHECK(!t3_ms_request(&ms, T, TG_A, true, false), "one request at a time");
	t3_csbk_t a = ack(T3_OP_C_ACKD, T3_RC_WAIT, OWN, CALLER);
	rx(&ms, 1, &a);
	CHECK(ms.ax.state == T3_AX_ACKED, "wait");
	t3_csbk_t g = grant(T3_OP_PD_GRANT, 999, 2, CALLER, OWN);
	rx(&ms, 1, &g);
	CHECK(ms.state == T3_FOLLOW && ms.call.data && ms.call.ours && !ms.call.group && ms.call.slot == 2 &&
	      ms.ax.result == T3_RES_GRANTED && tuned.payload && tuned.slot == 2, "granted: following the data call");
	CHECK(!t3_ms_can_talk(&ms), "no PTT on a data call");
	CHECK(!t3_ms_data_ready(&ms, T), "not settled yet");
	frames(&ms, 3);
	CHECK(t3_ms_data_ready(&ms, T), "settled: the data goes");
	// The repeat of our packet keeps the call; P_CLEAR ends it
	t3_ms_activity(&ms, T, 2);
	t3_csbk_t c = { .lb = true, .op = T3_OP_P_CLEAR };
	c.u.clear.dst = CALLER;
	rx(&ms, 2, &c);
	CHECK(ms.state == T3_CAMPED && !t3_ms_data_ready(&ms, T), "P_CLEAR: back on the TSCC");

	// To a talkgroup: 0011b, TD_GRANT
	frames(&ms, 50);
	nsent = 0;
	CHECK(t3_ms_pdata(&ms, T, TG_OTHER, true), "talkgroup packet data");
	CHECK(sent_rand(&ms) && sent[0].u.rand.kind == T3_SK_TG_DATA && sent[0].u.rand.dst == TG_OTHER,
	      "C_RAND talkgroup data");
	g = grant(T3_OP_TD_GRANT, 999, 1, TG_OTHER, OWN);
	rx(&ms, 1, &g);
	CHECK(ms.state == T3_FOLLOW && ms.call.data && ms.call.group && ms.call.dst == TG_OTHER &&
	      ms.ax.result == T3_RES_GRANTED, "talkgroup data granted (ours, though not listened to)");
	nsent = 0;
	t3_ms_end_call(&ms, T);
	frames(&ms, 6);
	CHECK(nsent == T3_N_MAINT && ms.state == T3_CAMPED, "ended with P_MAINT");

	// The radio's single item over (6.6.3.3.2.5), though the slot's idles go on (frame() counts them as activity) and
	// no P_CLEAR comes: retries spent, ended at once; delivered, ended T3_DATA_LINGER_MS after the last data heard
	for (int ok = 0; ok < 2; ok++) {
		frames(&ms, 50);
		CHECK(t3_ms_pdata(&ms, T, CALLER, false), "item %d", ok);
		g = grant(T3_OP_PD_GRANT, 999, 2, CALLER, OWN);
		rx(&ms, 1, &g);
		frames(&ms, 3);
		CHECK(ms.state == T3_FOLLOW && t3_ms_data_ready(&ms, T), "item %d: on the channel", ok);
		frames(&ms, 100);
		CHECK(ms.state == T3_FOLLOW, "item %d: idles hold the channel", ok);
		nsent = 0;
		t3_ms_data_done(&ms, T, ok != 0);
		if (ok) {
			frames(&ms, T3_DATA_LINGER_MS / 60 - 5);
			t3_ms_data_heard(&ms, T);	// a reply
			frames(&ms, T3_DATA_LINGER_MS / 60 - 5);
			CHECK(ms.state == T3_FOLLOW && nsent == 0, "delivered: a reply holds the channel");
		}
		frames(&ms, 6 + (ok ? 10 : 0));
		CHECK(nsent == T3_N_MAINT && sent[0].op == T3_OP_P_MAINT && ms.state == T3_CAMPED,
		      "item %d over: ended with P_MAINT, sent %d", ok, nsent);
	}
	// Not the radio's call, or not a data call: nothing
	frames(&ms, 50);
	t3_ms_data_done(&ms, T, false);
	CHECK(ms.state == T3_CAMPED && nsent == T3_N_MAINT, "camped: nothing");

	// Refused
	frames(&ms, 50);
	CHECK(t3_ms_pdata(&ms, T, CALLER, false), "again");
	frames(&ms, 2);
	a = ack(T3_OP_C_ACKD, T3_RC_MS_AWAY, OWN, CALLER);
	rx(&ms, 1, &a);
	CHECK(ms.ax.result == T3_RES_REFUSED && ms.state == T3_CAMPED, "NACK: refused");

	// To the radio: the availability check answered Message_Accepted (6.6.3.2.6), then PD_GRANT to it
	nsent = 0;
	t3_csbk_t h = ahoy(T3_SK_IND_DATA, false, OWN, CALLER);
	rx(&ms, 1, &h);
	frames(&ms, 1);
	CHECK(nsent == 1 && sent[0].op == T3_OP_C_ACKU && sent[0].u.ack.reason == T3_RC_MESSAGE_ACCEPTED &&
	      sent[0].u.ack.dst == CALLER, "availability check: accepted");
	// The data terminal busy: refused (6.6.3.2.6)
	nsent = 0;
	ms.data_nack = T3_RC_EQUIP_BUSY;
	rx(&ms, 1, &h);
	frames(&ms, 1);
	CHECK(nsent == 1 && sent[0].op == T3_OP_C_ACKU && sent[0].u.ack.reason == T3_RC_EQUIP_BUSY,
	      "data terminal busy: EquipBusy_Refused");
	ms.data_nack = 0;
	g = grant(T3_OP_PD_GRANT, 999, 1, OWN, CALLER);
	rx(&ms, 1, &g);
	CHECK(ms.state == T3_FOLLOW && ms.call.data && !ms.call.ours && ms.call.src == CALLER, "called: following");
	frames(&ms, 3);
	// The payload channel's packet data radio check (6.6.3.3.2.1): P_ACKU Message_Accepted
	nsent = 0;
	h = ahoy(T3_SK_IND_DATA, false, OWN, T3_ID_TSI);
	rx(&ms, 1, &h);
	frames(&ms, 1);
	CHECK(nsent == 1 && sent[0].op == T3_OP_P_ACKU && sent[0].u.ack.reason == T3_RC_MESSAGE_ACCEPTED &&
	      sent[0].u.ack.dst == T3_ID_TSI && ms.state == T3_FOLLOW, "payload data radio check: P_ACKU %u reason 0x%02x",
	      sent[0].op, sent[0].u.ack.reason);
	c.u.clear.dst = OWN;
	rx(&ms, 1, &c);
	CHECK(ms.state == T3_CAMPED, "cleared");

	// Talkgroup data to a group it listens to: followed; to another MS: not
	frames(&ms, 50);
	g = grant(T3_OP_TD_GRANT, 999, 1, TG_B, CALLER);
	rx(&ms, 1, &g);
	CHECK(ms.state == T3_FOLLOW && ms.call.data && ms.call.group && !ms.call.ours, "talkgroup data heard");
	frames(&ms, 3);
	c.u.clear.group = true;
	c.u.clear.dst = TG_B;
	rx(&ms, 1, &c);
	frames(&ms, 50);
	g = grant(T3_OP_PD_GRANT, 999, 1, CALLER + 1u, CALLER);
	rx(&ms, 1, &g);
	CHECK(ms.state == T3_CAMPED, "another MS's data: not followed");

	// Multi-item grants (55, 56): the same applicability (6.6.3.2.7)
	g = grant(T3_OP_PD_GRANT_MI, 999, 2, OWN, CALLER);
	rx(&ms, 1, &g);
	CHECK(ms.state == T3_FOLLOW && ms.call.data && !ms.call.group, "multi-item data to the radio: followed");
	frames(&ms, 3);
	c.u.clear.group = false;
	c.u.clear.dst = OWN;
	rx(&ms, 2, &c);
	CHECK(ms.state == T3_CAMPED, "multi-item data cleared");
	frames(&ms, 50);
	g = grant(T3_OP_TD_GRANT_MI, 999, 2, TG_B, CALLER);
	rx(&ms, 1, &g);
	CHECK(ms.state == T3_FOLLOW && ms.call.data && ms.call.group, "multi-item talkgroup data: followed");
	frames(&ms, 3);
	c.u.clear.group = true;
	c.u.clear.dst = TG_B;
	rx(&ms, 2, &c);
	CHECK(ms.state == T3_CAMPED, "multi-item talkgroup data cleared");

	// Full duplex MS to MS: the radio is simplex. The availability check is refused MS_Duplex_Not_Supported
	// (6.6.9.2.5, 6.6.10.2.5), and a duplex grant naming it isn't followed
	frames(&ms, 50);
	static const uint8_t dx_kind[2] = { T3_SK_DX_VOICE, T3_SK_DX_DATA };
	for (unsigned i = 0; i < 2; i++) {
		nsent = 0;
		t3_csbk_t h = ahoy(dx_kind[i], false, OWN, CALLER);
		rx(&ms, 1, &h);
		frames(&ms, 1);
		CHECK(nsent == 1 && sent[0].u.ack.reason == T3_RC_MS_DUPLEX_NOT_SUPPORTED && sent[0].u.ack.dst == CALLER,
		      "duplex kind %u: reason 0x%02x", dx_kind[i], nsent ? sent[0].u.ack.reason : 0xFFu);
		frames(&ms, 50);
	}
	g = grant(T3_OP_PV_GRANT_DX, 999, 1, OWN, CALLER);
	rx(&ms, 1, &g);
	g = grant(T3_OP_PD_GRANT_DX, 999, 1, OWN, CALLER);
	rx(&ms, 1, &g);
	CHECK(ms.state == T3_CAMPED, "duplex grants: not followed");
}

// Packet data to an IP destination (6.6.3.2.3-4): a multi-part request to IPI, the address uploaded when the TSCC
// IP Connection Advice (6.4.11): with an address set, the registration carries IP_Inform. A C_AHOY from IPI means
// registered and asks for the address (one UDT block in IP format to IPI); the final C_ACKD Reg_Accepted from IPI
// took it, with the power save offset; C_NACKD IP_Connection_failed from IPI leaves the radio registered without it
static void test_ip_advice(void)
{
	t3_ms_t ms;
	setup(T3_OPT_PRIVATE, true);
	cfg.ip_addr = 0x0C23CACEu;	// 12.35.202.206
	cfg.ps_rq = 2;
	nsent = 0;
	nup = 0;
	camp(&ms);
	CHECK(sent_rand(&ms), "registration sent");
	const t3_rand_t *q = &sent[nsent - 1].u.rand;
	CHECK(q->kind == T3_SK_REGISTRATION && (q->options & T3_SO_REG_IP_INFORM) && (q->options & T3_SO_REG_REGISTER) &&
	      ((q->options >> 1) & 7u) == 2u, "IP_Inform with Reg_Dereg 1 and PowerSave_RQ: options %02x", q->options);
	t3_csbk_t h = ahoy(T3_SK_IND_UDT, false, OWN, T3_ID_IPI);
	rx(&ms, 1, &h);
	frames(&ms, 1);
	CHECK(ms.reg == T3_REG_OK && !ms.ip_advised && ms.ax.state == T3_AX_ACKED, "the AHOY from IPI: registered");
	t3_udt_t u;
	CHECK(nup == 1 && nup_blocks == 1 && t3_udt_parse(up_hdr, &u) && u.format == T3_UDTF_IP && u.sf &&
	      u.dst == T3_ID_IPI && u.src == OWN && u.blocks == 1, "the address uploaded: %d", nup);
	static const uint8_t want[10] = { 12, 35, 202, 206, 0, 0, 0, 0, 0, 0 };
	CHECK(memcmp(up_blocks[0], want, sizeof want) == 0, "IPv4, 48 bits reserved");
	t3_csbk_t a = ack(T3_OP_C_ACKD, T3_RC_REG_ACCEPTED, OWN, T3_ID_IPI);
	a.u.ack.info = 1;
	rx(&ms, 1, &a);
	CHECK(ms.ax.result == T3_RES_DONE && ms.reg == T3_REG_OK && ms.ip_advised && ms.ps_offset == 1,
	      "Reg_Accepted from IPI: taken, offset %u", ms.ps_offset);

	// Refused at once (no IP bearer service): registered, without the address or power save
	setup(T3_OPT_PRIVATE, true);
	cfg.ip_addr = 0x0C23CACEu;
	cfg.ps_rq = 2;
	nsent = 0;
	nup = 0;
	camp(&ms);
	CHECK(sent_rand(&ms), "registration sent");
	a = ack(T3_OP_C_ACKD, T3_RC_IP_CONN_FAILED, OWN, T3_ID_IPI);
	rx(&ms, 1, &a);
	int hunts = (int)ms.st.hunts;
	frames(&ms, 100);
	CHECK(ms.ax.result == T3_RES_DONE && ms.reg == T3_REG_OK && !ms.ip_advised && ms.ps_offset == 0 && nup == 0 &&
	      ms.state == T3_CAMPED && (int)ms.st.hunts == hunts && nsent == 1, "IP_Connection_failed: registered, %d sent",
	      nsent);
	// Refused after the upload: the same
	setup(T3_OPT_PRIVATE, true);
	cfg.ip_addr = 0x0C23CACEu;
	nsent = 0;
	nup = 0;
	camp(&ms);
	CHECK(sent_rand(&ms), "registration sent");
	h = ahoy(T3_SK_REGISTRATION, false, OWN, T3_ID_IPI);	// Service_Kind 1110b taken too
	rx(&ms, 1, &h);
	frames(&ms, 1);
	CHECK(nup == 1, "uploaded on a 1110b AHOY");
	rx(&ms, 1, &a);
	CHECK(ms.ax.result == T3_RES_DONE && ms.reg == T3_REG_OK && !ms.ip_advised, "refused after the upload");

	// No address: no IP_Inform; an AHOY from IPI then asks for nothing
	setup(T3_OPT_PRIVATE, true);
	nsent = 0;
	nup = 0;
	camp(&ms);
	CHECK(sent_rand(&ms) && !(sent[nsent - 1].u.rand.options & T3_SO_REG_IP_INFORM), "no address: no IP_Inform");
	h = ahoy(T3_SK_IND_UDT, false, OWN, T3_ID_IPI);
	rx(&ms, 1, &h);
	frames(&ms, 1);
	CHECK(nup == 0 && ms.reg != T3_REG_OK, "nothing uploaded, not registered by it");
	cfg.ip_addr = 0;
	cfg.ps_rq = 0;
}

// asks, then the grant; a site that grants at once works too
static void test_pdata_ip(void)
{
	t3_ms_t ms;
	setup(T3_OPT_PRIVATE, true);
	camp_registered(&ms);
	nsent = 0;
	nup = 0;
	CHECK(!t3_ms_pdata_ip(&ms, T, 0) && ms.ax.result == T3_RES_NOT_ALLOWED, "no address: refused");
	CHECK(t3_ms_pdata_ip(&ms, T, 0xC0A80A05u), "packet data to 192.168.10.5");
	CHECK(sent_rand(&ms), "request sent");
	const t3_rand_t *q = &sent[0].u.rand;
	CHECK(q->kind == T3_SK_IND_DATA && q->dst == T3_ID_IPI && !q->proxy && q->options == 0 && q->src == OWN,
	      "C_RAND to IPI, Proxy 0 (IPv4): kind %u dst %06x", q->kind, (unsigned)q->dst);
	// An AHOY from IPI for another service or MS isn't ours
	t3_csbk_t other = ahoy(T3_SK_IND_DATA, false, CALLER, T3_ID_IPI);
	rx(&ms, 1, &other);
	frames(&ms, 1);
	CHECK(nup == 0, "not to us: nothing uploaded");
	t3_csbk_t h = ahoy(T3_SK_IND_DATA, false, OWN, T3_ID_IPI);
	rx(&ms, 1, &h);
	frames(&ms, 1);
	t3_udt_t u;
	CHECK(nup == 1 && nup_blocks == 1 && t3_udt_parse(up_hdr, &u), "address uploaded: %d", nup);
	CHECK(u.op == T3_OP_C_UDTHU && !u.group && u.sf && !u.a && !u.opt && u.format == T3_UDTF_IP &&
	      u.dst == T3_ID_IPI && u.src == OWN && u.blocks == 1, "C_UDTHU: dst %06x format %u sf %d", (unsigned)u.dst,
	      u.format, u.sf);
	static const uint8_t want[10] = { 192, 168, 10, 5, 0, 0, 0, 0, 0, 0 };
	CHECK(memcmp(up_blocks[0], want, sizeof want) == 0 &&
	      t3_udt_crc_ok((const uint8_t (*)[T3_UDT_BLOCK])up_blocks, 1), "B.3.7: the IPv4 address, 48 bits reserved");
	// Asked again (a CRC failure at the TSCC): sent again
	rx(&ms, 1, &h);
	frames(&ms, 1);
	CHECK(nup == 2, "uploaded again");
	t3_csbk_t g = grant(T3_OP_PD_GRANT, 999, 2, T3_ID_IPI, OWN);
	rx(&ms, 1, &g);
	CHECK(ms.state == T3_FOLLOW && ms.call.data && ms.call.ours && ms.ax.result == T3_RES_GRANTED,
	      "granted: following the data call");
	t3_ms_end_call(&ms, T);
	frames(&ms, 60);

	// Granted at once, no AHOY: fine too
	nsent = 0;
	CHECK(ms.state == T3_CAMPED && t3_ms_pdata_ip(&ms, T, 0x0A000001u), "again");
	CHECK(sent_rand(&ms), "request sent");
	rx(&ms, 1, &g);
	CHECK(ms.state == T3_FOLLOW && ms.ax.result == T3_RES_GRANTED && nup == 2, "granted without an upload");
	// A later single-part request uploads nothing to an AHOY from IPI
	t3_ms_end_call(&ms, T);
	frames(&ms, 60);
	nsent = 0;
	CHECK(t3_ms_pdata(&ms, T, CALLER, false) && sent_rand(&ms), "to an MS");
	rx(&ms, 1, &h);
	frames(&ms, 1);
	CHECK(nup == 2, "no address for a single-part call");
	t3_csbk_t a = ack(T3_OP_C_ACKD, T3_RC_MS_ACCEPTED, OWN, CALLER);
	rx(&ms, 1, &a);
	g = grant(T3_OP_PD_GRANT, 999, 2, CALLER, OWN);
	rx(&ms, 1, &g);
	t3_ms_end_call(&ms, T);
	frames(&ms, 60);

	// IPI's data to the radio (a reply from the network): with nothing of the radio's asked, its C_AHOY is the
	// availability check (6.6.3.2.6), answered Message_Accepted to IPI; then its PD_GRANT is followed
	nsent = 0;
	CHECK(ms.state == T3_CAMPED && ms.ax.state == T3_AX_IDLE, "camped, idle");
	rx(&ms, 1, &h);
	frames(&ms, 1);
	CHECK(nup == 2 && nsent == 1 && sent[0].op == T3_OP_C_ACKU && sent[0].u.ack.reason == T3_RC_MESSAGE_ACCEPTED &&
	      sent[0].u.ack.dst == T3_ID_IPI && sent[0].u.ack.src == OWN, "IPI's check: accepted, sent %d", nsent);
	g = grant(T3_OP_PD_GRANT, 999, 1, OWN, T3_ID_IPI);
	rx(&ms, 1, &g);
	CHECK(ms.state == T3_FOLLOW && ms.call.data && !ms.call.ours && ms.call.src == T3_ID_IPI,
	      "IPI's data call followed");
}

static void test_sdata(void)
{
	t3_ms_t ms;
	setup(T3_OPT_PRIVATE, true);
	camp_registered(&ms);
	static const char msg[] = "hello from the radio";	// 20 characters: 2 blocks

	// To an MS (6.6.4.2, table 6.55)
	CHECK(t3_ms_sdata(&ms, T, CALLER, false, T3_UDTF_ISO8, (const uint8_t *)msg, 20), "short data starts");
	CHECK(sent_rand(&ms), "short data request sent");
	const t3_rand_t *q = &sent[0].u.rand;
	CHECK(q->kind == T3_SK_IND_UDT && q->options == 0 && (q->extra & 3u) == 1u && q->dst == CALLER && q->src == OWN,
	      "C_RAND short data: kind %u extra %u", q->kind, q->extra);
	t3_csbk_t a = ack(T3_OP_C_ACKD, T3_RC_WAIT, OWN, CALLER);
	rx(&ms, 1, &a);
	CHECK(ms.ax.state == T3_AX_ACKED, "wait");
	// The TSCC asks for the data: the header and both blocks go in the next inbound slots
	t3_csbk_t h = ahoy(T3_SK_IND_UDT, false, OWN, T3_ID_SDMI);
	h.u.ahoy.blocks = 1;
	rx(&ms, 1, &h);
	CHECK(nup == 0, "the upload waits for its slot");
	frames(&ms, 1);
	t3_udt_t u;
	CHECK(nup == 1 && up_slot == 1 && nup_blocks == 2 && t3_udt_parse(up_hdr, &u), "upload sent: %d, %u blocks",
	      nup, nup_blocks);
	CHECK(u.op == T3_OP_C_UDTHU && !u.group && u.format == T3_UDTF_ISO8 && u.dst == CALLER && u.src == OWN &&
	      u.blocks == 2 && u.pad == 4 && !u.sf, "C_UDTHU: op %u format %u pad %u", u.op, u.format, u.pad);
	CHECK(t3_udt_crc_ok((const uint8_t (*)[T3_UDT_BLOCK])up_blocks, 2) && memcmp(up_blocks, msg, 20) == 0 &&
	      up_blocks[1][8] == 0xFF, "blocks: the text, 0xFF, the CRC");
	frames(&ms, 3);
	CHECK(nup == 1 && nsent == 1, "sent once");
	// A CRC failure at the TSCC: it asks again
	rx(&ms, 1, &h);
	frames(&ms, 1);
	CHECK(nup == 2, "upload again when asked");
	a = ack(T3_OP_C_ACKD, T3_RC_MS_ACCEPTED, OWN, CALLER);
	rx(&ms, 1, &a);
	CHECK(ms.ax.result == T3_RES_DONE, "delivered, result %s", t3_res_name(ms.ax.result));
	// Asked with nothing to send: no upload, no answer
	rx(&ms, 1, &h);
	frames(&ms, 2);
	CHECK(nup == 2 && nsent == 1, "no upload when nothing waits");

	// To a talkgroup: kind 0101b; Message_Accepted ends it
	nsent = 0;
	CHECK(t3_ms_sdata(&ms, T, TG_A, true, T3_UDTF_BINARY, (const uint8_t *)"\x01\x02\x03", 3), "talkgroup data");
	CHECK(sent_rand(&ms) && sent[0].u.rand.kind == T3_SK_TG_UDT && (sent[0].u.rand.extra & 3u) == 0u,
	      "C_RAND talkgroup short data");
	rx(&ms, 1, &h);
	frames(&ms, 1);
	CHECK(nup == 3 && t3_udt_parse(up_hdr, &u) && u.group && u.format == T3_UDTF_BINARY && u.pad == 0 &&
	      u.blocks == 1 && up_blocks[0][3] == 0, "binary upload: pad 0, zeros");
	a = ack(T3_OP_C_ACKD, T3_RC_MESSAGE_ACCEPTED, OWN, TG_A);
	rx(&ms, 1, &a);
	CHECK(ms.ax.result == T3_RES_DONE, "talkgroup data done");
	CHECK(!t3_ms_sdata(&ms, T, CALLER, false, T3_UDTF_ISO8, (const uint8_t *)msg, 0) &&
	      ms.ax.result == T3_RES_NOT_ALLOWED, "no empty message");

	// ISO 7 bit: one character an octet in, 7 bits each out, 1s after; 4 symbols pad 13 nibbles (table B.4)
	nsent = 0;
	CHECK(t3_ms_sdata(&ms, T, CALLER, false, T3_UDTF_ISO7, (const uint8_t *)"ABCD", 4), "ISO 7 data");
	CHECK(sent_rand(&ms), "C_RAND short data");
	rx(&ms, 1, &h);
	frames(&ms, 1);
	CHECK(nup == 4 && t3_udt_parse(up_hdr, &u) && u.format == T3_UDTF_ISO7 && u.pad == 13 && u.blocks == 1 &&
	      memcmp(up_blocks[0], "\x83\x0A\x1C\x4F\xFF\xFF\xFF\xFF\xFF\xFF", 10) == 0, "ISO 7 upload: pad %u, %02x %02x %02x %02x %02x",
	      u.pad, up_blocks[0][0], up_blocks[0][1], up_blocks[0][2], up_blocks[0][3], up_blocks[0][4]);
	a = ack(T3_OP_C_ACKD, T3_RC_MS_ACCEPTED, OWN, CALLER);
	rx(&ms, 1, &a);
	CHECK(ms.ax.result == T3_RES_DONE, "ISO 7 delivered");
	CHECK(!t3_ms_sdata(&ms, T, CALLER, false, T3_UDTF_ISO7, (const uint8_t *)msg, T3_UDT_ISO7_MAX + 1) &&
	      ms.ax.result == T3_RES_NOT_ALLOWED, "ISO 7: at most 52 characters");
	// BCD "123": 0x12 0x3F, the DigitNULL counted with the fill (17 nibbles in one block, table B.3)
	nsent = 0;
	CHECK(t3_ms_sdata(&ms, T, CALLER, false, T3_UDTF_BCD, (const uint8_t *)"\x12\x3F", 2), "BCD data");
	CHECK(sent_rand(&ms), "C_RAND short data");
	rx(&ms, 1, &h);
	frames(&ms, 1);
	CHECK(nup == 5 && t3_udt_parse(up_hdr, &u) && u.format == T3_UDTF_BCD && u.pad == 17 && up_blocks[0][2] == 0xFF,
	      "BCD upload: pad %u", u.pad);
	a = ack(T3_OP_C_ACKD, T3_RC_MS_ACCEPTED, OWN, CALLER);
	rx(&ms, 1, &a);
	CHECK(ms.ax.result == T3_RES_DONE, "BCD delivered");

	// Received: to the radio with A, answered MS_Accepted; the text without its padding
	nsent = 0;
	t3_udt_t d = { .op = T3_OP_C_UDTHD, .a = true, .format = T3_UDTF_ISO8, .dst = OWN, .src = CALLER };
	udt_down(&ms, 1, &d, (const uint8_t *)"hello", 5, false);
	frames(&ms, 1);
	CHECK(ms.sd_rx.count == 1 && ms.sd_rx.len == 5 && memcmp(ms.sd_rx.data, "hello", 5) == 0 &&
	      ms.sd_rx.src == CALLER && ms.sd_rx.format == T3_UDTF_ISO8 && !ms.sd_rx.group, "short data heard: len %u",
	      ms.sd_rx.len);
	CHECK(nsent == 1 && sent[0].op == T3_OP_C_ACKU && sent[0].u.ack.reason == T3_RC_MS_ACCEPTED &&
	      sent[0].u.ack.dst == CALLER, "short data acknowledged");
	// Sent again (the answer was lost): answered again, not another message
	udt_down(&ms, 1, &d, (const uint8_t *)"hello", 5, false);
	frames(&ms, 1);
	CHECK(ms.sd_rx.count == 1 && nsent == 2 && sent[1].u.ack.reason == T3_RC_MS_ACCEPTED,
	      "repeat: answered, count %u", (unsigned)ms.sd_rx.count);
	// 46 characters, 4 blocks
	udt_down(&ms, 1, &d, (const uint8_t *)"0123456789012345678901234567890123456789012345", 46, false);
	frames(&ms, 1);
	CHECK(ms.sd_rx.count == 2 && ms.sd_rx.len == 46 && ms.sd_rx.data[45] == '5' && nsent == 3, "46 characters");
	// A bad CRC: not kept, no answer (the TSCC sends it again)
	udt_down(&ms, 1, &d, (const uint8_t *)"hello", 5, true);
	frames(&ms, 2);
	CHECK(ms.sd_rx.count == 2 && nsent == 3, "bad CRC: dropped");
	// A block missing: nothing
	d.dst = OWN;
	{
		uint8_t blk[T3_UDT_BLOCKS_MAX][T3_UDT_BLOCK], pad, hd[T3_CSBK_BYTES];
		t3_udt_t u2 = d;
		u2.blocks = t3_udt_pack((const uint8_t *)msg, 20, 0xFF, blk, &pad);
		u2.pad = pad;
		t3_udt_build(&u2, hd);
		t3_ms_rx(&ms, T, 1, T3_DT_DATA_HDR, CC, hd);
		T += 60;
		t3_ms_rx(&ms, T, 1, T3_DT_R12, CC, blk[0]);
		frames(&ms, 4);
		CHECK(ms.sd_rx.count == 2 && nsent == 3, "a block missing: dropped");
	}
	// To a talkgroup it listens to: kept, no answer; another MS's or talkgroup's: ignored
	d.group = true;
	d.a = false;
	d.dst = TG_B;
	udt_down(&ms, 1, &d, (const uint8_t *)"all", 3, false);
	frames(&ms, 2);
	CHECK(ms.sd_rx.count == 3 && ms.sd_rx.group && ms.sd_rx.dst == TG_B && nsent == 3, "talkgroup data kept");
	udt_down(&ms, 1, &d, (const uint8_t *)"all", 3, false);
	frames(&ms, 2);
	CHECK(ms.sd_rx.count == 3, "talkgroup repeat not counted: %u", (unsigned)ms.sd_rx.count);
	T += T3_REPEAT_MS;
	udt_down(&ms, 1, &d, (const uint8_t *)"all", 3, false);
	frames(&ms, 2);
	CHECK(ms.sd_rx.count == 4, "the same later is counted: %u", (unsigned)ms.sd_rx.count);
	d.dst = TG_OTHER;
	udt_down(&ms, 1, &d, (const uint8_t *)"x", 1, false);
	d.group = false;
	d.a = true;
	d.dst = OWN + 1u;
	udt_down(&ms, 1, &d, (const uint8_t *)"x", 1, false);
	frames(&ms, 2);
	CHECK(ms.sd_rx.count == 4 && nsent == 3, "others' data ignored");

	// Listen only: kept, not answered, none sent
	setup(T3_OPT_PRIVATE | T3_OPT_LISTEN_ONLY, true);
	camp(&ms);
	d.dst = OWN;
	udt_down(&ms, 1, &d, (const uint8_t *)"hi", 2, false);
	frames(&ms, 3);
	CHECK(ms.sd_rx.count == 1 && nsent == 0, "listen only: kept, no answer");
	CHECK(!t3_ms_sdata(&ms, T, CALLER, false, T3_UDTF_ISO8, (const uint8_t *)"hi", 2), "listen only: none sent");
}

// Supplementary user data (6.4.13): offered with a request (SUPED_SV, SUPED_VAL), uploaded to SUPLI when it asks, and
// heard from SUPLI, held until the grant or user data that follows names its sender
static void test_supl(void)
{
	t3_ms_t ms;
	setup(T3_OPT_PRIVATE, true);
	camp_registered(&ms);
	static const uint8_t gps[14] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14 };	// 2 blocks
	cfg.supl_data = gps;
	cfg.supl_len = sizeof gps;
	cfg.supl_format = T3_UDTF_BINARY;

	// Short data with supplementary data: both uploads, each when its gateway asks (figure 6.39)
	CHECK(t3_ms_sdata(&ms, T, CALLER, false, T3_UDTF_ISO8, (const uint8_t *)"hi", 2), "short data starts");
	CHECK(sent_rand(&ms), "request sent");
	const t3_rand_t *q = &sent[0].u.rand;
	CHECK((q->options & T3_SO_SUPPL_DATA) && (q->extra >> 2) == 1u && (q->extra & 3u) == 0u,
	      "SUPED_SV 1, SUPED_VAL 2 blocks, SDATA_VAL 1 block: options 0x%02x extra 0x%x", q->options, q->extra);
	t3_csbk_t h = ahoy(T3_SK_IND_UDT, false, OWN, T3_ID_SUPLI);
	h.u.ahoy.blocks = 1;
	rx(&ms, 1, &h);
	frames(&ms, 1);
	t3_udt_t u;
	CHECK(nup == 1 && nup_blocks == 2 && t3_udt_parse(up_hdr, &u) && u.op == T3_OP_C_UDTHU && u.sf &&
	      u.dst == T3_ID_SUPLI && u.src == OWN && u.format == T3_UDTF_BINARY && !u.group &&
	      memcmp(up_blocks, gps, sizeof gps) == 0, "supplementary upload: %d, sf %u dst %06x", nup, u.sf,
	      (unsigned)u.dst);
	CHECK(ms.ax.state == T3_AX_ACKED, "the request goes on");
	h = ahoy(T3_SK_IND_UDT, false, OWN, T3_ID_SDMI);
	rx(&ms, 1, &h);
	frames(&ms, 1);
	CHECK(nup == 2 && t3_udt_parse(up_hdr, &u) && !u.sf && u.dst == CALLER && memcmp(up_blocks, "hi", 2) == 0,
	      "then the user data");
	t3_csbk_t a = ack(T3_OP_C_ACKD, T3_RC_MS_ACCEPTED, OWN, CALLER);
	rx(&ms, 1, &a);
	CHECK(ms.ax.result == T3_RES_DONE, "delivered");

	// A voice call offers it too (table 6.40): SUPED_VAL above ALS_SERV
	nsent = 0;
	CHECK(t3_ms_request(&ms, T, TG_A, true, false), "group call");
	CHECK(sent_rand(&ms) && (sent[0].u.rand.options & T3_SO_SUPPL_DATA) && (sent[0].u.rand.extra >> 2) == 1u,
	      "voice request with supplementary data");
	nsent = 0;
	t3_ms_cancel(&ms, T);
	CHECK(sent_rand(&ms) && sent[nsent - 1].u.rand.kind == T3_SK_CANCEL, "canceled");
	a = ack(T3_OP_C_ACKD, T3_RC_MESSAGE_ACCEPTED, OWN, TG_A);
	rx(&ms, 1, &a);

	// A status offers it too (table 6.57): SUPED_SV above the status's 5 bits, SUPED_VAL above its 2; the upload
	// when SUPLI asks (C_AHOY Service_Kind status), then the status goes on
	nsent = 0;
	CHECK(t3_ms_status(&ms, T, CALLER, false, 37), "status starts");
	q = &sent[0].u.rand;
	CHECK(sent_rand(&ms) && q->kind == T3_SK_STATUS && q->options == (0x20 | (37 >> 2)) &&
	      q->extra == ((1u << 2) | (37 & 3)), "status with supplementary data: options 0x%02x extra 0x%x", q->options,
	      q->extra);
	h = ahoy(T3_SK_STATUS, false, OWN, T3_ID_SUPLI);
	h.u.ahoy.blocks = 1;
	rx(&ms, 1, &h);
	frames(&ms, 1);
	CHECK(nup == 3 && nup_blocks == 2 && t3_udt_parse(up_hdr, &u) && u.sf && u.dst == T3_ID_SUPLI &&
	      memcmp(up_blocks, gps, sizeof gps) == 0 && ms.status_rx.count == 0, "status's supplementary upload: %d",
	      nup);
	h = status_ahoy(37, false, CALLER, OWN);
	rx(&ms, 1, &h);
	a = ack(T3_OP_C_ACKD, T3_RC_MS_ACCEPTED, OWN, CALLER);
	rx(&ms, 1, &a);
	CHECK(ms.ax.result == T3_RES_DONE && ms.own_status == 37, "status delivered");

	// None configured: not offered, and a C_AHOY from SUPLI gets nothing
	cfg.supl_len = 0;
	nsent = 0;
	CHECK(t3_ms_sdata(&ms, T, CALLER, false, T3_UDTF_ISO8, (const uint8_t *)"hi", 2), "short data starts");
	CHECK(sent_rand(&ms) && !(sent[0].u.rand.options & T3_SO_SUPPL_DATA) && sent[0].u.rand.extra == 0u,
	      "not offered");
	h = ahoy(T3_SK_IND_UDT, false, OWN, T3_ID_SUPLI);
	rx(&ms, 1, &h);
	frames(&ms, 2);
	CHECK(nup == 3 && nsent == 1, "nothing to upload");
	a = ack(T3_OP_C_ACKD, T3_RC_MS_ACCEPTED, OWN, CALLER);
	rx(&ms, 1, &a);

	// Heard: to the radio with A, accepted (C_ACKU Message_Accepted to SUPLI); not short data; the sender unknown
	nsent = 0;
	t3_udt_t d = { .op = T3_OP_C_UDTHD, .a = true, .sf = true, .format = T3_UDTF_BINARY, .dst = OWN,
		       .src = T3_ID_SUPLI };
	uint32_t sd0 = ms.sd_rx.count;
	udt_down(&ms, 1, &d, gps, 5, false);
	frames(&ms, 1);
	CHECK(ms.supl_rx.count == 1 && ms.supl_rx.src == 0 && ms.supl_rx.len == 5 && ms.supl_rx.dst == OWN &&
	      memcmp(ms.supl_rx.data, gps, 5) == 0 && ms.sd_rx.count == sd0, "supplementary data held: len %u",
	      ms.supl_rx.len);
	CHECK(nsent == 1 && sent[0].op == T3_OP_C_ACKU && sent[0].u.ack.reason == T3_RC_MESSAGE_ACCEPTED &&
	      sent[0].u.ack.dst == T3_ID_SUPLI && sent[0].u.ack.src == OWN, "accepted: 0x%02x to %06x",
	      sent[0].u.ack.reason, (unsigned)sent[0].u.ack.dst);
	// Sent again: answered, not another
	udt_down(&ms, 1, &d, gps, 5, false);
	frames(&ms, 1);
	CHECK(ms.supl_rx.count == 1 && nsent == 2, "repeat answered once more");
	// The user data that follows names the sender (figure 6.39)
	t3_udt_t m = { .op = T3_OP_C_UDTHD, .a = true, .format = T3_UDTF_ISO8, .dst = OWN, .src = CALLER };
	udt_down(&ms, 1, &m, (const uint8_t *)"hello", 5, false);
	frames(&ms, 1);
	CHECK(ms.sd_rx.count == sd0 + 1 && ms.supl_rx.src == CALLER, "sender named by the user data: %06x",
	      (unsigned)ms.supl_rx.src);

	// To a talkgroup the radio follows: kept, no answer; the grant names the sender (figure 6.38)
	nsent = 0;
	d.group = true;
	d.a = false;
	d.dst = TG_B;
	udt_down(&ms, 1, &d, gps + 1, 3, false);
	frames(&ms, 2);
	CHECK(ms.supl_rx.count == 2 && ms.supl_rx.src == 0 && ms.supl_rx.group && nsent == 0, "talkgroup's: held");
	t3_csbk_t g = grant(T3_OP_TV_GRANT, 895, 1, TG_B, CALLER + 1u);
	rx(&ms, 1, &g);
	CHECK(ms.state == T3_FOLLOW && ms.supl_rx.src == CALLER + 1u, "sender named by the grant: %06x",
	      (unsigned)ms.supl_rx.src);
	t3_ms_end_call(&ms, T);
	poll_all(&ms);
	// Another talkgroup's or another MS's: ignored
	d.dst = TG_OTHER;
	udt_down(&ms, 1, &d, gps, 3, false);
	d.group = false;
	d.a = true;
	d.dst = OWN + 1u;
	udt_down(&ms, 1, &d, gps, 3, false);
	frames(&ms, 2);
	CHECK(ms.supl_rx.count == 2 && nsent == 0, "not ours: ignored");
	// Too late to be named: a grant after T3_SUPL_HOLD_MS leaves it unnamed
	d.dst = OWN;
	udt_down(&ms, 1, &d, gps + 2, 4, false);
	frames(&ms, 1);
	T += T3_SUPL_HOLD_MS + 60;
	udt_down(&ms, 1, &m, (const uint8_t *)"later", 5, false);
	CHECK(ms.supl_rx.count == 3 && ms.supl_rx.src == 0, "too late: unnamed");
	// A status to the radio after it names the sender (6.6.6.1.1.2 c, then 6.6.6.1.1.4)
	frames(&ms, 2);
	udt_down(&ms, 1, &d, gps + 3, 2, false);
	frames(&ms, 1);
	t3_csbk_t s = status_ahoy(9, false, OWN, CALLER);
	rx(&ms, 1, &s);
	CHECK(ms.supl_rx.count == 4 && ms.supl_rx.src == CALLER && ms.status_rx.count == 1,
	      "sender named by the status: %06x count %u status %u", (unsigned)ms.supl_rx.src,
	      (unsigned)ms.supl_rx.count, (unsigned)ms.status_rx.count);
}

// Calls through a dialing gateway (6.6.2.2.3, table 6.41; 6.6.4.2, table 6.54): the request goes to the gateway with
// the Proxy Flag set by the digits' count, and the digits go up as BCD with SF 1 when the gateway asks (table B.3 for
// the Pad Nibble). A caller's number from a gateway (table 6.33) is kept and, with A, accepted
static void test_dial(void)
{
	t3_ms_t ms;
	setup(T3_OPT_PRIVATE, true);
	camp_registered(&ms);
	uint8_t bcd[T3_DIAL_MAX / 2];
	CHECK(t3_dial_pack("5551234", bcd) == 7 && bcd[0] == 0x55 && bcd[1] == 0x51 && bcd[2] == 0x23 && bcd[3] == 0x4F &&
	      bcd[4] == 0xFF, "packed: %02x %02x %02x %02x", bcd[0], bcd[1], bcd[2], bcd[3]);
	CHECK(t3_dial_pack("*0#", bcd) == 3 && bcd[0] == 0xA0 && bcd[1] == 0xBF, "* and #");
	CHECK(t3_dial_pack("12a", bcd) == 0 && t3_dial_pack("", bcd) == 0 &&
	      t3_dial_pack("123456789012345678901234567890123456789012345", bcd) == 0, "bad strings refused");
	CHECK(t3_dial_gw(T3_ID_PSTNI) && t3_dial_gw(T3_ID_PABXI) && t3_dial_gw(T3_ID_LINEI) && t3_dial_gw(T3_ID_DISPATI) &&
	      t3_dial_gw(T3_ID_PSTNDI) && t3_dial_gw(T3_ID_DISPATDI) && !t3_dial_gw(T3_ID_IPI) && !t3_dial_gw(T3_ID_SDMI),
	      "dialing gateways");

	// No digits, or to a talkgroup: refused
	CHECK(!t3_ms_request(&ms, T, T3_ID_PSTNI, false, false) && ms.ax.result == T3_RES_NOT_ALLOWED, "no digits");
	uint8_t digits[T3_DIAL_MAX / 2];
	cfg.dial = digits;
	cfg.dial_n = t3_dial_pack("5551234", digits);
	CHECK(!t3_ms_request(&ms, T, T3_ID_PSTNI, true, false) && ms.ax.result == T3_RES_NOT_ALLOWED, "not a talkgroup");

	// A voice call to the PSTN: an individual request to PSTNI, Proxy 0 (up to 20 digits), the digits when it asks
	nsent = 0;
	CHECK(t3_ms_request(&ms, T, T3_ID_PSTNI, false, false), "call to the PSTN");
	CHECK(sent_rand(&ms) && sent[0].u.rand.kind == T3_SK_IND_VOICE && sent[0].u.rand.dst == T3_ID_PSTNI &&
	      !sent[0].u.rand.proxy, "C_RAND to PSTNI, proxy %u", sent[0].u.rand.proxy);
	int up0 = nup;
	t3_csbk_t h = ahoy(T3_SK_IND_VOICE, false, OWN, T3_ID_PABXI);	// another gateway's: not ours
	rx(&ms, 1, &h);
	frames(&ms, 2);
	CHECK(nup == up0, "another gateway's ask: nothing");
	h = ahoy(T3_SK_IND_VOICE, false, OWN, T3_ID_PSTNI);
	rx(&ms, 1, &h);
	frames(&ms, 1);
	t3_udt_t u;
	CHECK(nup == up0 + 1 && nup_blocks == 1 && t3_udt_parse(up_hdr, &u) && u.op == T3_OP_C_UDTHU && u.sf &&
	      u.format == T3_UDTF_BCD && u.dst == T3_ID_PSTNI && u.src == OWN && u.pad == 13 && up_blocks[0][0] == 0x55 &&
	      up_blocks[0][1] == 0x51 && up_blocks[0][2] == 0x23 && up_blocks[0][3] == 0x4F && up_blocks[0][4] == 0xFF,
	      "digits uploaded: sf %u fmt %u pad %u", u.sf, u.format, u.pad);
	CHECK(ms.ax.state == T3_AX_ACKED, "the request goes on");
	t3_csbk_t g = grant(T3_OP_PV_GRANT, 895, 1, T3_ID_PSTNI, OWN);
	rx(&ms, 1, &g);
	CHECK(ms.state == T3_FOLLOW && ms.ax.result == T3_RES_GRANTED, "granted");
	t3_ms_end_call(&ms, T);
	frames(&ms, 6);
	CHECK(ms.state == T3_CAMPED, "back");
	frames(&ms, 20);

	// 21-44 digits: Proxy 1, two blocks
	cfg.dial_n = t3_dial_pack("123456789012345678901", digits);
	nsent = 0;
	bool started = t3_ms_request(&ms, T, T3_ID_PABXI, false, false);
	CHECK(started && sent_rand(&ms) && sent[0].u.rand.proxy, "proxy 1: started %u state %d ax %d res %d", started,
	      ms.state, ms.ax.state, ms.ax.result);
	h = ahoy(T3_SK_IND_VOICE, false, OWN, T3_ID_PABXI);
	h.u.ahoy.blocks = 1;
	rx(&ms, 1, &h);
	frames(&ms, 1);
	CHECK(nup_blocks == 2 && t3_udt_parse(up_hdr, &u) && u.dst == T3_ID_PABXI && u.pad == 23,
	      "21 digits: 2 blocks, pad %u", u.pad);
	nsent = 0;
	t3_ms_cancel(&ms, T);
	CHECK(sent_rand(&ms) && sent[nsent - 1].u.rand.kind == T3_SK_CANCEL, "canceled");
	t3_csbk_t a = ack(T3_OP_C_ACKD, T3_RC_MESSAGE_ACCEPTED, OWN, T3_ID_PABXI);
	rx(&ms, 1, &a);
	CHECK(ms.ax.state == T3_AX_IDLE, "idle again: %d", ms.ax.state);

	// Short data to the dispatcher: the digits when DISPATI asks, then the message when SDMI does
	cfg.dial_n = t3_dial_pack("42", digits);
	nsent = 0;
	CHECK(t3_ms_sdata(&ms, T, T3_ID_DISPATI, false, T3_UDTF_ISO8, (const uint8_t *)"hi", 2) && sent_rand(&ms) &&
	      sent[0].u.rand.dst == T3_ID_DISPATI, "short data to DISPATI");
	h = ahoy(T3_SK_IND_UDT, false, OWN, T3_ID_DISPATI);
	rx(&ms, 1, &h);
	frames(&ms, 1);
	CHECK(t3_udt_parse(up_hdr, &u) && u.sf && u.format == T3_UDTF_BCD && u.dst == T3_ID_DISPATI && up_blocks[0][0] == 0x42,
	      "digits first");
	h = ahoy(T3_SK_IND_UDT, false, OWN, T3_ID_SDMI);
	rx(&ms, 1, &h);
	frames(&ms, 1);
	CHECK(t3_udt_parse(up_hdr, &u) && !u.sf && u.format == T3_UDTF_ISO8 && memcmp(up_blocks, "hi", 2) == 0,
	      "then the message");
	a = ack(T3_OP_C_ACKD, T3_RC_MS_ACCEPTED, OWN, T3_ID_DISPATI);
	rx(&ms, 1, &a);
	CHECK(ms.ax.result == T3_RES_DONE, "delivered");

	// The caller's number from the PABX, to the radio with A: kept, accepted, not short data
	static const uint8_t num[5] = { 0x80, 0x05, 0x55, 0x12, 0x12 };	// 8005551212
	nsent = 0;
	uint32_t sd0 = ms.sd_rx.count;
	t3_udt_t d = { .op = T3_OP_C_UDTHD, .a = true, .sf = true, .format = T3_UDTF_BCD, .dst = OWN, .src = T3_ID_PABXI };
	udt_down(&ms, 1, &d, num, sizeof num, false);
	frames(&ms, 1);
	char text[T3_DIAL_MAX + 1];
	t3_bcd_text(ms.cli.bcd, ms.cli.n, text);
	CHECK(ms.cli.count == 1 && ms.cli.gw == T3_ID_PABXI && ms.cli.n == 10 && strcmp(text, "8005551212") == 0 &&
	      ms.sd_rx.count == sd0, "number kept: %u digits \"%s\"", ms.cli.n, text);
	CHECK(nsent == 1 && sent[0].op == T3_OP_C_ACKU && sent[0].u.ack.reason == T3_RC_MESSAGE_ACCEPTED &&
	      sent[0].u.ack.dst == T3_ID_PABXI, "accepted");
	udt_down(&ms, 1, &d, num, sizeof num, false);
	frames(&ms, 1);
	CHECK(ms.cli.count == 1 && nsent == 2, "repeat answered, not counted again");
	// To a talkgroup the radio follows: kept, not answered
	d.group = true;
	d.a = false;
	d.dst = TG_B;
	udt_down(&ms, 1, &d, num, 2, false);
	frames(&ms, 2);
	CHECK(ms.cli.count == 2 && ms.cli.group && ms.cli.n == 4 && nsent == 2, "talkgroup's: kept, %u digits", ms.cli.n);
	cfg.dial = NULL;
	cfg.dial_n = 0;
}

// A gateway's caller number belongs to the call it precedes (figure 6.45): the grant from that gateway to the same
// destination within T3_CLI_CALL_MS. Later, or from another source, the call has no number
static void test_cli_call(void)
{
	t3_ms_t ms;
	setup(T3_OPT_PRIVATE, true);
	camp_registered(&ms);
	static const uint8_t num[4] = { 0x55, 0x51, 0x23, 0x4F };	// 5551234
	t3_udt_t d = { .op = T3_OP_C_UDTHD, .a = true, .sf = true, .format = T3_UDTF_BCD, .dst = OWN, .src = T3_ID_PSTNI };
	struct {
		uint32_t src;
		int wait;	// frames between the number and the grant
		bool cli;
		const char *what;
	} c[] = {
		{ T3_ID_PSTNI, 4, true, "the gateway's call: its number" },
		{ T3_ID_PSTNI, 170, false, "granted 10.2 s after the number: none" },
		{ T3_ID_PABXI, 4, false, "another gateway's call: none" },
		{ CALLER, 4, false, "a radio's call: none" },
	};
	for (unsigned i = 0; i < sizeof c / sizeof c[0]; i++) {
		udt_down(&ms, 1, &d, num, sizeof num, false);
		frames(&ms, c[i].wait);
		t3_csbk_t g = grant(T3_OP_PV_GRANT, 999, 1, OWN, c[i].src);
		rx(&ms, 1, &g);
		CHECK(ms.state == T3_FOLLOW && ms.call.cli == c[i].cli, "%s (state %d, cli %d)", c[i].what, ms.state,
		      ms.call.cli);
		t3_ms_end_call(&ms, T);
		frames(&ms, 6);
		CHECK(ms.state == T3_CAMPED, "camped again");
		frames(&ms, 140);	// past T3_REPEAT_MS: the next number is a new one
	}
}

// Camped on a TSCC that uses slot 1 only, the carrier's slot 2 is a payload channel: its P_AHOY (C_AHOY's opcode) is
// not the TSCC's and gets no answer. An Aloha on slot 2 makes it a control slot too
static void test_other_slot(void)
{
	t3_ms_t ms;
	setup(T3_OPT_PRIVATE, true);
	camp_registered(&ms);
	nsent = 0;
	t3_csbk_t h = ahoy(T3_SK_IND_VOICE, false, OWN, CALLER);
	rx(&ms, 2, &h);
	frames(&ms, 2);
	CHECK(nsent == 0, "payload slot's P_AHOY ignored: %d sent", nsent);
	rx(&ms, 1, &h);
	frames(&ms, 1);
	CHECK(nsent == 1 && sent[0].op == T3_OP_C_ACKU && sent_act[0].slot == 1, "the TSCC's answered");
	t3_csbk_t a = { .lb = true, .op = T3_OP_C_ALOHA };
	a.u.aloha = al;
	rx(&ms, 2, &a);
	rx(&ms, 2, &h);
	frames(&ms, 1);
	CHECK(nsent == 2 && sent_act[1].slot == 2, "a control slot after its Aloha");
}

int main(void)
{
	test_register();
	test_reg_list();
	test_no_answer();
	test_ra_log();
	test_ra_on_air();
	test_composite();
	test_composite_late_entry();
	test_composite_no_slc();
	test_area_unset();
	test_refused_denied();
	test_permission();
	test_group_call();
	test_group_heard();
	test_private_call();
	test_cancel();
	test_ahoy();
	test_other_slot();
	test_broadcast();
	test_stun();
	test_stun_off();
	test_groups_changed();
	test_auth();
	test_interrupt();
	test_sdpoll();
	test_not_registered();
	test_mass_reg();
	test_move();
	test_power_save();
	test_dgna();
	test_divert();
	test_als();
	test_acvit();
	test_status();
	test_sdata();
	test_supl();
	test_dial();
	test_cli_call();
	test_pdata();
	test_pdata_ip();
	test_ip_advice();
	test_deregister();
	test_receive_rules();
	test_one_carrier_site();
	test_per_slot_lcn();
	printf("t3AccessTest: %d checks, %d failures\n", checks, failures);
	return failures != 0;
}
