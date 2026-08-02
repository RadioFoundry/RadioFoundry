// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// HR_C6000 DMR: register sequences, burst status decode and the slot engine (see hrc6000_dmr.h for the model).
// Register facts: docs/spec/modem-hrc6000.md 2-7. Behavior rules: docs/spec/dmr-modem.md 6, docs/spec/gotchas.md 1, 3, 5.
#include <stddef.h>
#include <string.h>
#include "hrc6000_dmr.h"
#include "config.h"

#if CONFIG_DMR	// the whole file: DMR only

// Page 4 registers
#define R_MODE		0x10
#define R_CC		0x1F
#define R_ACCESS0	0x20
#define R_ACCESS1	0x21
#define R_CODEC_CTL	0x22
#define R_L2_CTL	0x40
#define R_SLOT_CTL	0x41
#define R_SLOT_STAT	0x42
#define R_TX_TYPE	0x50
#define R_RX_TYPE	0x51
#define R_RX_CC		0x52
#define R_SYNC_TYPE	0x5F
#define R_INT_FLAGS	0x82
#define R_INT_CLEAR	0x83
#define R_VOICE_ABN	0x98

// 0x40
#define L2_PASSIVE	0xC3	// TX + RX enabled, timing follows received bursts
#define L2_ACTIVE	0xE3	// TX + RX enabled, own timing
#define L2_MCU_CRC	0x08	// CRC_MCU_Control

// 0x41
#define CTL_TX		0x80	// send in the next slot
#define CTL_RX		0x50	// receive in the next slot
#define CTL_NONE	0x00
#define CTL_SYNC_FAIL	0x20	// drop sync, search again
#define CTL_CC_ANY	0x08	// CC_Match_Ctrl: deframe whatever the color code (effect unconfirmed, 9)

// With the color code filter off, also set CC_Match_Ctrl in every receive write. Off: follow the color code the way
// that has worked on air (5.4: the CC seen at system interrupts, written to 0x1F after repeated matches)
#ifndef HRC6000_DMR_CC_ANY_DEFRAME
#define HRC6000_DMR_CC_ANY_DEFRAME	0
#endif

// 0x82
#define INT_REJECTED	0x80
#define INT_LATE_ENTRY	0x10
#define INT_RX_DATA	0x08
#define INT_VOICE_ABN	0x02

// 0x42 bit 7: the slot now starting is a transmit slot
#define STAT_TX_SLOT	0x80

// The CACH TC read at the timeslot interrupt: 1 = it gives the number of the slot now starting, 0 = of the slot that
// just ended. (unverified: flip it if bursts heard through a repeater report the wrong ts on the bench)
#ifndef HRC6000_DMR_TC_IS_SLOT_NOW
#define HRC6000_DMR_TC_IS_SLOT_NOW	1
#endif

// Timing rules (modem-hrc6000.md 4.2, 4.3, 5.3, 5.5; gotchas 1.2, 1.5)
#define IN_STEP_MIN_US		1000u
#define IN_STEP_MAX_US		12000u
#define OUT_OF_STEP_US		22000u
#define OUT_OF_STEP_RESYNC	2	// consecutive out-of-step reports before the receiver is restarted
#define VOICE_DEDUP_US		45000u	// one slot locked
#define VOICE_DEDUP_BOTH_US	28000u	// both slots received
#define PA_MIN_ON_US		20000u
#define PA_FORCE_OFF_US		40000u	// poll fallback when no timeslot interrupt came to drop the PA
#define STALL_US		200000u
#define RX_LOST_TICKS		13u	// a reception is over (390 ms)
#define RX_IDLE_TICKS		20u	// nothing taken while idle: restart the receiver
#define CC_MATCH_US		200000u
#define CC_SET_MATCHES		2u
#define CC_HOLD_MATCHES		8u
#define CC_RELEASE_TICKS	167u	// 5 s
#define TC_STABLE		4u	// more than this many agreements
#define TC_RESYNC		3u	// more than this many disagreements
#define BS_RECENT_TICKS		40u
#define ALIGN_TIMEOUT_TICKS	50u	// 1.5 s for a slot to align to
#define DONE_FALLBACK_TICKS	4u
#define STALE_LC_VOICE		6u	// voice bursts after which the embedded LC is trusted even if unchanged

enum { TXS_IDLE, TXS_START, TXS_RUN, TXS_ENDING };

#define KIND_UNSET	0xFEu
#define EV_N		HRC6000_DMR_EVQ_DEPTH
#define TXQ_N		HRC6000_DMR_TXQ_DEPTH

static const uint8_t sync_patterns[4][6] = {
	{ 0xD5, 0xD7, 0xF7, 0x7F, 0xD7, 0x57 },	// MS data
	{ 0xDF, 0xF5, 0x7D, 0x75, 0xDF, 0x5D },	// BS data
	{ 0xF7, 0xFD, 0xD5, 0xDD, 0xFD, 0x55 },	// direct mode TS1 data
	{ 0xD7, 0x55, 0x7F, 0x5F, 0xF7, 0xF5 },	// direct mode TS2 data
};

// AMBE+2 silence (modem-hrc6000.md 5.7), three frames
static const uint8_t ambe_silence[DMR_VOICE_BYTES] = {
	0xB9, 0xE8, 0x81, 0x52, 0x61, 0x73, 0x00, 0x2A, 0x6B,
	0xB9, 0xE8, 0x81, 0x52, 0x61, 0x73, 0x00, 0x2A, 0x6B,
	0xB9, 0xE8, 0x81, 0x52, 0x61, 0x73, 0x00, 0x2A, 0x6B,
};

// --- Chip-level helpers ---

static bool w(hrc6000_t *dev, uint8_t reg, uint8_t val)
{
	return hrc6000_write(dev, HRC6000_PAGE_SYS, reg, val);
}

