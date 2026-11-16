// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The second receiver's SoC as the main SoC's second DMR transmitter (rx2_dmr.h, docs/spec/rx2-link.md 2.4)
#include <string.h>
#include "rx2_dmr.h"

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

// --- Main side ---

static rx2_dmr_main_cfg_t mcfg;
static rx2_dmr_main_state_t ms;
static dmr_event_cb_t mcb;
static void *mctx;

static bool msend1(uint8_t op)
{
	return mcfg.send(&op, 1);
}

static void mevent(uint8_t kind, int8_t ts)
{
	if (mcb != NULL) {
		dmr_event_t ev = { .kind = kind, .ts = ts };
		mcb(&ev, mctx);
	}
}

// Back to the main SoC's own transmitter
// The receiver is given up on: it is told to drop the role too (else it would stay out of its receiving until the
// main SoC restarts, and a later tx_split(false) would find nothing to turn off)
static void mfallback(void)
{
	if (ms.remote_tx) {
		msend1(RX2_DMR_ABORT);
	}
	uint8_t p[3] = { RX2_DMR_SPLIT, 0u, (uint8_t)ms.cc };
	mcfg.send(p, 3);
	ms.split = ms.ready = ms.remote = ms.remote_tx = false;
	ms.arm_hz = 0;
	ms.fallbacks++;
	if (mcfg.split != NULL) {
		mcfg.split(false);
	}
}

static bool m_start(const dmr_config_t *cfg, dmr_event_cb_t cb, void *ctx)
{
	mcb = cb;
	mctx = ctx;
	ms.cc = cfg->cc;
	return mcfg.local->start(cfg, cb, ctx);
}

static void m_tx_split_stats(dmr_split_stats_t *s)
{
	s->ready = ms.ready;
	s->remote = ms.remote;
	s->events = ms.events;
	s->bursts = ms.bursts;
	s->ends_forced = ms.ends_forced;
	s->fallbacks = ms.fallbacks;
}

static bool m_tx_split(bool on)
{
	if (on == ms.split) {
		return on;
	}
	uint8_t p[3] = { RX2_DMR_SPLIT, on ? 1u : 0u, (uint8_t)ms.cc };
	if (!on) {
		if (ms.remote_tx) {
			msend1(RX2_DMR_ABORT);
		}
		mcfg.send(p, 3);
		ms.split = ms.ready = ms.remote = ms.remote_tx = false;
		ms.arm_hz = 0;
		if (mcfg.split != NULL) {
			mcfg.split(false);
		}
		return false;
	}
	if (!mcfg.send(p, 3)) {
		return false;	// down, or no transmitter there
	}
	ms.split = true;
	ms.ready = ms.remote = ms.remote_tx = false;
	ms.split_ms = ms.heard_ms = ms.now_ms;
	if (mcfg.split != NULL) {
		mcfg.split(true);	// the board arms the receiver's transmitter where its own was armed
	}
	uint32_t hz = 0;
	uint8_t power = 0;
	if (mcfg.listen != NULL && mcfg.listen(&hz, &power) && hz != 0) {
		rx2_dmr_main_arm(hz, power);	// listening from now: the slot timing before the first repeat
	}
	return true;
}

static bool m_tx_begin(dmr_timing_t timing, int8_t ts, dmr_access_t access)
{
	ms.remote = false;
	if (ms.split && !ms.ready) {
		if ((int32_t)(ms.now_ms - ms.split_ms) < (int32_t)RX2_DMR_READY_MS) {
			return false;	// not armed yet: the caller tries again
		}
		mfallback();
	}
	if (!ms.split) {
		return mcfg.local->tx_begin(timing, ts, access);
	}
	uint8_t p[4] = { RX2_DMR_BEGIN, (uint8_t)timing, (uint8_t)ts, (uint8_t)access };
	if (!mcfg.send(p, 4)) {
		mfallback();
		return mcfg.local->tx_begin(timing, ts, access);
	}
	ms.remote = ms.remote_tx = true;
	ms.ending = false;
	ms.sent = ms.taken;	// nothing on the link yet
	ms.heard_ms = ms.now_ms;
	return true;
}

