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
#define TXRX_NONE		0x00u
#define TXRX_RX			0x40u
#define TXRX_TX			0x80u
#define TXRX_RX_VOICE		0x8040u
#define TXRX_SYNC_RELEASE	0x800000u
#define SLOTON_OFF		0x00u
#define SLOTON_RXTX		0x05u

// The chip slot the RX timeslot interrupt decides is the one after the slot number LAYER2_STATUS b30 shows then
// (assumed: the interrupt comes 1.6 ms ahead of the boundary, LAYER2_SLOT_PRE). Flip to 0 if the bring-up shows the
// voice-access flag landing on the other slot
#ifndef HRC7000_RX_NEXT_SLOT_XOR
#define HRC7000_RX_NEXT_SLOT_XOR	1u
#endif

#define VOICE_LOST_TICKS	13u	// about 390 ms without a voice burst on the call's slot
#define CC_SCAN_TICKS		4u	// color code scan: next code after 120 ms of nothing
#define CC_HOLD_TICKS		167u	// ... held 5 s after the last burst
#define BUSY_TICKS		12u	// polite access: activity in the last 360 ms
#define RX_LOST_TICKS		13u	// a reception is over (390 ms without a burst taken)
#define RX_IDLE_TICKS		20u	// slot timing but nothing taken for 600 ms: the timing is stale
#define PA_OFF_US		20000u	// the PA goes off at the first tick at least this long after the RF TX interrupt
#define STALL_US		200000u
#define SYS_LOOPS		4u

enum { TXS_IDLE, TXS_ARMING, TXS_RUN, TXS_DRAIN };
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
static void chip_rx_restart(hrc7000_dmr_t *e)
{
	w(e, C7_WORK_MODE, C7_WORK_MODE_FM);
	w(e, C7_WORK_MODE, C7_WORK_MODE_DMR);
	w(e, C7_VOICE_EMB_CTRL, 0x02);
	w(e, C7_LAYER2_CONTROL, L2_PASSIVE);
	w(e, C7_LAYER2_SLOTON, SLOTON_OFF);
	w(e, C7_LAYER2_TXRX_CTRL, TXRX_SYNC_RELEASE);
	w(e, C7_LAYER2_TXRX_CTRL, TXRX_RX);
	w(e, C7_LAYER2_SLOTON, SLOTON_RXTX);
	e->voice_chip_slot = -1;
	e->tc_slot[0] = e->tc_slot[1] = 0;
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
	e->st.tx_done++;
	if (e->cfg.repeater) {
		// Repeater: passive timing now, receive from the next tick on (keeps the repeater's slot timing)
		w(e, C7_LAYER2_CONTROL, L2_PASSIVE);
		e->rx_restart_ticks = 1;
	} else {
		chip_rx_restart(e);
	}
	ev_simple(e, DMR_EV_TX_DONE, e->tx_ts, tick_arg(e));
}

// --- Transmit ---

static void write_tx_ram(hrc7000_dmr_t *e, const uint8_t *p, uint8_t n)
{
	for (uint8_t i = 0; i < n; i++) {
		e->dev->buf[C7_TX_RAM + i] = p[i];
	}
}

static void arm(hrc7000_dmr_t *e, const dmr_tx_burst_t *b)
{
	uint8_t kind = b->kind;
	if (b->sync_override != DMR_SYNC_NONE) {
		e->st.tx_unsupported++;	// the send sync is fixed at init (MS data); sent with it
	}
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

static void tx_tick(hrc7000_dmr_t *e)
{
	if (e->tx_abort_req) {
		tx_finish(e);
		return;
	}
	switch (e->tx_state) {
	case TXS_ARMING:
		e->tx_our_next = true;
		e->tx_state = TXS_RUN;
		// fall through
	case TXS_RUN:
		if (!e->tx_our_next) {
			w(e, C7_LAYER2_TXRX_CTRL, TXRX_NONE);
			e->tx_our_next = true;
			break;
		}
		if (txq_count(e) > 0) {
			dmr_tx_burst_t b = e->txq[e->txq_tail];
			e->txq_tail = (uint8_t)((e->txq_tail + 1u) % HRC7000_DMR_TXQ_DEPTH);
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
	if (e->rx_restart_ticks > 0) {
		e->rx_restart_ticks = 0;
		w(e, C7_LAYER2_TXRX_CTRL, TXRX_RX);	// 6.3 step 3
		return;
	}
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
	if (e->tx_state != TXS_IDLE) {
		return;	// not receiving while we send
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
	if (e->lock_ts != 0 && ts != e->lock_ts) {
		return;
	}
	bool header = s.kind != DMR_BURST_VOICE && s.kind != DMR_BURST_RATE_12_DATA && s.kind != DMR_BURST_RATE_34_DATA &&
		      s.kind != DMR_BURST_RATE_1_DATA;
	if (!s.crc_ok) {
		e->st.rx_crc_err++;
		if (header) {
			return;	// headers and CSBKs with a bad CRC are dropped (dmr-modem.md 2.2)
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
			e->st.rx_lc_header++;
			break;
		case DMR_BURST_TERMINATOR_LC:
			memcpy(b->lc, b->payload, 9);
			b->lc_valid = true;
			if (e->voice_chip_slot == (int8_t)s.chip_slot) {
				e->voice_chip_slot = -1;
			}
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
	if (!e->running || tx_vector != (e->tx_state != TXS_IDLE)) {
		return;	// TX states tick on the TX vector, the rest on the RX vector
	}
	e->tick++;
	e->st.ticks++;
	e->t_tick = now(e);
	e->timing = true;
	if (e->rf_tx && (uint32_t)(e->t_tick - e->t_rf_tx) >= PA_OFF_US) {
		rf_off(e);
	}
	if (e->tx_state != TXS_IDLE) {
		tx_tick(e);
	} else {
		rx_tick(e);
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
	e->txq_tail = e->txq_head;
	e->rx_any = false;
	e->cc_tick = e->tick;
	e->rx_restart_ticks = 0;
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
	e->cfg = *cfg;
	e->cc_filter = cfg->cc;
	if (e->running && cfg->cc >= 0 && (uint8_t)cfg->cc != e->cc_cur) {
		set_cc(e, (uint8_t)cfg->cc);
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
		e->tx_wait = 0;
		e->last_kind = KIND_UNSET;
		e->next_voice = 0;
		e->rx_restart_ticks = 0;
		e->t_tick = now(e);	// the stall watchdog counts from here
		// 6.1 step 1: codec buffers cleared, active timing (we own the slot clock), nothing in the next slot
		w(e, C7_AUDIO_BUFFER_CLR, 0x03);
		w(e, C7_LAYER2_CONTROL, L2_ACTIVE);
		w(e, C7_LAYER2_TXRX_CTRL, TXRX_NONE);
		e->tx_state = TXS_ARMING;
	}
	unlock(e);
	return ok;
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