static uint8_t r(hrc6000_t *dev, uint8_t reg)
{
	uint8_t v = 0;
	(void)hrc6000_read(dev, HRC6000_PAGE_SYS, reg, &v);
	return v;
}

bool hrc6000_dmr_enter(hrc6000_t *dev, uint8_t mod2, uint8_t mod1, uint8_t mic_gain)
{
	bool ok = w(dev, 0x60, 0x00);			// analog voice TX off (FM leftovers)
	ok &= w(dev, 0x01, 0xF0);			// two-point modulation TX, IF RX
	ok &= hrc6000_set_mod_gain(dev, mod2, mod1);	// DMR pair
	ok &= w(dev, R_MODE, 0x6E);			// DMR, Tier II, layer 2 (operation value, not the init 0x6B)
	ok &= w(dev, 0xE0, 0xC9);			// codec under MCU control, mic on
	ok &= hrc6000_set_mic_gain(dev, mic_gain);
	ok &= w(dev, 0x26, 0xFD);
	ok &= w(dev, 0x81, 0x19);			// system interrupt mask: post access, received data, PHY
	ok &= w(dev, R_INT_CLEAR, 0xFF);
	return ok;
}

bool hrc6000_dmr_chip_rx_restart(hrc6000_t *dev, uint8_t rx_ctl)
{
	bool ok = w(dev, R_L2_CTL, L2_PASSIVE);
	ok &= w(dev, R_SLOT_CTL, CTL_SYNC_FAIL);
	ok &= w(dev, R_SLOT_CTL, CTL_NONE);
	ok &= w(dev, R_SLOT_CTL, CTL_SYNC_FAIL);
	ok &= w(dev, R_SLOT_CTL, rx_ctl != 0 ? rx_ctl : CTL_RX);
	return ok;
}

bool hrc6000_dmr_set_cc(hrc6000_t *dev, uint8_t cc)
{
	return w(dev, R_CC, (uint8_t)((cc & 0x0Fu) << 4));
}

bool hrc6000_dmr_set_sync(hrc6000_t *dev, uint8_t family)
{
	uint8_t i = family == DMR_SYNC_BS ? 1 : family == DMR_SYNC_TS1 ? 2 : family == DMR_SYNC_TS2 ? 3 : 0;
	return hrc6000_write_burst(dev, HRC6000_PAGE_AUX, 0x04, sync_patterns[i], 6);
}

bool hrc6000_dmr_voice(hrc6000_t *dev, bool write, uint8_t *buf)
{
	if (dev->bus->voice != NULL) {
		return dev->bus->voice(dev->bus->ctx, write, buf, DMR_VOICE_BYTES);
	}
	return write ? hrc6000_write_burst(dev, HRC6000_PAGE_VOCODER, 0x00, buf, DMR_VOICE_BYTES)
		     : hrc6000_read_burst(dev, HRC6000_PAGE_VOCODER, 0x00, buf, DMR_VOICE_BYTES);
}

uint8_t hrc6000_dmr_access_reg(dmr_access_t access)
{
	switch (access) {
	case DMR_ACCESS_POLITE_CC: return 0xA2;		// 10: polite to own CC
	case DMR_ACCESS_POLITE_ALL: return 0x52;	// 01: polite to all
	default: return 0x02;				// impolite
	}
}

// --- Pure decode ---

void hrc6000_dmr_decode(uint8_t r51, uint8_t r52, uint8_t r5f, hrc6000_rx_status_t *st)
{
	static const uint8_t families[4] = { DMR_SYNC_MS, DMR_SYNC_BS, DMR_SYNC_TS1, DMR_SYNC_TS2 };
	uint8_t nib = (uint8_t)(r51 >> 4);
	st->sync_class = r51 & 3u;
	st->crc_ok = (r51 & 0x04u) == 0;
	st->pi = (r51 & 0x08u) != 0;
	st->cc = (uint8_t)(r52 >> 4);
	st->tc = (uint8_t)((r52 >> 2) & 1u);
	st->sync = families[r5f & 3u];
	st->kind = HRC6000_KIND_NONE;
	st->voice_index = 0;
	switch (st->sync_class) {
	case 2:		// data sync: the ETSI data type (0x9 is idle here, not voice A)
		if (nib <= 0x0Bu) {
			st->kind = nib;
		}
		break;
	case 0:		// no sync: voice B-F (and the EMB report)
	case 1:		// voice sync: voice A
		// Voice numbers come with bit 3 set (0x9-0xE): the burst is the low 3 bits, 1-6 = A-F (gotchas 1.3)
		if ((nib & 0x08u) != 0 && (nib & 7u) >= 1u && (nib & 7u) <= 6u) {
			st->kind = DMR_BURST_VOICE;
			st->voice_index = (uint8_t)((nib & 7u) - 1u);
		}
		break;
	default:	// reverse channel
		break;
	}
}

uint8_t hrc6000_dmr_payload_len(uint8_t kind)
{
	switch (kind) {
	case DMR_BURST_VOICE: return DMR_VOICE_BYTES;
	case DMR_BURST_RATE_34_DATA: return 18;
	case DMR_BURST_RATE_1_DATA: return 24;
	default: return 12;
	}
}

uint8_t hrc6000_dmr_reg50(uint8_t kind, uint8_t voice_index)
{
	if (kind == DMR_BURST_VOICE) {
		return (uint8_t)(0x08u | ((voice_index % 6u) << 4));
	}
	return (uint8_t)((kind & 0x0Fu) << 4);
}

// --- Engine: small helpers ---

static uint32_t now(hrc6000_dmr_t *e)
{
	return e->ops->now_us(e->ops->ctx);
}

static void lock(hrc6000_dmr_t *e)
{
	if (e->ops->lock != NULL) {
		e->ops->lock(e->ops->ctx);
	}
}

