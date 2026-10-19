// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// HR_C7000 DMR slot engine (hrc7000_dmr.h; docs/spec/modem-hrc7000.md 3, 5, 6)
#include <string.h>
#include "hrc7000_dmr.h"

// RX_TYPE_INFO (2.7)
#define RTI_ERR_MASK		0x7F000000u	// bits 30:24
#define RTI_CRC8		(1u << 30)
#define RTI_GOLAY		(1u << 29)
#define RTI_QR			(1u << 28)
#define RTI_CS			(1u << 27)
#define RTI_CRC16		(1u << 26)
#define RTI_RS			(1u << 25)
#define RTI_BLOCK		(1u << 24)

// System interrupt causes (2.4)
#define SYS_LATE_LC		(1u << 2)
#define SYS_DEFRAME		(1u << 3)
#define SYS_ABNORMAL		((1u << 5) | (1u << 6) | (1u << 16))

// LAYER2 values (2.5)
#define L2_PASSIVE		0xC0u
#define L2_ACTIVE		0xC3u
#define L2_RX_FOLLOW		0xC4u	// b2 tx_slave_auto_mode: TX slots follow the receiver's (receive through a repeater)
// Active timing with the receive slot edges following the received sync (b3 rx_master_auto_update): without it the
// receiver keeps our own slot edges while we send, and a station in the other slot isn't deframed (2.5)
#define L2_ACTIVE_RX_FOLLOW	0xCBu
#define TXRX_NONE		0x00u
#define TXRX_RX			0x40u
#define TXRX_TX			0x80u
#define TXRX_RX_VOICE		0x8040u
#define TXRX_SYNC_RELEASE	0x800000u	// b23: release the receive slot sync
#define TXRX_TX_SYNC_RELEASE	0x400000u	// b22: release the transmit slot sync
#define SLOTON_OFF		0x00u
#define SLOTON_RXTX		0x05u
// b5 dll_rx_open_tx (write 1, clears itself): the TX slot built from the receive slot, with LAYER2_CONTROL passive
// (tx_slave_auto_mode 0) and b0 dll_tx_slot_on set (DS 10.2.2.17)
#define SLOTON_RX_OPEN_TX	0x20u

// The chip slot the RX timeslot interrupt decides is the one after the slot number LAYER2_STATUS b30 shows then
// (assumed: the interrupt comes 1.6 ms ahead of the boundary, LAYER2_SLOT_PRE). A board whose interrupt comes after
// the boundary sets 0 (the DB25-D): with the flag on the other slot the chip leaves the voice RAM unwritten, and every
// voice burst reads the same stale bytes
#ifndef HRC7000_RX_NEXT_SLOT_XOR
#define HRC7000_RX_NEXT_SLOT_XOR	1u
#endif

#define VOICE_LOST_TICKS	13u	// about 390 ms without a voice burst on the call's slot
#define CC_SCAN_TICKS		4u	// color code scan: next code after 120 ms of nothing
#define CC_HOLD_TICKS		167u	// ... held 5 s after the last burst
#define BUSY_TICKS		12u	// polite access: activity in the last 360 ms
#define RX_LOST_TICKS		13u	// a reception is over (390 ms without a burst taken)
#define RX_IDLE_TICKS		20u	// slot timing but nothing taken for 600 ms: the timing is stale
#define SLOT_US			30000u
#define TC_LOCK			4u	// a repeater's slot numbering: this many BS bursts in a row with TC alternating
#define TC_LOCK_WAIT_TICKS	33u	// an aligned start through a repeater waits this long for the lock (1 s)
#define PA_OFF_US		20000u	// the PA goes off at the first tick at least this long after the RF TX interrupt
#define STALL_US		200000u
#define SYS_LOOPS		4u

// TXS_LOCK: an aligned start through a repeater waits, still receiving, for the TC lock before it arms
enum { TXS_IDLE, TXS_LOCK, TXS_ARMING, TXS_RUN, TXS_DRAIN };
#define KIND_UNSET		0xFEu

static const uint8_t ambe_silence[9] = { 0xB9, 0xE8, 0x81, 0x52, 0x61, 0x73, 0x00, 0x2A, 0x6B };

// --- Pure helpers ---

void hrc7000_dmr_decode(uint32_t rti, hrc7000_rx_status_t *st)
{
	uint8_t vb = (uint8_t)((rti >> 12) & 0x7u);
	uint8_t dt = (uint8_t)((rti >> 8) & 0xFu);
	uint8_t sync_type = (uint8_t)(rti & 0x3u);
	static const uint8_t fam[4] = { DMR_SYNC_MS, DMR_SYNC_BS, DMR_SYNC_TS1, DMR_SYNC_TS2 };

	st->cc = (uint8_t)((rti >> 4) & 0xFu);
	st->sync = fam[(rti >> 2) & 0x3u];
	st->tc = (uint8_t)((rti >> 22) & 0x1u);
	st->chip_slot = (uint8_t)((rti >> 19) & 0x1u);
	st->pi = ((rti >> 17) & 0x1u) != 0;
	st->lcss = (uint8_t)((rti >> 15) & 0x3u);
	st->voice_index = 0;
	if (vb >= 1 && vb <= 6) {
		st->kind = DMR_BURST_VOICE;
		st->voice_index = (uint8_t)(vb - 1u);
		// Burst A has no EMB (nothing to check); B-F carry the QR coded EMB
		st->crc_ok = vb == 1 ? true : (rti & RTI_QR) == 0;
		return;
	}
	if (sync_type != 2 || dt > DMR_BURST_RATE_1_DATA) {
		st->kind = HRC7000_KIND_NONE;	// RC, EMB-only without a number, reserved types
		st->crc_ok = false;
		return;
	}
	st->kind = dt;
	switch (dt) {
	case DMR_BURST_VOICE_LC_HEADER:
	case DMR_BURST_TERMINATOR_LC:
		st->crc_ok = (rti & (RTI_RS | RTI_GOLAY)) == 0;
		break;
	case DMR_BURST_RATE_12_DATA:
	case DMR_BURST_RATE_34_DATA:
	case DMR_BURST_RATE_1_DATA:
		st->crc_ok = (rti & (RTI_BLOCK | RTI_GOLAY)) == 0;	// informative: the packet CRC-32 decides
		break;
	case DMR_BURST_IDLE:
		st->crc_ok = (rti & RTI_GOLAY) == 0;
		break;
	default:	// PI header, CSBK, MBC, data header
		st->crc_ok = (rti & (RTI_CRC16 | RTI_GOLAY)) == 0;
		break;
	}
}