static bool m_tx_queue(const dmr_tx_burst_t *b)
{
	if (!ms.remote) {
		return mcfg.local->tx_queue(b);
	}
	uint8_t p[6 + DMR_PAYLOAD_MAX];
	uint8_t n = b->len <= DMR_PAYLOAD_MAX ? b->len : DMR_PAYLOAD_MAX;
	p[0] = RX2_DMR_BURST;
	p[1] = b->kind;
	p[2] = b->voice_index;
	p[3] = b->host_crc ? 1u : 0u;
	p[4] = b->sync_override;
	p[5] = n;
	memcpy(&p[6], b->payload, n);
	if (!ms.remote_tx || !mcfg.send(p, (uint8_t)(6u + n))) {
		return false;
	}
	ms.sent++;
	ms.bursts++;
	return true;
}

static uint8_t m_tx_queue_space(void)
{
	if (!ms.remote) {
		return mcfg.local->tx_queue_space();
	}
	uint8_t on_link = (uint8_t)(ms.sent - ms.taken);
	return ms.space > on_link ? (uint8_t)(ms.space - on_link) : 0u;
}

static void m_tx_end(void)
{
	if (!ms.remote) {
		mcfg.local->tx_end();
		return;
	}
	ms.ending = true;
	msend1(RX2_DMR_END);
}

static void m_tx_abort(void)
{
	if (!ms.remote) {
		mcfg.local->tx_abort();
		return;
	}
	msend1(RX2_DMR_ABORT);
	ms.remote_tx = ms.ending = false;
}

static void m_set_embedded_lc(const uint8_t lc[9])
{
	if (ms.split) {
		uint8_t p[10] = { RX2_DMR_EMB };
		memcpy(&p[1], lc, 9);
		mcfg.send(p, 10);
	}
	mcfg.local->set_embedded_lc(lc);
}

static bool m_is_transmitting(void)
{
	return ms.remote ? ms.remote_tx : mcfg.local->is_transmitting();
}

static void m_tx_listen(void)
{
	if (ms.remote) {
		msend1(RX2_DMR_LISTEN);	// the receiver's chip takes its slot timing from the inbound bursts
	} else if (mcfg.local->tx_listen != NULL) {
		mcfg.local->tx_listen();
	}
}

static int8_t m_tx_fill(const dmr_tx_burst_t *b)
{
	if (ms.remote || mcfg.local->tx_fill == NULL) {
		return -1;	// the caller queues it
	}
	return mcfg.local->tx_fill(b);
}

// A new color code (a hotspot's or an SFR's config after the start): the receiver filters on it too. Its role taken
// again with it, then armed where it was, so its modem hears the inbound and keeps the slot timing
static void m_set_cc_filter(int8_t cc)
{
	if (mcfg.local->set_cc_filter != NULL) {
		mcfg.local->set_cc_filter(cc);
	}
	if (cc == ms.cc) {
		return;
	}
	ms.cc = cc;
	if (!ms.split) {
		return;
	}
	uint8_t p[3] = { RX2_DMR_SPLIT, 1u, (uint8_t)cc };
	mcfg.send(p, 3);
	if (ms.arm_hz != 0) {
		rx2_dmr_main_arm(ms.arm_hz, ms.arm_power);	// in order on the link: a begin after this finds it armed
	} else {
		ms.ready = false;
	}
}

void rx2_dmr_main_init(const rx2_dmr_main_cfg_t *cfg, hal_dmr_modem_t *out)
{
	mcfg = *cfg;
	memset(&ms, 0, sizeof(ms));
	mcb = NULL;
	mctx = NULL;
	*out = *cfg->local;
	out->start = m_start;
	out->tx_split = m_tx_split;
	out->tx_split_stats = m_tx_split_stats;
	out->tx_begin = m_tx_begin;
	out->tx_queue = m_tx_queue;
	out->tx_queue_space = m_tx_queue_space;
	out->tx_end = m_tx_end;
	out->tx_abort = m_tx_abort;
	out->set_embedded_lc = m_set_embedded_lc;
	out->is_transmitting = m_is_transmitting;
	out->tx_listen = m_tx_listen;
	out->tx_fill = m_tx_fill;
	out->set_cc_filter = m_set_cc_filter;
}