static void unlock(hrc6000_dmr_t *e)
{
	bool n = e->notify_req;	// pushes made under the lock wake the task after it (no RTOS call inside it)
	e->notify_req = false;
	if (e->ops->unlock != NULL) {
		e->ops->unlock(e->ops->ctx);
	}
	if (n && e->ops->notify != NULL) {
		e->ops->notify(e->ops->ctx);
	}
}

static void isr_done(hrc6000_dmr_t *e)
{
	if (e->notify_req) {
		e->notify_req = false;
		if (e->ops->notify != NULL) {
			e->ops->notify(e->ops->ctx);
		}
	}
}

// Event ring: producers are the ISRs and locked task code (never concurrent), the consumer the board's task
static hrc6000_dmr_evslot_t *ev_begin(hrc6000_dmr_t *e)
{
	uint8_t next = (uint8_t)((e->evq_head + 1u) % EV_N);
	if (next == e->evq_tail) {
		e->st.ev_overflow++;
		return NULL;
	}
	return &e->evq[e->evq_head];
}

static void ev_commit(hrc6000_dmr_t *e)
{
	__atomic_thread_fence(__ATOMIC_RELEASE);
	e->evq_head = (uint8_t)((e->evq_head + 1u) % EV_N);
	e->notify_req = true;
}

static void push(hrc6000_dmr_t *e, uint8_t kind, int8_t ts, uint32_t arg)
{
	hrc6000_dmr_evslot_t *s = ev_begin(e);
	if (s != NULL) {
		s->ev.kind = kind;
		s->ev.ts = ts;
		s->ev.arg = arg;
		s->ev.burst = NULL;
		ev_commit(e);
	}
}

static uint8_t txq_count(const hrc6000_dmr_t *e)
{
	return (uint8_t)((e->txq_head + TXQ_N - e->txq_tail) % TXQ_N);
}

static void txq_flush(hrc6000_dmr_t *e)
{
	e->txq_tail = e->txq_head;
}

static bool cc_any(const hrc6000_dmr_t *e)
{
	return e->cc_filter < 0;
}

static uint8_t rx_ctl(const hrc6000_dmr_t *e)
{
	return (uint8_t)(CTL_RX | (HRC6000_DMR_CC_ANY_DEFRAME && cc_any(e) ? CTL_CC_ANY : 0u));
}

static bool rmo(const hrc6000_dmr_t *e)
{
	return e->cfg.repeater || (e->rmo_seen && (uint32_t)(e->tick - e->bs_tick) < BS_RECENT_TICKS);
}

static void set_ctl(hrc6000_dmr_t *e, uint8_t ctl)
{
	w(e->dev, R_SLOT_CTL, ctl);
	e->ctl_next = ctl;
}

static void forget_slots(hrc6000_dmr_t *e)
{
	e->phase_ts[0] = 0;
	e->phase_ts[1] = 0;
	e->own_ts = 0;
	e->tc_agree = 0;
	e->tc_disagree = 0;
}

static void do_restart(hrc6000_dmr_t *e)
{
	hrc6000_dmr_chip_rx_restart(e->dev, rx_ctl(e));
	e->ctl_next = rx_ctl(e);
	forget_slots(e);
	e->rx_any = false;
	e->rx_tick = e->tick;
	e->report_valid = false;
	e->voice_valid = false;
	e->out_of_step_run = 0;
	e->restart_pending = false;
	e->st.rx_restarts++;
}

static void pa_drop(hrc6000_dmr_t *e, uint32_t t)
{
	if (!e->ops->rf(e->ops->ctx, false)) {
		e->st.rf_fail++;
	}
	e->pa_on = false;
	uint32_t ms = (t - e->t_pa_on) / 1000u;
	e->st.last_pa_ms = (uint8_t)(ms > 255u ? 255u : ms);
	push(e, DMR_EV_TX_SLOT_ENDED, e->phase_ts[e->tx_phase], e->tick);
}

// --- Engine: transmit ---

static void write_page2(hrc6000_dmr_t *e, uint8_t addr, const uint8_t *data, uint8_t len, uint8_t size)
{
	uint8_t buf[24];
	if (size > sizeof(buf)) {
		size = sizeof(buf);
	}
	memset(buf, 0, size);
	memcpy(buf, data, len < size ? len : size);
	hrc6000_write_burst(e->dev, HRC6000_PAGE_DATA, addr, buf, size);
}