uint8_t hrc7000_dmr_payload_len(uint8_t kind)
{
	switch (kind) {
	case DMR_BURST_VOICE:
		return DMR_VOICE_BYTES;
	case DMR_BURST_RATE_34_DATA:
		return 18;
	case DMR_BURST_RATE_1_DATA:
		return 24;
	default:
		return 12;
	}
}

uint32_t hrc7000_dmr_send_type(uint8_t kind, uint8_t voice_index, bool lcss)
{
	static const uint8_t lcss_of[6] = { 0, 1, 3, 3, 2, 0 };	// user guide superframe: 0x08 0x19 0x2B 0x3B 0x4A 0x50
	if (kind == DMR_BURST_VOICE) {
		uint8_t i = voice_index % 6u;
		return 0x08u | ((uint32_t)i << 4) | (lcss ? lcss_of[i] : 0u);
	}
	return (uint32_t)(kind & 0xFu) << 4;
}

uint8_t hrc7000_dmr_emb_checksum(const uint8_t lc[9])
{
	uint32_t s = 0;
	for (int i = 0; i < 9; i++) {
		s += lc[i];
	}
	return (uint8_t)(s % 31u);
}

// --- Plumbing ---

static inline void w(hrc7000_dmr_t *e, uint32_t off, uint32_t v) { hrc7000_wr(e->dev, off, v); }
static inline uint32_t r(hrc7000_dmr_t *e, uint32_t off) { return hrc7000_rd(e->dev, off); }
static inline uint32_t now(hrc7000_dmr_t *e) { return e->ops->now_us(e->ops->ctx); }
static void lock(hrc7000_dmr_t *e) { if (e->ops->lock != NULL) e->ops->lock(e->ops->ctx); }
static void unlock(hrc7000_dmr_t *e) { if (e->ops->unlock != NULL) e->ops->unlock(e->ops->ctx); }

static uint8_t txq_count(const hrc7000_dmr_t *e)
{
	return (uint8_t)((e->txq_head + HRC7000_DMR_TXQ_DEPTH - e->txq_tail) % HRC7000_DMR_TXQ_DEPTH);
}

// Event slot to fill, or NULL when the queue is full (counted)
static hrc7000_dmr_evslot_t *ev_alloc(hrc7000_dmr_t *e)
{
	uint8_t next = (uint8_t)((e->evq_head + 1u) % HRC7000_DMR_EVQ_DEPTH);
	if (next == e->evq_tail) {
		e->st.ev_overflow++;
		return NULL;
	}
	hrc7000_dmr_evslot_t *s = &e->evq[e->evq_head];
	memset(&s->ev, 0, sizeof s->ev);
	return s;
}

static void ev_commit(hrc7000_dmr_t *e)
{
	e->evq_head = (uint8_t)((e->evq_head + 1u) % HRC7000_DMR_EVQ_DEPTH);
	if (e->ops->notify != NULL) {
		e->ops->notify(e->ops->ctx);
	}
}

static void ev_simple(hrc7000_dmr_t *e, uint8_t kind, int8_t ts, uint32_t arg)
{
	hrc7000_dmr_evslot_t *s = ev_alloc(e);
	if (s == NULL) {
		return;
	}
	s->ev.kind = kind;
	s->ev.ts = ts;
	s->ev.arg = arg;
	ev_commit(e);
}

static uint32_t tick_arg(const hrc7000_dmr_t *e)
{
	return e->tick != 0 ? e->tick : 1u;
}

// 5.2: the DMR state machine reset through FM (step 1, the WORK_MODE pair of 5.1), burst F EMB from the chip, passive
// timing, drop slot sync, receive the next slot, slots on
// LAYER2_CONTROL while receiving. Through a repeater the TX slots follow the receiver from the start (0xC4, as stock
// does), so a transmission needs no LAYER2 write: written at the key-up instead it left the receiver without slot sync
// for about 1 s (2.5), and building the TX slot from the receive slot there (SLOTON b5) put the bursts on the
// repeater's grid only for one of the two slots (issue 72)
static uint32_t l2_rx(const hrc7000_dmr_t *e)
{
	return e->cfg.repeater ? L2_RX_FOLLOW : L2_PASSIVE;
}

static void chip_rx_restart(hrc7000_dmr_t *e)
{
	w(e, C7_WORK_MODE, C7_WORK_MODE_FM);
	w(e, C7_WORK_MODE, C7_WORK_MODE_DMR);
	w(e, C7_VOICE_EMB_CTRL, 0x02);
	w(e, C7_LAYER2_CONTROL, l2_rx(e));
	w(e, C7_LAYER2_SLOTON, SLOTON_OFF);
	// The TX slot sync too, as stock does: kept, the TX slots stayed at the phase of our last transmission (the
	// wake-up, in our own timing) instead of taking the receiver's, and calls went out 10-20 ms off the repeater's
	// slot grid (issue 72, SDR bench, 2026-10-18). An SFR's aligned start builds its TX slot from the receiver the
	// same way, so every channel releases both
	w(e, C7_LAYER2_TXRX_CTRL, TXRX_SYNC_RELEASE | TXRX_TX_SYNC_RELEASE);
	w(e, C7_LAYER2_TXRX_CTRL, TXRX_RX);
	w(e, C7_LAYER2_SLOTON, SLOTON_RXTX);
	e->voice_chip_slot = -1;
	e->tc_slot[0] = e->tc_slot[1] = 0;
	e->tc_agree = 0;
	e->emb_pending = false;
	e->resync_tick = e->tick;
	e->st.rx_restarts++;
}