bool rx2_dmr_main_arm(uint32_t hz, uint8_t power)
{
	if (hz == 0) {
		return ms.split;	// it keeps listening (rx2_dmr.h); tx_split(false) disarms
	}
	ms.arm_hz = hz;
	ms.arm_power = power;
	uint8_t p[6] = { RX2_DMR_ARM };
	put32(&p[1], hz);
	p[5] = power;
	return ms.split && mcfg.send(p, 6);
}

bool rx2_dmr_main_split(void)
{
	return ms.split;
}

void rx2_dmr_main_event(const uint8_t *p, uint8_t len, uint32_t now_ms)
{
	if (len < RX2_DMR_EV_LEN || !ms.split) {
		return;
	}
	ms.events++;
	ms.heard_ms = now_ms;
	ms.space = p[2];
	ms.ready = (p[3] & RX2_DMR_ST_READY) != 0;
	ms.taken = p[4];
	uint8_t kind = p[0];
	if (!ms.remote || kind == RX2_DMR_EV_STATE) {
		return;
	}
	if (kind == DMR_EV_TX_DONE || kind == DMR_EV_TX_REJECTED) {
		if (!ms.remote_tx) {
			return;		// aborted here meanwhile
		}
		ms.remote_tx = ms.ending = false;
	}
	if (kind == DMR_EV_TX_SLOT_STARTED || kind == DMR_EV_TX_DONE || kind == DMR_EV_TX_REJECTED) {
		mevent(kind, (int8_t)p[1]);
	}
}

void rx2_dmr_main_poll(bool up, uint32_t now_ms)
{
	ms.now_ms = now_ms;
	if (!ms.split) {
		return;
	}
	if (!up) {
		ms.ready = false;
	}
	if (ms.remote_tx && (!up || (int32_t)(now_ms - ms.heard_ms) > (int32_t)RX2_DMR_QUIET_MS)) {
		// Nothing more will come: the transmission ends here, so the caller never waits for it
		ms.remote_tx = false;
		ms.ends_forced++;
		mevent(ms.ending ? DMR_EV_TX_DONE : DMR_EV_TX_REJECTED, 0);
		ms.ending = false;
	}
}

const rx2_dmr_main_state_t *rx2_dmr_main_state(void)
{
	return &ms;
}

// --- Receiver side ---

#define REV_QUEUE	8u

static rx2_dmr_recv_cfg_t rcfg;
static rx2_dmr_recv_state_t rs;
static volatile uint8_t rq_head, rq_tail;
static uint8_t rq[REV_QUEUE][2];	// kind, ts
static uint8_t r_last_state;
static uint32_t r_beat_ms;

static uint8_t rstate(void)
{
	return (uint8_t)((rs.split && rs.armed ? RX2_DMR_ST_READY : 0u) | (rs.tx ? RX2_DMR_ST_TX : 0u));
}

static void rpush(uint8_t kind, int8_t ts)
{
	uint8_t next = (uint8_t)((rq_head + 1u) % REV_QUEUE);
	if (next == rq_tail) {
		rs.dropped++;
		return;
	}
	rq[rq_head][0] = kind;
	rq[rq_head][1] = (uint8_t)ts;
	rq_head = next;
}

static void rsend(uint8_t kind, uint8_t ts)
{
	uint8_t space = rs.tx ? rcfg.modem->tx_queue_space() : 0u;
	uint8_t p[RX2_DMR_EV_LEN] = { kind, ts, space, rstate(), rs.taken };
	r_last_state = p[3];
	rcfg.send(p, RX2_DMR_EV_LEN);
}

static void roff(void)
{
	if (rs.tx) {
		rcfg.modem->tx_abort();
		rs.tx = false;
	}
	if (rs.armed) {
		rcfg.arm(0, 0);
		rs.armed = false;
		rs.hz = 0;
	}
	if (rs.split) {
		rcfg.split(false, 0);
		rs.split = false;
	}
}

void rx2_dmr_recv_init(const rx2_dmr_recv_cfg_t *cfg)
{
	rcfg = *cfg;
	memset(&rs, 0, sizeof(rs));
	rq_head = rq_tail = 0;
	r_last_state = 0;
	r_beat_ms = 0;
}