static void setup_burst(hrc6000_dmr_t *e, const dmr_tx_burst_t *b)
{
	uint8_t kind = b->kind;
	bool voice = kind == DMR_BURST_VOICE;
	uint8_t idx = (uint8_t)(b->voice_index % 6u);

	uint8_t family = b->sync_override != DMR_SYNC_NONE ? b->sync_override : DMR_SYNC_MS;
	if (family != e->sync_cur) {
		hrc6000_dmr_set_sync(e->dev, family);
		e->sync_cur = family;
	}

	// CRC_MCU_Control only for bursts that carry their own CRC; voice and idle never (gotchas 5.1)
	bool mcu_crc = !voice && kind != DMR_BURST_IDLE && b->host_crc;
	uint8_t r40 = (uint8_t)((e->reg40 & (uint8_t)~L2_MCU_CRC) | (mcu_crc ? L2_MCU_CRC : 0u));
	if (r40 != e->reg40) {
		w(e->dev, R_L2_CTL, r40);
		e->reg40 = r40;
	}

	if (voice) {
		if (idx == 0 && e->emb_pending) {	// the embedded LC for B-E of this superframe
			write_page2(e, 0x00, e->emb_lc, 9, 12);
			e->emb_pending = false;
		}
		uint8_t frame[DMR_VOICE_BYTES];
		memcpy(frame, b->len >= DMR_VOICE_BYTES ? b->payload : ambe_silence, DMR_VOICE_BYTES);
		hrc6000_dmr_voice(e->dev, true, frame);
	} else if (kind == DMR_BURST_IDLE) {
		write_page2(e, 0x18, b->payload, b->len, 12);	// its own place, keeps the LC at 0x00
	} else {
		uint8_t size = hrc6000_dmr_payload_len(kind);
		uint8_t len = b->len;
		if (!b->host_crc && (kind == DMR_BURST_VOICE_LC_HEADER || kind == DMR_BURST_TERMINATOR_LC) && len > 9) {
			len = 9;	// the chip computes the RS(12,9) parity
		}
		write_page2(e, 0x00, b->payload, len, size);
		if (kind == DMR_BURST_TERMINATOR_LC) {
			uint8_t frame[DMR_VOICE_BYTES];
			memcpy(frame, ambe_silence, sizeof(frame));
			hrc6000_dmr_voice(e->dev, true, frame);
		}
	}
	w(e->dev, R_SLOT_CTL, CTL_TX);
	w(e->dev, R_TX_TYPE, hrc6000_dmr_reg50(kind, idx));
	e->ctl_next = CTL_TX;

	e->last_kind = kind;
	if (voice) {
		e->last_voice = idx;
	} else if (kind == DMR_BURST_VOICE_LC_HEADER) {
		e->last_hdr = *b;
	}
	e->tx_last_setup = e->tick;
	e->tx_any_setup = true;
	e->st.tx_bursts++;
}

// Back to receive after a transmission (modem-hrc6000.md 6.3 steps 10-11, 6.1 restore of 0x40)
static void tx_finish(hrc6000_dmr_t *e)
{
	if (e->pa_on) {
		pa_drop(e, now(e));
	}
	if (e->sync_cur != DMR_SYNC_MS) {
		hrc6000_dmr_set_sync(e->dev, DMR_SYNC_MS);
		e->sync_cur = DMR_SYNC_MS;
	}
	bool keep_timing = e->tx_timing != DMR_TIMING_OWN && rmo(e) && e->tx_any_setup;
	e->reg40 = L2_PASSIVE;
	if (keep_timing) {
		w(e->dev, R_L2_CTL, L2_PASSIVE);	// passive timing again, CRC_MCU_Control clear
		set_ctl(e, rx_ctl(e));
		e->rx_tick = e->tick;
	} else {
		do_restart(e);				// direct mode: follow whoever talks next
	}
	txq_flush(e);
	e->tx_state = TXS_IDLE;
	e->tx_end_req = false;
	e->tx_abort_req = false;
	e->st.tx_done++;
	push(e, DMR_EV_TX_DONE, e->tx_ts, e->tick);
}

static void tx_slot(hrc6000_dmr_t *e)
{
	if (e->tx_abort_req) {
		e->tx_state = TXS_ENDING;
		set_ctl(e, CTL_NONE);
		return;
	}
	if (txq_count(e) > 0) {
		dmr_tx_burst_t *b = &e->txq[e->txq_tail];
		setup_burst(e, b);
		__atomic_thread_fence(__ATOMIC_RELEASE);
		e->txq_tail = (uint8_t)((e->txq_tail + 1u) % TXQ_N);
		return;
	}
	if (e->tx_end_req) {
		e->tx_state = TXS_ENDING;
		set_ctl(e, CTL_NONE);
		return;
	}
	// Underrun: keep a voice call going (dmr-modem.md 6.1 point 2), repeat a header, else leave the slot empty
	if (e->last_kind == DMR_BURST_VOICE) {
		e->st.tx_underrun++;
		dmr_tx_burst_t s = { .kind = DMR_BURST_VOICE, .voice_index = (uint8_t)((e->last_voice + 1u) % 6u),
			.len = DMR_VOICE_BYTES };
		memcpy(s.payload, ambe_silence, DMR_VOICE_BYTES);
		setup_burst(e, &s);
	} else if (e->last_kind == DMR_BURST_VOICE_LC_HEADER) {
		e->st.tx_underrun++;
		dmr_tx_burst_t h = e->last_hdr;
		setup_burst(e, &h);
	} else {
		if (e->last_kind != KIND_UNSET) {
			e->st.tx_underrun++;
		}
		set_ctl(e, CTL_NONE);
	}
}

static void tx_tick(hrc6000_dmr_t *e, uint8_t r42)
{
	uint8_t next_phase = (uint8_t)((e->tick + 1u) & 1u);
	switch (e->tx_state) {
	case TXS_START: {
		bool go;
		if (e->tx_timing == DMR_TIMING_ALIGNED) {
			go = e->phase_ts[next_phase] != 0 && e->phase_ts[next_phase] == e->tx_ts;
		} else {
			go = true;	// own timing just started, or the timing we have: the next slot is ours
		}
		if (e->tx_abort_req) {
			tx_finish(e);
			return;
		}
		if (!go) {
			if (++e->tx_wait > ALIGN_TIMEOUT_TICKS) {
				e->st.tx_rejected++;
				push(e, DMR_EV_TX_REJECTED, e->tx_ts, 1);	// arg 1: no slot timing to align to
				tx_finish(e);
				return;
			}
			set_ctl(e, rx_ctl(e));	// keep listening while waiting for our slot
			return;
		}
		e->tx_phase = next_phase;
		if (e->tx_ts <= 0) {
			e->tx_ts = e->phase_ts[next_phase];
		}
		e->tx_state = TXS_RUN;
		tx_slot(e);
		return;
	}
	case TXS_RUN:
		if (next_phase == e->tx_phase) {
			tx_slot(e);
		} else {
			set_ctl(e, CTL_NONE);
		}
		return;
	case TXS_ENDING: {
		if (e->ctl_next != CTL_NONE) {
			set_ctl(e, CTL_NONE);
		}
		uint32_t since = e->tick - e->tx_last_setup;
		// Over only once the last burst's slot has gone by: its PA dropped, the slot starting not a TX slot
		// (gotchas 3.1, 3.2)
		bool done = !e->tx_any_setup || (since >= 2u && !e->pa_on && (r42 & STAT_TX_SLOT) == 0)
			|| since >= DONE_FALLBACK_TICKS;
		if (done) {
			tx_finish(e);
		}
		return;
	}
	default:
		return;
	}
}