// The receiver's slot timing is dropped (5.2) when it stops giving bursts: at the end of a reception, and while the
// chip keeps the timing of a sender that left (it then only reports unusable and abnormal slots and takes no new
// call). Timeslot interrupts stop once the timing is released, so an empty channel sees no more restarts. Only with
// a color code set: the color code scan restarts the receiver itself
static void rx_resync(hrc7000_dmr_t *e)
{
	if (e->cc_filter < 0) {
		return;
	}
	uint32_t quiet = e->tick - (e->rx_any ? e->rx_tick : e->resync_tick);
	if (quiet > (e->rx_any ? RX_LOST_TICKS : RX_IDLE_TICKS)) {
		if (e->rx_any) {
			e->st.rx_lost++;
		} else {
			e->st.rx_stale++;
		}
		chip_rx_restart(e);
		e->rx_any = false;
	}
}

static void set_cc(hrc7000_dmr_t *e, uint8_t cc)
{
	e->cc_cur = cc & 0xFu;
	w(e, C7_LOCAL_CC, e->cc_cur);	// cc_opt 0: deframe only this color code
}

static void rf_off(hrc7000_dmr_t *e)
{
	if (e->rf_tx) {
		e->rf_tx = false;
		uint32_t ms = (now(e) - e->t_rf_tx) / 1000u;
		e->st.last_pa_ms = (uint8_t)(ms > 255u ? 255u : ms);
		e->ops->rf(e->ops->ctx, false);
		ev_simple(e, DMR_EV_TX_SLOT_ENDED, e->tx_ts, tick_arg(e));
	}
}

// End of a transmission: RF off, queue emptied, TX_DONE, receiver back (6.3)
static void tx_finish(hrc7000_dmr_t *e)
{
	w(e, C7_LAYER2_TXRX_CTRL, TXRX_NONE);
	rf_off(e);
	e->txq_tail = e->txq_head;
	e->tx_state = TXS_IDLE;
	e->tx_listen = false;
	e->st.tx_done++;
	// Through a repeater too, as stock does: both slot syncs released and a new TC lock, so the next transmission's
	// slots come from the repeater's timing, not ours (issue 72)
	chip_rx_restart(e);
	ev_simple(e, DMR_EV_TX_DONE, e->tx_ts, tick_arg(e));
}

// --- Transmit ---

static void write_tx_ram(hrc7000_dmr_t *e, const uint8_t *p, uint8_t n)
{
	for (uint8_t i = 0; i < n; i++) {
		e->dev->buf[C7_TX_RAM + i] = p[i];
	}
}

// The data sync pattern the next burst is sent with (2.2); the chip sends a voice burst with its complement
static void set_send_sync(hrc7000_dmr_t *e, uint8_t family)
{
	static const uint32_t pattern[][2] = {
		[DMR_SYNC_MS] = { 0xD5D7F7, 0x7FD757 }, [DMR_SYNC_BS] = { 0xDFF57D, 0x75DF5D },
		[DMR_SYNC_TS1] = { 0xF7FDD5, 0xDDFD55 }, [DMR_SYNC_TS2] = { 0xD7557F, 0x5FF7F5 },
	};
	if (family == e->send_sync || family < DMR_SYNC_MS || family > DMR_SYNC_TS2) {
		return;
	}
	w(e, C7_SEND_DATA_SYNC_H, pattern[family][0]);
	w(e, C7_SEND_DATA_SYNC_L, pattern[family][1]);
	e->send_sync = family;
}

static void arm(hrc7000_dmr_t *e, const dmr_tx_burst_t *b)
{
	uint8_t kind = b->kind;
	set_send_sync(e, b->sync_override != DMR_SYNC_NONE ? b->sync_override : DMR_SYNC_MS);
	if (kind == DMR_BURST_VOICE) {
		uint8_t idx = (uint8_t)(b->voice_index % 6u);
		for (uint8_t i = 0; i < DMR_VOICE_BYTES; i++) {
			e->dev->buf[C7_VOICE_RAM + i] = i < b->len ? b->payload[i] : ambe_silence[i % 9u];
		}
		if (idx == 0) {
			// Burst A: the LC the chip spreads over B-E, with its 5-bit checksum (2.8)
			const uint8_t *lc = e->emb_set ? e->emb_lc : e->call_lc;
			uint8_t buf10[10];
			memcpy(buf10, lc, 9);
			buf10[9] = hrc7000_dmr_emb_checksum(lc);
			write_tx_ram(e, buf10, 10);
		}
		e->next_voice = (uint8_t)((idx + 1u) % 6u);
		w(e, C7_LAYER2_SEND_TYPE, hrc7000_dmr_send_type(kind, idx, e->lcss));
	} else {
		uint8_t n;
		uint8_t buf[24];
		memset(buf, 0, sizeof buf);
		switch (kind) {
		case DMR_BURST_VOICE_LC_HEADER:
		case DMR_BURST_TERMINATOR_LC:
			// 9 LC bytes and 3 zero bytes: the chip adds RS(12,9) with the header / terminator mask
			memcpy(buf, b->payload, b->len < 9 ? b->len : 9);
			if (kind == DMR_BURST_VOICE_LC_HEADER) {
				memcpy(e->call_lc, buf, 9);
			}
			n = 12;
			break;
		case DMR_BURST_PI_HEADER:
		case DMR_BURST_CSBK:
		case DMR_BURST_MBC_HEADER:
		case DMR_BURST_DATA_HEADER:
			// 10 bytes: the chip appends the CRC-16 with its mask; a host CRC can't be kept (gotchas 5.2)
			memcpy(buf, b->payload, b->len < 10 ? b->len : 10);
			n = 10;
			break;
		default:
			n = hrc7000_dmr_payload_len(kind);
			memcpy(buf, b->payload, b->len < n ? b->len : n);
			break;
		}
		write_tx_ram(e, buf, n);
		w(e, C7_LAYER2_SEND_TYPE, hrc7000_dmr_send_type(kind, 0, false));
		if (kind == DMR_BURST_VOICE_LC_HEADER || kind == DMR_BURST_PI_HEADER) {
			e->last_hdr = *b;
		}
	}
	w(e, C7_LAYER2_TXRX_CTRL, TXRX_TX);
	e->ops->pa_arm(e->ops->ctx);	// every arming starts the PA one-shot (hardware-dm32.md 8.6)
	e->last_kind = kind;
	e->tx_last_arm = e->tick;
	e->tx_any = true;
	e->st.tx_bursts++;
}