void rx2_dmr_recv(const uint8_t *p, uint8_t len)
{
	if (p == NULL || len == 0) {
		roff();
		return;
	}
	rs.requests++;
	switch (p[0]) {
	case RX2_DMR_SPLIT:
		if (len < 3) {
			break;
		}
		if (p[1] == 0) {
			roff();
		} else if (!rs.split || rs.cc != (int8_t)p[2]) {
			roff();
			rs.cc = (int8_t)p[2];
			rs.split = rcfg.split(true, rs.cc);
		}
		break;
	case RX2_DMR_ARM: {
		if (len < 6 || !rs.split) {
			break;
		}
		uint32_t hz = get32(&p[1]);
		if (hz == rs.hz && rs.armed && p[5] == rs.power) {
			break;		// already listening there: its slot timing kept
		}
		if (rs.tx) {
			rcfg.modem->tx_abort();
			rs.tx = false;
		}
		rs.armed = hz != 0 && rcfg.arm(hz, p[5]);
		rs.hz = rs.armed ? hz : 0;
		rs.power = p[5];
		if (hz != 0 && !rs.armed) {
			rs.arm_fails++;
		}
		if (!rs.armed && hz == 0) {
			rcfg.arm(0, 0);
		}
		break;
	}
	case RX2_DMR_BEGIN:
		if (len < 4) {
			break;
		}
		if (rs.tx) {
			rcfg.modem->tx_abort();	// a new transmission: the main side has given up on the last one
			rs.tx = false;
		}
		if (!rs.split || !rs.armed ||
		    !rcfg.modem->tx_begin((dmr_timing_t)p[1], (int8_t)p[2], (dmr_access_t)p[3])) {
			rs.refused++;
			rpush(DMR_EV_TX_REJECTED, (int8_t)p[2]);
			break;
		}
		rs.tx = true;
		break;
	case RX2_DMR_LISTEN:
		if (rs.tx && rcfg.modem->tx_listen != NULL) {
			rcfg.modem->tx_listen();
		}
		break;
	case RX2_DMR_BURST: {
		rs.taken++;	// counted even when dropped: the main side's count of bursts on the link
		if (len < 6 || !rs.tx) {
			break;
		}
		dmr_tx_burst_t b;
		memset(&b, 0, sizeof(b));
		b.kind = p[1];
		b.voice_index = p[2];
		b.host_crc = (p[3] & 1u) != 0;
		b.sync_override = p[4];
		b.len = p[5] <= DMR_PAYLOAD_MAX ? p[5] : DMR_PAYLOAD_MAX;
		if ((uint8_t)(len - 6u) < b.len) {
			break;
		}
		memcpy(b.payload, &p[6], b.len);
		if (rcfg.modem->tx_queue(&b)) {
			rs.bursts++;
		}
		break;
	}
	case RX2_DMR_END:
		if (rs.tx) {
			rcfg.modem->tx_end();
		}
		break;
	case RX2_DMR_ABORT:
		if (rs.tx) {
			rcfg.modem->tx_abort();
			rs.tx = false;
		}
		break;
	case RX2_DMR_EMB:
		if (len >= 10 && rs.split) {
			rcfg.modem->set_embedded_lc(&p[1]);
		}
		break;
	default:
		break;
	}
}

void rx2_dmr_recv_event(const dmr_event_t *ev)
{
	switch (ev->kind) {
	case DMR_EV_TX_SLOT_STARTED:
	case DMR_EV_TX_REJECTED:
	case DMR_EV_TX_DONE:
		rpush(ev->kind, ev->ts);
		break;
	default:
		break;	// what it receives is the main SoC's business
	}
}

void rx2_dmr_recv_poll(uint32_t now_ms)
{
	while (rq_tail != rq_head) {
		uint8_t kind = rq[rq_tail][0], ts = rq[rq_tail][1];
		rq_tail = (uint8_t)((rq_tail + 1u) % REV_QUEUE);
		if (kind == DMR_EV_TX_DONE || kind == DMR_EV_TX_REJECTED) {
			rs.tx = false;
		}
		rsend(kind, ts);
		r_beat_ms = now_ms;
	}
	if (rs.split || r_last_state != 0) {
		if (rstate() != r_last_state || (int32_t)(now_ms - r_beat_ms) >= (int32_t)RX2_DMR_BEAT_MS) {
			rsend(RX2_DMR_EV_STATE, 0);
			r_beat_ms = now_ms;
		}
	}
}

const rx2_dmr_recv_state_t *rx2_dmr_recv_state(void)
{
	return &rs;
}