// --- Engine: receive ---

// CACH TC tracking at the timeslot interrupt (5.3); only in slots being received, while a repeater is heard
static void track_tc(hrc6000_dmr_t *e)
{
	uint8_t r52 = r(e->dev, R_RX_CC);
	int8_t heard = (int8_t)(((r52 >> 2) & 1u) ? 2 : 1);
	if (!HRC6000_DMR_TC_IS_SLOT_NOW) {
		heard = (int8_t)(3 - heard);	// it was the slot that ended: the one starting is the other
	}
	if (e->own_ts != 0) {
		e->own_ts = (int8_t)(3 - e->own_ts);
	}
	if (e->own_ts == 0) {
		e->own_ts = heard;
		e->tc_agree = 0;
		e->tc_disagree = 0;
	} else if (heard == e->own_ts) {
		if (e->tc_agree < 255u) {
			e->tc_agree++;
		}
	} else {
		e->tc_disagree++;
		if (e->tc_agree > 0) {
			e->tc_agree--;
		}
		if (e->tc_disagree > TC_RESYNC) {
			e->own_ts = heard;
			e->tc_agree = 0;
			e->tc_disagree = 0;
		}
	}
	if (e->tc_agree > TC_STABLE) {
		uint8_t ph = (uint8_t)(e->tick & 1u);
		e->phase_ts[ph] = e->own_ts;
		e->phase_ts[ph ^ 1u] = (int8_t)(3 - e->own_ts);
		e->tc_disagree = 0;
	} else if (e->tc_agree == 0) {
		e->phase_ts[0] = 0;
		e->phase_ts[1] = 0;
	}
}

static void cc_follow(hrc6000_dmr_t *e, uint8_t cc, uint32_t t)
{
	if (!cc_any(e) || e->cc_held || e->tx_state != TXS_IDLE) {
		return;
	}
	if (cc == e->cc_last && (uint32_t)(t - e->t_cc) < CC_MATCH_US) {
		e->cc_matches++;
	} else {
		e->cc_matches = 1;
	}
	e->cc_last = cc;
	e->t_cc = t;
	if (e->cc_matches >= CC_SET_MATCHES && cc != e->cc_cur) {
		e->cc_cur = cc;
		hrc6000_dmr_set_cc(e->dev, cc);
		e->restart_pending = true;	// at the next timeslot interrupt: no busy wait here (gotchas 1.7)
	}
	if (e->cc_matches >= CC_HOLD_MATCHES) {
		e->cc_held = true;
		e->cc_hold_tick = e->tick;
	}
}