// A slot we don't send in while transmitting. A call there needs the voice-access flag as in rx_tick: with 0x40 alone
// the chip reports voice bursts B-F without a burst number (modem-hrc7000.md 5.3)
static uint32_t listen_ctl(const hrc7000_dmr_t *e)
{
	if (!e->tx_listen) {
		return TXRX_NONE;
	}
	return (e->listen_voice || e->voice_chip_slot >= 0) ? TXRX_RX_VOICE : TXRX_RX;
}

// In a TX state that ticks on the TX vector (not the TC lock wait, which still receives)
static bool tx_ticking(const hrc7000_dmr_t *e)
{
	return e->tx_state != TXS_IDLE && e->tx_state != TXS_LOCK;
}

// A repeater's slot numbering (modem-hrc7000.md 5.3): the CACH TC of BS-sourced bursts, locked once it alternated
// TC_LOCK times in a row; the estimate then runs on with the slot ticks. The first bursts after the receiver syncs
// to a repeater can carry a wrong TC (LAYER2_STATUS b31 still set): a TX slot picked from one of them was the wrong
// one about half the time (issue 72, DM-32UV on the SDR bench, 2026-10-18)
static void track_tc(hrc7000_dmr_t *e, const hrc7000_rx_status_t *s)
{
	e->rx_bs = s->sync == DMR_SYNC_BS;
	if (!e->rx_bs) {
		e->tc_agree = 0;
		return;
	}
	// A burst's deframe comes near the end of its slot, around the next timeslot interrupt: in the first half of a slot
	// it is the previous slot's burst. Counting it to the slot of the last interrupt either way set the estimate one
	// slot off in some runs and not others (DM-32UV on the SDR bench, 2026-10-18)
	uint32_t us = now(e) - e->t_tick;
	bool early = us < SLOT_US / 2u;
	uint32_t slot = e->tick - (early ? 1u : 0u);
	e->st.rx_deframe_ms = (uint8_t)(us / 1000u > 255u ? 255u : us / 1000u);
	bool next = e->tc_agree > 0 && s->tc != e->tc_prev && slot - e->tc_tick == 1u;
	e->tc_agree = next ? (uint8_t)(e->tc_agree < 255u ? e->tc_agree + 1u : 255u) : 1u;
	e->tc_prev = s->tc;
	e->tc_tick = slot;
	e->tc_est = early ? (uint8_t)(s->tc ^ 1u) : s->tc;	// the slot started at the last interrupt
}

// The lock holds while the repeater is heard: a burst in the last 390 ms
static bool tc_locked(const hrc7000_dmr_t *e)
{
	return e->tc_agree >= TC_LOCK && e->tick - e->tc_tick <= RX_LOST_TICKS;
}

// Aligned start (2.5): LAYER2_CONTROL stays passive and the TX slot is built from the receive slot. Writing
// LAYER2_CONTROL (0xC3, or 0xC4 for the TX slots to follow) left the receiver without slot sync for about 1 s, deaf to
// an SFR's inbound call (DM-32UV, 2026-10-16)
static void aligned_arm(hrc7000_dmr_t *e)
{
	w(e, C7_LAYER2_SLOTON, SLOTON_RXTX | SLOTON_RX_OPEN_TX);
	w(e, C7_LAYER2_TXRX_CTRL, TXRX_NONE);
	e->tx_state = TXS_ARMING;
}

// The TC lock wait, at each receive tick: arm once locked, give up (TX_REJECTED, nothing sent) after
// TC_LOCK_WAIT_TICKS, as stock does not key on a repeater without the lock either
static void lock_tick(hrc7000_dmr_t *e)
{
	if (e->tx_abort_req) {
		tx_finish(e);
	} else if (tc_locked(e)) {
		e->st.txb_lock_wait = e->tx_wait;
		e->st.txb_tc_agree = e->tc_agree;
		e->st.txb_deframe_ms = e->st.rx_deframe_ms;
		// No LAYER2_CONTROL or SLOTON write: in 0xC4 the TX slots already follow the receiver (stock writes only
		// TXRX at the key-up)
		w(e, C7_LAYER2_TXRX_CTRL, TXRX_NONE);
		e->tx_state = TXS_ARMING;
	} else if (++e->tx_wait > TC_LOCK_WAIT_TICKS) {
		e->st.tx_rejected++;
		e->st.txb_lock_wait = e->tx_wait;
		ev_simple(e, DMR_EV_TX_REJECTED, e->tx_ts, 1u);	// arg 1: no slot timing to align to
		tx_finish(e);
	}
}

static void tx_tick(hrc7000_dmr_t *e)
{
	if (e->tx_abort_req) {
		tx_finish(e);
		return;
	}
	switch (e->tx_state) {
	case TXS_ARMING:
		// Own timing: the first slot is ours. Aligned to an SFR or a direct-mode station (no CACH): the slot whose
		// number (LAYER2_STATUS b31) is ours; the first one after an aligned start is the one the received bursts
		// came in (DM-32UV on the SDR bench, 2026-10-16)
		// Through a repeater: the next slot is ours when the one starting now (the TC estimate) is the other TS
		if (e->tx_timing != DMR_TIMING_ALIGNED) {
			e->tx_our_next = true;
		} else if (e->tx_bs) {
			e->tx_our_next = (e->tc_est ^ 1u) == (uint8_t)(e->tx_ts - 1);
			uint32_t us = e->t_tick - e->t_rx_tick;
			e->st.txa_rx_us = (uint16_t)(us / 100u > 65535u ? 65535u : us / 100u);
			e->st.txa_est = (uint8_t)(e->tc_est | (e->tx_our_next ? 2u : 0u));
		} else {
			uint32_t b31 = (r(e, C7_LAYER2_STATUS) >> 31) & 1u;
			e->tx_our_next = b31 == e->tx_chip_slot;
			uint32_t us = e->t_tick - e->t_rx_tick;
			e->st.txa_rx_us = (uint16_t)(us / 100u > 65535u ? 65535u : us / 100u);
			e->st.txa_est = (uint8_t)(b31 | (e->tx_our_next ? 2u : 0u));
		}
		e->tx_state = TXS_RUN;
		// fall through
	case TXS_RUN:
		if (!e->tx_our_next) {
			w(e, C7_LAYER2_TXRX_CTRL, listen_ctl(e));
			e->tx_our_next = true;
			break;
		}
		if (txq_count(e) > 0) {
			dmr_tx_burst_t b = e->txq[e->txq_tail];
			e->txq_tail = (uint8_t)((e->txq_tail + 1u) % HRC7000_DMR_TXQ_DEPTH);
			if (b.kind == DMR_BURST_EMPTY) {
				// Our slot left empty (an SFR's gap in the inbound call, gotchas 14.4): nothing armed, so no RF TX
				// interrupt; the slot still counts as ours for the caller and is received like the other one
				w(e, C7_LAYER2_TXRX_CTRL, listen_ctl(e));
				e->st.tx_empty++;
				ev_simple(e, DMR_EV_TX_SLOT_STARTED, e->tx_ts, tick_arg(e));
				e->tx_our_next = false;
				break;
			}
			arm(e, &b);
		} else if (e->tx_end_req && e->tx_any) {
			w(e, C7_LAYER2_TXRX_CTRL, TXRX_NONE);
			e->tx_state = TXS_DRAIN;
			break;
		} else if (e->last_kind == DMR_BURST_VOICE) {
			dmr_tx_burst_t s = { .kind = DMR_BURST_VOICE, .voice_index = e->next_voice, .len = 0 };
			e->st.tx_underrun++;
			arm(e, &s);
		} else if (e->last_kind == DMR_BURST_VOICE_LC_HEADER || e->last_kind == DMR_BURST_PI_HEADER) {
			dmr_tx_burst_t h = e->last_hdr;
			e->st.tx_underrun++;
			arm(e, &h);
		} else if (e->tx_any) {
			// Data or CSBK list ran out: end of the list
			w(e, C7_LAYER2_TXRX_CTRL, TXRX_NONE);
			e->tx_state = TXS_DRAIN;
			break;
		} else {
			w(e, C7_LAYER2_TXRX_CTRL, TXRX_NONE);	// nothing queued yet: keep the turn
			break;
		}
		e->tx_our_next = false;
		break;
	case TXS_DRAIN:
		// The burst armed at tick k is on air in the slot from tick k+1 to k+2
		w(e, C7_LAYER2_TXRX_CTRL, TXRX_NONE);
		if ((uint32_t)(e->tick - e->tx_last_arm) >= 2u) {
			tx_finish(e);
		}
		break;
	default:
		break;
	}
}

// --- Receive ---

static int8_t slot_ts(hrc7000_dmr_t *e, const hrc7000_rx_status_t *s)
{
	switch (s->sync) {
	case DMR_SYNC_BS:
		e->tc_slot[s->chip_slot] = (int8_t)(s->tc + 1);
		e->tc_slot[s->chip_slot ^ 1u] = (int8_t)(2 - s->tc);
		return (int8_t)(s->tc + 1);
	case DMR_SYNC_TS1:
		return 1;
	case DMR_SYNC_TS2:
		return 2;
	default:
		return e->tc_slot[s->chip_slot] != 0 ? e->tc_slot[s->chip_slot] : (int8_t)(s->chip_slot + 1);
	}
}

static void rx_tick(hrc7000_dmr_t *e)
{
	if (e->voice_chip_slot >= 0 && (uint32_t)(e->tick - e->voice_tick) > VOICE_LOST_TICKS) {
		ev_simple(e, DMR_EV_VOICE_LOST, e->tc_slot[e->voice_chip_slot], tick_arg(e));
		e->voice_chip_slot = -1;
	}
	rx_resync(e);
	uint32_t status = r(e, C7_LAYER2_STATUS);
	e->st.last_l2_status = status;
	int8_t next = (int8_t)((((status >> 30) & 1u) ^ HRC7000_RX_NEXT_SLOT_XOR) & 1u);
	w(e, C7_LAYER2_TXRX_CTRL, (e->voice_chip_slot >= 0 && next == e->voice_chip_slot) ? TXRX_RX_VOICE : TXRX_RX);

	// Color code scan when no filter is set: step through the codes while nothing is heard
	if (e->cc_filter < 0 && (uint32_t)(e->tick - e->cc_tick) >= (e->rx_any ? CC_HOLD_TICKS : CC_SCAN_TICKS)) {
		set_cc(e, (uint8_t)(e->cc_cur + 1u));
		w(e, C7_LAYER2_CONTROL, L2_PASSIVE);
		w(e, C7_LAYER2_SLOTON, SLOTON_OFF);
		w(e, C7_LAYER2_TXRX_CTRL, TXRX_SYNC_RELEASE);
		w(e, C7_LAYER2_TXRX_CTRL, TXRX_RX);
		w(e, C7_LAYER2_SLOTON, SLOTON_RXTX);
		e->cc_tick = e->tick;
		e->rx_any = false;
		e->st.cc_scan_steps++;
	}
}