static void rx_report(hrc6000_dmr_t *e, uint32_t t, uint8_t r52)
{
	uint8_t r51 = r(e->dev, R_RX_TYPE);
	uint8_t r5f = r(e->dev, R_SYNC_TYPE);
	hrc6000_rx_status_t s;
	hrc6000_dmr_decode(r51, r52, r5f, &s);
	e->st.last_r51 = r51;

	bool tx = e->tx_state != TXS_IDLE;
	uint32_t dt = t - e->t_tick;
	uint32_t dms = dt / 1000u;
	e->st.last_report_ms = (uint8_t)(dms > 255u ? 255u : dms);
	bool in_step = e->timing && dt >= IN_STEP_MIN_US && dt <= IN_STEP_MAX_US;

	if (s.kind == HRC6000_KIND_NONE) {
		e->st.rx_rc++;
		return;
	}
	if (e->timing && dt >= OUT_OF_STEP_US) {
		e->st.rx_out_of_step++;
		// The chip's timing is off the sender's (it locked to noise): resynchronize (gotchas 1.5)
		if (!tx && ++e->out_of_step_run >= OUT_OF_STEP_RESYNC) {
			e->restart_pending = true;
			e->out_of_step_run = 0;
		}
	} else if (in_step) {
		e->out_of_step_run = 0;
	}

	// One report per slot (gotchas 1.2)
	if (e->timing && e->report_valid && e->report_tick == e->tick) {
		e->st.rx_dup++;
		return;
	}
	// Following an in-step signal, a report off the slot timing is an extra one (the EMB report mid-slot)
	if (!tx && !in_step && e->rx_any && (uint32_t)(e->tick - e->rx_tick) <= 2u && e->timing) {
		e->st.rx_dup++;
		return;
	}
	bool voice = s.kind == DMR_BURST_VOICE;
	if (voice && e->voice_valid
	    && (uint32_t)(t - e->t_voice) < (e->lock_ts > 0 ? VOICE_DEDUP_US : VOICE_DEDUP_BOTH_US)) {
		e->st.rx_dup++;
		return;
	}

	// A new reception: capture the sticky LC so a stale one isn't taken for this call's (5.2)
	bool fresh = !e->heard_any || (uint32_t)(e->tick - e->heard_tick) > RX_LOST_TICKS;	// not reset by restarts
	if (fresh) {
		hrc6000_read_burst(e->dev, HRC6000_PAGE_DATA, 0x00, e->stale_lc, sizeof(e->stale_lc));
		e->voice_since_start = 0;
	}

	// Slot numbers (see hrc6000_dmr.h): the report is for the slot that just ended
	uint8_t ph = (uint8_t)((e->tick - 1u) & 1u);
	if (s.sync == DMR_SYNC_BS) {
		e->bs_tick = e->tick;
		e->rmo_seen = true;
	} else if (in_step && !tx && !e->cfg.repeater) {
		int8_t want = s.sync == DMR_SYNC_TS2 ? 2 : 1;
		bool tdma = s.sync == DMR_SYNC_TS1 || s.sync == DMR_SYNC_TS2;
		if (e->phase_ts[ph] == 0 || (tdma && e->phase_ts[ph] != want)) {
			e->phase_ts[ph] = want;
			e->phase_ts[ph ^ 1u] = (int8_t)(3 - want);
		}
	}

	e->report_tick = e->tick;
	e->report_valid = true;
	e->heard_tick = e->tick;
	e->heard_any = true;
	e->rx_tick = e->tick;
	e->rx_any = true;
	if (voice) {
		e->t_voice = t;
		e->voice_valid = true;
		if (e->voice_since_start < 255u) {
			e->voice_since_start++;
		}
	}
	e->st.last_cc = s.cc;
	e->st.last_sync = s.sync;
	e->st.last_ts = e->phase_ts[ph];
	if (!s.crc_ok) {
		e->st.rx_crc_err++;
	}
	switch (s.kind) {
	case DMR_BURST_VOICE: e->st.rx_voice++; break;
	case DMR_BURST_VOICE_LC_HEADER: e->st.rx_lc_header++; break;
	case DMR_BURST_TERMINATOR_LC: e->st.rx_terminator++; break;
	case DMR_BURST_CSBK: case DMR_BURST_MBC_HEADER: case DMR_BURST_MBC_CONT: e->st.rx_csbk++; break;
	case DMR_BURST_IDLE: e->st.rx_idle++; break;
	default: e->st.rx_data++; break;
	}

	hrc6000_dmr_evslot_t *slot = ev_begin(e);
	if (slot == NULL) {
		return;
	}
	dmr_burst_t *b = &slot->burst;
	memset(b, 0, sizeof(*b));
	b->kind = s.kind;
	b->voice_index = s.voice_index;
	b->sync = s.sync;
	b->cc = s.cc;
	b->ts = e->phase_ts[ph];
	b->crc_ok = s.crc_ok;
	b->pi = s.pi;
	b->in_step = in_step && !tx;	// what the chip says while it sends is suspect (7.8)
	b->len = hrc6000_dmr_payload_len(s.kind);
	if (voice) {
		hrc6000_dmr_voice(e->dev, false, b->payload);
		if (s.voice_index == 5) {	// the embedded LC of B-E is assembled by F
			hrc6000_read_burst(e->dev, HRC6000_PAGE_DATA, 0x00, b->lc, sizeof(b->lc));
			bool changed = memcmp(b->lc, e->stale_lc, sizeof(b->lc)) != 0;
			b->lc_valid = s.crc_ok && (changed || e->voice_since_start > STALE_LC_VOICE);
		}
	} else {
		hrc6000_read_burst(e->dev, HRC6000_PAGE_DATA, 0x00, b->payload, b->len);
		if (s.kind == DMR_BURST_VOICE_LC_HEADER || s.kind == DMR_BURST_TERMINATOR_LC) {
			memcpy(b->lc, b->payload, sizeof(b->lc));
			b->lc_valid = s.crc_ok;
			if (s.crc_ok) {	// this call's LC from its own header: the embedded one may now equal it
				memcpy(e->stale_lc, b->lc, sizeof(e->stale_lc));
				e->voice_since_start = STALE_LC_VOICE + 1u;
			}
		}
	}
	slot->ev.kind = DMR_EV_BURST_RX;
	slot->ev.ts = b->ts;
	slot->ev.arg = e->tick;
	slot->ev.burst = NULL;
	ev_commit(e);
}

// --- Engine: ISRs ---

void hrc6000_dmr_isr_timeslot(hrc6000_dmr_t *e)
{
	if (!e->running) {
		return;
	}
	uint32_t t = now(e);
	// PA off first: our burst of the slot that just ended (hardware-dm1701.md 6, gotchas 3.4)
	if (e->pa_on && (uint32_t)(t - e->t_pa_on) > PA_MIN_ON_US) {
		pa_drop(e, t);
	}
	e->tick++;
	e->st.ticks++;
	e->t_tick = t;
	e->timing = true;
	e->ctl_now = e->ctl_next;
	uint8_t r42 = r(e->dev, R_SLOT_STAT);
	e->st.last_r42 = r42;

	bool idle = e->tx_state == TXS_IDLE || e->tx_state == TXS_START;
	if (idle && (e->ctl_now & CTL_RX) != 0 && rmo(e)) {
		track_tc(e);
	}
	uint8_t ph = (uint8_t)(e->tick & 1u);
	push(e, DMR_EV_SLOT_TICK, e->phase_ts[ph], e->tick);

	if (e->tx_state == TXS_IDLE) {
		uint32_t quiet = e->tick - e->rx_tick;
		if (e->cc_held && quiet > CC_RELEASE_TICKS) {
			e->cc_held = false;
			e->cc_matches = 0;
		}
		if (e->restart_pending) {
			do_restart(e);
			push(e, DMR_EV_SYNC_LOST, 0, e->tick);
		} else if ((e->rx_any && quiet > RX_LOST_TICKS) || (!e->rx_any && quiet > RX_IDLE_TICKS)) {
			do_restart(e);
		} else {
			uint8_t ctl = rx_ctl(e);
			if (e->lock_ts > 0) {
				int8_t nts = e->phase_ts[ph ^ 1u];
				if (nts != 0 && nts != e->lock_ts) {
					ctl = CTL_NONE;
				}
			}
			set_ctl(e, ctl);
		}
	} else {
		tx_tick(e, r42);
	}
	e->st.tx_state = e->tx_state;
	isr_done(e);
}