static void rx_burst(hrc7000_dmr_t *e)
{
	uint32_t rti = r(e, C7_RX_TYPE_INFO);
	hrc7000_rx_status_t s;
	e->st.last_rx_type_info = rti;
	if (tx_ticking(e)) {
		e->st.rx_in_tx[(e->tick - e->tx_last_arm) & 1u]++;	// which slot the chip hears in (tx_listen)
	}
	if (tx_ticking(e) && !e->tx_listen) {
		return;	// not receiving while we send, unless asked to (tx_listen)
	}
	hrc7000_dmr_decode(rti, &s);
	if (s.kind == HRC7000_KIND_NONE) {
		e->st.rx_unusable++;
		return;
	}
	int8_t ts = slot_ts(e, &s);
	e->rx_tick = e->tick;
	e->rx_any = true;
	e->cc_tick = e->tick;
	e->st.last_cc = s.cc;
	e->st.last_ts = ts;
	e->rx_chip_slot = s.chip_slot;
	track_tc(e, &s);
	e->st.rx_hist = (e->st.rx_hist << 4) | ((uint32_t)s.tc << 3) | ((uint32_t)s.chip_slot << 2) |
			((r(e, C7_LAYER2_STATUS) >> 30) & 3u);
	if (e->lock_ts != 0 && ts != e->lock_ts) {
		return;
	}
	bool header = s.kind != DMR_BURST_VOICE && s.kind != DMR_BURST_RATE_12_DATA && s.kind != DMR_BURST_RATE_34_DATA &&
		      s.kind != DMR_BURST_RATE_1_DATA;
	if (!s.crc_ok) {
		e->st.rx_crc_err++;
		// Headers and CSBKs with a bad CRC are dropped (dmr-modem.md 2.2), except while we send: the chip's verdict
		// is wrong at times then, and the caller checks them itself or rebuilds them from the call's LC (gotchas 14.1;
		// the DM-32UV's SFR lost every terminator heard while keyed to an RS flag)
		if (header && e->tx_state == TXS_IDLE) {
			return;
		}
	}
	hrc7000_dmr_evslot_t *slot = ev_alloc(e);
	if (slot == NULL) {
		return;
	}
	dmr_burst_t *b = &slot->burst;
	memset(b, 0, sizeof *b);
	b->kind = s.kind;
	b->voice_index = s.voice_index;
	b->sync = s.sync;
	b->cc = s.cc;
	b->ts = ts;
	b->crc_ok = s.crc_ok;
	b->pi = s.pi;
	b->in_step = true;
	b->len = hrc7000_dmr_payload_len(s.kind);
	if (s.kind == DMR_BURST_VOICE) {
		for (uint8_t i = 0; i < DMR_VOICE_BYTES; i++) {
			b->payload[i] = e->dev->buf[C7_VOICE_RAM + i];
		}
		if (e->emb_pending) {
			memcpy(b->lc, e->emb_rx, 9);
			b->lc_valid = true;
			e->emb_pending = false;
		}
		e->voice_chip_slot = (int8_t)s.chip_slot;
		e->voice_tick = e->tick;
		e->st.rx_voice++;
	} else {
		for (uint8_t i = 0; i < b->len; i++) {
			b->payload[i] = e->dev->buf[C7_RX_RAM + i];
		}
		switch (s.kind) {
		case DMR_BURST_VOICE_LC_HEADER:
			memcpy(b->lc, b->payload, 9);
			b->lc_valid = true;
			e->listen_voice = e->tx_state != TXS_IDLE;
			e->st.rx_lc_header++;
			break;
		case DMR_BURST_TERMINATOR_LC:
			memcpy(b->lc, b->payload, 9);
			b->lc_valid = true;
			if (e->voice_chip_slot == (int8_t)s.chip_slot) {
				e->voice_chip_slot = -1;
			}
			e->listen_voice = false;
			e->st.rx_terminator++;
			break;
		case DMR_BURST_CSBK:
		case DMR_BURST_MBC_HEADER:
		case DMR_BURST_MBC_CONT:
			e->st.rx_csbk++;
			break;
		case DMR_BURST_IDLE:
			e->st.rx_idle++;
			break;
		default:
			e->st.rx_data++;
			break;
		}
	}
	slot->ev.kind = DMR_EV_BURST_RX;
	slot->ev.ts = ts;
	slot->ev.arg = tick_arg(e);
	ev_commit(e);
}

// --- ISRs ---

void hrc7000_dmr_isr_sys(hrc7000_dmr_t *e)
{
	e->st.sys_ints++;
	for (uint32_t n = 0; n < SYS_LOOPS; n++) {
		uint32_t list = r(e, C7_SYS_INTERP_LIST);
		if (list == 0) {
			break;
		}
		w(e, C7_SYS_INTERP_CLEAR, list);
		e->st.last_sys_list = list;
		if ((list & C7_SYS_FM_RX) != 0 && e->fm_rx != NULL) {
			e->fm_rx();
		}
		if (!e->running) {
			continue;
		}
		if (list & SYS_LATE_LC) {
			// Embedded LC assembled from a superframe: in the RX RAM (2.8), handed over with the next voice burst
			e->st.late_entry++;
			if ((r(e, C7_RX_TYPE_INFO) & RTI_CS) == 0) {
				for (uint8_t i = 0; i < 9; i++) {
					e->emb_rx[i] = e->dev->buf[C7_RX_RAM + i];
				}
				e->emb_pending = true;
			}
		}
		if (list & SYS_DEFRAME) {
			rx_burst(e);
		}
		if (list & SYS_ABNORMAL) {
			e->st.slot_abnormal++;
		}
	}
}

void hrc7000_dmr_isr_timeslot(hrc7000_dmr_t *e, bool tx_vector)
{
	w(e, C7_INTERP_CLEAR, C7_INT_TS_TX | C7_INT_TS_RX);
	if (tx_vector) {
		e->st.ts_tx_ints++;
	} else {
		e->st.ts_rx_ints++;
	}
	if (!e->running || tx_vector != tx_ticking(e)) {
		return;	// TX states tick on the TX vector, the rest on the RX vector
	}
	uint32_t t = now(e);
	// Slots alternate: the estimate is the TC of the slot now starting. Through a repeater the receive timing keeps
	// running when the TX slots take over (l2_rx), and the first TX-vector interrupt came either a slot after the
	// last receive one or about 5 ms after it, in the same slot: counted as a new slot there, the estimate was one
	// slot off and the call went out on the other slot (DM-32UV on the SDR bench, 2026-10-18)
	bool same_slot = tx_vector && !e->tick_tx && t - e->t_rx_tick < SLOT_US / 2u;
	if (!same_slot) {
		e->tc_est ^= 1u;
	}
	e->tick_tx = tx_vector;
	e->tick++;
	e->st.ticks++;
	e->t_tick = t;
	if (!tx_vector) {
		e->t_rx_tick = t;
	}
	e->timing = true;
	if (e->rf_tx && (uint32_t)(e->t_tick - e->t_rf_tx) >= PA_OFF_US) {
		rf_off(e);
	}
	if (tx_ticking(e)) {
		tx_tick(e);
	} else {
		if (e->tx_state == TXS_LOCK) {
			lock_tick(e);
		}
		if (!tx_ticking(e)) {
			rx_tick(e);	// armed: the TX vector ticks from the next slot
		}
	}
	e->st.tx_state = e->tx_state;
	ev_simple(e, DMR_EV_SLOT_TICK, 0, tick_arg(e));
}

void hrc7000_dmr_isr_rftx(hrc7000_dmr_t *e)
{
	w(e, C7_INTERP_CLEAR, C7_INT_RF_TX);
	e->st.rftx_ints++;
	if (!e->running || e->tx_state == TXS_IDLE || !e->tx_any) {
		return;
	}
	e->ops->rf(e->ops->ctx, true);	// FD6818 idle -> TX, 3.1 ms ahead of the burst
	e->rf_tx = true;
	e->t_rf_tx = now(e);
	ev_simple(e, DMR_EV_TX_SLOT_STARTED, e->tx_ts, tick_arg(e));
}

// --- Task side ---

void hrc7000_dmr_init(hrc7000_dmr_t *e, hrc7000_t *dev, const hrc7000_dmr_ops_t *ops)
{
	memset(e, 0, sizeof *e);
	e->dev = dev;
	e->ops = ops;
	e->lcss = true;
	e->cc_filter = -1;
	e->cfg.cc = -1;
	e->voice_chip_slot = -1;
	e->cc_cur = 1;
}

void hrc7000_dmr_run(hrc7000_dmr_t *e)
{
	lock(e);
	e->tx_state = TXS_IDLE;
	e->tx_listen = false;
	e->txq_tail = e->txq_head;
	e->rx_any = false;
	e->cc_tick = e->tick;
	set_cc(e, e->cc_filter >= 0 ? (uint8_t)e->cc_filter : e->cc_cur);
	chip_rx_restart(e);
	e->running = true;
	unlock(e);
}

void hrc7000_dmr_halt(hrc7000_dmr_t *e)
{
	lock(e);
	bool was_tx = e->tx_state != TXS_IDLE;
	w(e, C7_LAYER2_TXRX_CTRL, TXRX_NONE);
	rf_off(e);
	e->tx_state = TXS_IDLE;
	e->tx_listen = false;
	e->txq_tail = e->txq_head;
	e->running = false;
	if (was_tx) {
		ev_simple(e, DMR_EV_TX_DONE, e->tx_ts, tick_arg(e));
	}
	unlock(e);
}

bool hrc7000_dmr_pop(hrc7000_dmr_t *e, dmr_event_t *ev, dmr_burst_t *burst)
{
	if (e->evq_tail == e->evq_head) {
		return false;
	}
	const hrc7000_dmr_evslot_t *s = &e->evq[e->evq_tail];
	*ev = s->ev;
	if (ev->kind == DMR_EV_BURST_RX) {
		*burst = s->burst;
		ev->burst = burst;
	} else {
		ev->burst = NULL;
	}
	e->evq_tail = (uint8_t)((e->evq_tail + 1u) % HRC7000_DMR_EVQ_DEPTH);
	return true;
}

void hrc7000_dmr_poll(hrc7000_dmr_t *e)
{
	lock(e);
	if (e->running && e->tx_state != TXS_IDLE && (uint32_t)(now(e) - e->t_tick) > STALL_US) {
		// No timeslot interrupt for 200 ms while sending: drop the transmission and restart the receiver
		e->st.stalls++;
		e->t_tick = now(e);
		tx_finish(e);
		ev_simple(e, DMR_EV_SYNC_LOST, 0, tick_arg(e));
	}
	unlock(e);
}

void hrc7000_dmr_configure(hrc7000_dmr_t *e, const dmr_config_t *cfg)
{
	lock(e);
	bool framing = cfg->repeater != e->cfg.repeater;	// LAYER2_CONTROL changes with it (l2_rx)
	e->cfg = *cfg;
	e->cc_filter = cfg->cc;
	if (e->running && cfg->cc >= 0 && (uint8_t)cfg->cc != e->cc_cur) {
		set_cc(e, (uint8_t)cfg->cc);
		chip_rx_restart(e);
	} else if (e->running && framing && e->tx_state == TXS_IDLE) {
		chip_rx_restart(e);
	}
	unlock(e);
}

void hrc7000_dmr_rx_restart(hrc7000_dmr_t *e)
{
	lock(e);
	if (e->running && e->tx_state == TXS_IDLE) {
		chip_rx_restart(e);
	}
	unlock(e);
}

void hrc7000_dmr_rx_lock_slot(hrc7000_dmr_t *e, int8_t ts)
{
	e->lock_ts = ts;
}