void hrc6000_dmr_isr_sys(hrc6000_dmr_t *e)
{
	if (!e->running) {
		return;
	}
	uint32_t t = now(e);
	e->st.sys_ints++;
	// The line stays low while any flag is set: clear what was read, and go again if it is still asserted
	for (int pass = 0; pass < 3; pass++) {
		uint8_t flags = r(e->dev, R_INT_FLAGS);
		uint8_t r52 = r(e->dev, R_RX_CC);
		if (flags == 0) {
			break;
		}
		if ((flags & INT_REJECTED) != 0 && e->tx_state != TXS_IDLE && e->tx_access != DMR_ACCESS_IMPOLITE
		    && !e->tx_abort_req) {
			e->st.tx_rejected++;
			push(e, DMR_EV_TX_REJECTED, e->tx_ts, 0);
			e->tx_abort_req = true;
			txq_flush(e);
		}
		if ((flags & INT_LATE_ENTRY) != 0) {
			e->st.late_entry++;
		}
		if ((flags & INT_RX_DATA) != 0) {
			rx_report(e, t, r52);
		}
		if ((flags & INT_VOICE_ABN) != 0) {
			uint8_t v = r(e->dev, R_VOICE_ABN);
			if ((v & 0x02u) != 0) {
				e->st.voice_lost++;
				push(e, DMR_EV_VOICE_LOST, 1, e->tick);
			}
			if ((v & 0x04u) != 0) {
				e->st.voice_lost++;
				push(e, DMR_EV_VOICE_LOST, 2, e->tick);
			}
		}
		cc_follow(e, (uint8_t)(r52 >> 4), t);
		w(e->dev, R_INT_CLEAR, flags);
		if (e->ops->sys_line_low == NULL || !e->ops->sys_line_low(e->ops->ctx)) {
			break;
		}
	}
	isr_done(e);
}

void hrc6000_dmr_isr_rftx(hrc6000_dmr_t *e)
{
	if (!e->running) {
		return;
	}
	uint32_t t = now(e);
	e->st.rftx_ints++;
	if (e->tx_state == TXS_IDLE) {
		return;		// nothing of ours set up: never key the PA
	}
	if (e->ops->rf(e->ops->ctx, true)) {
		bool was_on = e->pa_on;
		e->pa_on = true;
		e->t_pa_on = t;
		e->pa_on_tick = e->tick;
		if (!was_on) {
			push(e, DMR_EV_TX_SLOT_STARTED, e->phase_ts[e->tx_phase], e->tick);
		}
	} else {
		e->st.rf_fail++;
	}
	isr_done(e);
}

// --- Engine: task side ---

void hrc6000_dmr_init(hrc6000_dmr_t *e, hrc6000_t *dev, const hrc6000_dmr_ops_t *ops)
{
	memset(e, 0, sizeof(*e));
	e->dev = dev;
	e->ops = ops;
	e->cc_filter = -1;	// until configured (bring-up without the stack): follow whatever color code is heard
	e->cfg.cc = -1;
	e->cc_cur = 1;
	e->sync_cur = DMR_SYNC_MS;
	e->reg40 = L2_PASSIVE;
	e->last_kind = KIND_UNSET;
}

void hrc6000_dmr_run(hrc6000_dmr_t *e)
{
	lock(e);
	e->tx_state = TXS_IDLE;
	txq_flush(e);
	e->tick = 0;
	e->timing = false;
	e->pa_on = false;
	e->rmo_seen = false;
	e->reg40 = L2_PASSIVE;
	e->sync_cur = DMR_SYNC_MS;
	e->cc_held = false;
	e->cc_matches = 0;
	e->heard_any = false;
	hrc6000_dmr_set_sync(e->dev, DMR_SYNC_MS);
	if (e->cc_filter >= 0) {
		e->cc_cur = (uint8_t)e->cc_filter;
	}
	hrc6000_dmr_set_cc(e->dev, e->cc_cur);
	do_restart(e);
	e->running = true;
	unlock(e);
}

void hrc6000_dmr_halt(hrc6000_dmr_t *e)
{
	lock(e);
	if (e->pa_on) {
		pa_drop(e, now(e));
	}
	if (e->tx_state != TXS_IDLE) {
		e->tx_state = TXS_IDLE;
		push(e, DMR_EV_TX_DONE, e->tx_ts, e->tick);
	}
	txq_flush(e);
	if (e->running && e->sync_cur != DMR_SYNC_MS) {
		hrc6000_dmr_set_sync(e->dev, DMR_SYNC_MS);
		e->sync_cur = DMR_SYNC_MS;
	}
	e->running = false;
	e->timing = false;
	unlock(e);
}

bool hrc6000_dmr_pop(hrc6000_dmr_t *e, dmr_event_t *ev, dmr_burst_t *burst)
{
	if (e->evq_tail == e->evq_head) {
		return false;
	}
	__atomic_thread_fence(__ATOMIC_ACQUIRE);
	const hrc6000_dmr_evslot_t *s = &e->evq[e->evq_tail];
	*ev = s->ev;
	if (ev->kind == DMR_EV_BURST_RX) {
		*burst = s->burst;
		ev->burst = burst;
	}
	__atomic_thread_fence(__ATOMIC_RELEASE);
	e->evq_tail = (uint8_t)((e->evq_tail + 1u) % EV_N);
	return true;
}