bool hrc7000_dmr_tx_begin(hrc7000_dmr_t *e, dmr_timing_t timing, int8_t ts, dmr_access_t access)
{
	lock(e);
	bool ok = e->running && e->tx_state == TXS_IDLE;
	if (ok && access != DMR_ACCESS_IMPOLITE && e->rx_any && (uint32_t)(e->tick - e->rx_tick) < BUSY_TICKS &&
	    (access == DMR_ACCESS_POLITE_ALL || (e->cfg.cc >= 0 && e->st.last_cc == (uint8_t)e->cfg.cc))) {
		e->st.tx_rejected++;
		ev_simple(e, DMR_EV_TX_REJECTED, ts, tick_arg(e));
		ok = false;
	}
	if (ok) {
		e->tx_timing = (uint8_t)timing;
		e->tx_ts = ts;
		e->tx_access = (uint8_t)access;
		e->tx_end_req = false;
		e->tx_abort_req = false;
		e->tx_any = false;
		e->tx_listen = false;
		e->listen_voice = false;
		e->send_sync = DMR_SYNC_NONE;	// written with the first burst (a re-init may have put MS back)
		e->tx_wait = 0;
		e->last_kind = KIND_UNSET;
		e->next_voice = 0;
		e->t_tick = now(e);	// the stall watchdog counts from here
		// 6.1 step 1: codec buffers cleared, nothing in the next slot
		w(e, C7_AUDIO_BUFFER_CLR, 0x03);
		e->tx_bs = timing == DMR_TIMING_ALIGNED && e->rx_bs && ts > 0;
		if (e->tx_bs) {
			e->tx_state = TXS_LOCK;	// lock_tick arms with the lock
		} else if (timing == DMR_TIMING_ALIGNED && e->rx_any) {
			// Our chip slot (no CACH): the received one for the same TS, else the other
			e->tx_chip_slot = ts == e->st.last_ts ? e->rx_chip_slot : (uint8_t)(e->rx_chip_slot ^ 1u);
			aligned_arm(e);
		} else {
			e->tx_timing = (uint8_t)DMR_TIMING_OWN;
			w(e, C7_LAYER2_CONTROL, L2_ACTIVE);	// active timing: we own the slot clock
			w(e, C7_LAYER2_TXRX_CTRL, TXRX_NONE);
			e->tx_state = TXS_ARMING;
		}
		uint32_t age = e->tick - e->rx_tick;
		e->st.txb_tc_agree = e->tc_agree;
		e->st.txb_lock_wait = 0;
		e->st.txb_timing = e->tx_timing;
		e->st.txb_chip_slot = e->tx_chip_slot;
		e->st.txb_rx_chip_slot = e->rx_chip_slot;
		e->st.txb_rx_age = (uint8_t)(e->rx_any ? (age > 254u ? 254u : age) : 255u);
		e->st.txb_ts = ts;
		e->st.txb_last_ts = e->st.last_ts;
		e->st.txb_rx_type_info = e->st.last_rx_type_info;
		e->st.txb_rx_hist = e->st.rx_hist;
	}
	unlock(e);
	return ok;
}

void hrc7000_dmr_tx_listen(hrc7000_dmr_t *e)
{
	lock(e);
	if (e->tx_state != TXS_IDLE) {
		e->tx_listen = true;
		if (e->tx_timing != DMR_TIMING_ALIGNED) {
			w(e, C7_LAYER2_CONTROL, L2_ACTIVE_RX_FOLLOW);	// aligned: the receiver already keeps its passive timing
		}
	}
	unlock(e);
}

bool hrc7000_dmr_tx_queue(hrc7000_dmr_t *e, const dmr_tx_burst_t *b)
{
	lock(e);
	bool ok = txq_count(e) < HRC7000_DMR_TXQ_DEPTH - 1u;
	if (ok) {
		e->txq[e->txq_head] = *b;
		e->txq_head = (uint8_t)((e->txq_head + 1u) % HRC7000_DMR_TXQ_DEPTH);
	}
	unlock(e);
	return ok;
}

int8_t hrc7000_dmr_tx_fill(hrc7000_dmr_t *e, const dmr_tx_burst_t *b)
{
	int8_t after = -1;
	lock(e);
	uint8_t n = txq_count(e);
	for (uint8_t i = 0; i < n; i++) {
		dmr_tx_burst_t *q = &e->txq[(e->txq_tail + i) % HRC7000_DMR_TXQ_DEPTH];
		if (q->kind == DMR_BURST_EMPTY) {
			*q = *b;
			after = (int8_t)(n - 1u - i);
			break;
		}
	}
	unlock(e);
	return after;
}

uint8_t hrc7000_dmr_tx_queue_space(const hrc7000_dmr_t *e)
{
	return (uint8_t)(HRC7000_DMR_TXQ_DEPTH - 1u - txq_count(e));
}

void hrc7000_dmr_tx_end(hrc7000_dmr_t *e)
{
	lock(e);
	e->tx_end_req = true;
	if (e->tx_state != TXS_IDLE && !e->tx_any && txq_count(e) == 0) {
		tx_finish(e);	// nothing queued and nothing sent
	}
	unlock(e);
}

void hrc7000_dmr_tx_abort(hrc7000_dmr_t *e)
{
	lock(e);
	if (e->tx_state != TXS_IDLE) {
		e->tx_abort_req = true;
	}
	unlock(e);
}

void hrc7000_dmr_set_embedded_lc(hrc7000_dmr_t *e, const uint8_t lc[9])
{
	lock(e);
	memcpy(e->emb_lc, lc, 9);
	e->emb_set = true;
	unlock(e);
}

void hrc7000_dmr_set_cc_filter(hrc7000_dmr_t *e, int8_t cc)
{
	lock(e);
	e->cc_filter = cc;
	e->cfg.cc = cc;
	if (e->running && cc >= 0 && (uint8_t)cc != e->cc_cur && e->tx_state == TXS_IDLE) {
		set_cc(e, (uint8_t)cc);
		chip_rx_restart(e);
	}
	unlock(e);
}

bool hrc7000_dmr_is_transmitting(const hrc7000_dmr_t *e)
{
	return e->tx_state != TXS_IDLE;
}