void hrc6000_dmr_poll(hrc6000_dmr_t *e)
{
	lock(e);
	if (e->running) {
		uint32_t t = now(e);
		if (e->pa_on && (uint32_t)(t - e->t_pa_on) > PA_FORCE_OFF_US) {
			pa_drop(e, t);
		}
		if (e->timing && (uint32_t)(t - e->t_tick) > STALL_US) {
			e->timing = false;
			bool busy = e->tx_state != TXS_IDLE || (e->rx_any && (uint32_t)(e->tick - e->rx_tick) <= RX_LOST_TICKS);
			if (e->tx_state != TXS_IDLE) {
				e->tx_any_setup = false;	// nothing more will go out
				tx_finish(e);
			} else if (busy) {
				do_restart(e);
			}
			if (busy) {
				e->st.stalls++;
				push(e, DMR_EV_SYNC_LOST, 0, e->tick);
			}
		}
	}
	unlock(e);
}

void hrc6000_dmr_configure(hrc6000_dmr_t *e, const dmr_config_t *cfg)
{
	lock(e);
	e->cfg = *cfg;
	hrc6000_set_dmr_id(e->dev, cfg->own_id);
	e->cc_filter = cfg->cc;
	if (cfg->cc >= 0) {
		e->cc_cur = (uint8_t)cfg->cc;
		hrc6000_dmr_set_cc(e->dev, e->cc_cur);
	}
	e->cc_held = false;
	if (e->running && e->tx_state == TXS_IDLE) {
		do_restart(e);
	}
	unlock(e);
}

void hrc6000_dmr_rx_restart(hrc6000_dmr_t *e)
{
	lock(e);
	if (e->running) {
		if (e->tx_state == TXS_IDLE) {
			do_restart(e);
		} else {
			e->restart_pending = true;
		}
	}
	unlock(e);
}

void hrc6000_dmr_rx_lock_slot(hrc6000_dmr_t *e, int8_t ts)
{
	lock(e);
	e->lock_ts = ts > 0 && ts <= 2 ? ts : 0;
	unlock(e);
}

bool hrc6000_dmr_tx_begin(hrc6000_dmr_t *e, dmr_timing_t timing, int8_t ts, dmr_access_t access)
{
	lock(e);
	bool ok = e->running && e->tx_state == TXS_IDLE;
	if (ok) {
		uint32_t t = now(e);
		bool have_timing = e->timing && (uint32_t)(t - e->t_tick) < 100000u;
		if (timing == DMR_TIMING_ASAP && !have_timing) {
			timing = DMR_TIMING_OWN;
		}
		e->tx_timing = (uint8_t)timing;
		e->tx_ts = timing == DMR_TIMING_ALIGNED ? ts : 0;
		e->tx_access = (uint8_t)access;
		e->tx_end_req = false;
		e->tx_abort_req = false;
		e->tx_any_setup = false;
		e->tx_wait = 0;
		e->last_kind = KIND_UNSET;
		e->restart_pending = false;
		e->tx_state = TXS_START;
		// modem-hrc6000.md 6.2: access policy and encoder start, then own timing for direct mode
		w(e->dev, R_ACCESS0, 0x00);
		w(e->dev, R_ACCESS1, hrc6000_dmr_access_reg(access));
		w(e->dev, R_CODEC_CTL, 0x86);
		e->reg40 = timing == DMR_TIMING_OWN ? L2_ACTIVE : L2_PASSIVE;
		if (timing == DMR_TIMING_OWN) {
			w(e->dev, R_L2_CTL, L2_ACTIVE);
		}
	}
	unlock(e);
	return ok;
}

bool hrc6000_dmr_tx_queue(hrc6000_dmr_t *e, const dmr_tx_burst_t *b)
{
	lock(e);
	bool ok = txq_count(e) < TXQ_N - 1u;
	if (ok) {
		e->txq[e->txq_head] = *b;
		__atomic_thread_fence(__ATOMIC_RELEASE);
		e->txq_head = (uint8_t)((e->txq_head + 1u) % TXQ_N);
	}
	unlock(e);
	return ok;
}

uint8_t hrc6000_dmr_tx_queue_space(const hrc6000_dmr_t *e)
{
	return (uint8_t)(TXQ_N - 1u - txq_count(e));
}

void hrc6000_dmr_tx_end(hrc6000_dmr_t *e)
{
	lock(e);
	if (e->tx_state != TXS_IDLE) {
		e->tx_end_req = true;
	}
	unlock(e);
}

void hrc6000_dmr_tx_abort(hrc6000_dmr_t *e)
{
	lock(e);
	if (e->tx_state != TXS_IDLE) {
		e->tx_abort_req = true;
		txq_flush(e);
		if (e->tx_state == TXS_START) {
			tx_finish(e);
		} else if (e->ctl_next == CTL_TX && e->tick == e->tx_last_setup) {
			set_ctl(e, CTL_NONE);	// best effort: cancel the burst set up for the next slot
		}
	}
	unlock(e);
}

void hrc6000_dmr_set_embedded_lc(hrc6000_dmr_t *e, const uint8_t lc[9])
{
	lock(e);
	memcpy(e->emb_lc, lc, sizeof(e->emb_lc));
	e->emb_set = true;
	e->emb_pending = true;
	unlock(e);
}

void hrc6000_dmr_set_cc_filter(hrc6000_dmr_t *e, int8_t cc)
{
	lock(e);
	e->cc_filter = cc;
	e->cc_held = false;
	e->cc_matches = 0;
	if (cc >= 0) {
		e->cc_cur = (uint8_t)cc;
		if (e->running) {
			hrc6000_dmr_set_cc(e->dev, e->cc_cur);
		}
	}
	if (e->running) {
		e->restart_pending = true;	// 5.1 after a color code change
	}
	unlock(e);
}

bool hrc6000_dmr_is_transmitting(const hrc6000_dmr_t *e)
{
	return e->tx_state != TXS_IDLE;
}

void hrc6000_dmr_stats(const hrc6000_dmr_t *e, hrc6000_dmr_stats_t *out)
{
	*out = e->st;
}

#endif	// CONFIG_DMR
