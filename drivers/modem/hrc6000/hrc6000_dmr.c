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
#define VOICE_DEDUP_US		45000u	// the same burst reported again
#define VOICE_DEDUP_BOTH_US	28000u	// any two voice bursts
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
	// Bench 2026-10-10 (one MS sync call: 8 headers, 96 voice, 3 terminators): the class was 2 for exactly the 11
	// headers and terminators, 1 for the 16 voice A, 0 for everything else, and a report came at every tick (both
	// slots). Class 0 reports carry a voice number in the low 3 bits of the type, with bit 7 set or clear (0x1-0x6
	// as often as 0x9-0xE, gotchas 1.3), and stale ones keep repeating the last number. So:
	//   class 2: the ETSI data type (the only class whose type is a data type)
	//   class 1: voice A (voice sync)
	//   class 0: a voice B-F candidate; its number from the low 3 bits when 1-6, else unknown. The engine takes it
	//            only within a superframe in progress on that slot, by sequence
	//   class 3: reverse channel, dropped
	// For voice, bit 3 (PI) is unreliable (it is set on plain calls)
	uint8_t low = nib & 7u;
	switch (st->sync_class) {
	case 2:
		if (nib <= 0x0Bu) {
			st->kind = nib;
		}
		break;
	case 1:
		st->kind = DMR_BURST_VOICE;
		st->pi = false;
		break;
	case 0:
		st->kind = DMR_BURST_VOICE;
		st->voice_index = low >= 1u && low <= 6u ? (uint8_t)(low - 1u) : HRC6000_INDEX_UNKNOWN;
		st->pi = false;
		break;
	default:
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

static void do_restart(hrc6000_dmr_t *e, uint8_t why)
{
	if (why < HRC6000_RS_COUNT) {
		e->st.rs_why[why]++;
	}
	hrc6000_dmr_chip_rx_restart(e->dev, rx_ctl(e));
	e->ctl_next = rx_ctl(e);
	forget_slots(e);
	e->held[0].valid = false;	// the timing they were heard in is gone
	e->held[1].valid = false;
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
		do_restart(e, HRC6000_RS_TX_END);	// direct mode: follow whoever talks next
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
		e->restart_why = HRC6000_RS_CC;
	}
	if (e->cc_matches >= CC_HOLD_MATCHES) {
		e->cc_held = true;
		e->cc_hold_tick = e->tick;
	}
}

// Raw evidence for the bench, before anything is dropped: status class and type, sync type, report timing
static void rx_evidence(hrc6000_dmr_t *e, uint8_t r51, uint8_t r5f, uint32_t dt)
{
	static const uint32_t edges_us[HRC6000_DT_BINS - 1] = { 1000u, 3000u, 6000u, 12000u, 22000u };
	e->st.last_r51 = r51;
	e->st.last_r5f = r5f;
	e->st.by_class[r51 & 3u]++;
	e->st.by_type[r51 >> 4]++;
	e->st.by_r5f[r5f & 3u]++;
	if (!e->timing) {
		return;
	}
	uint8_t bin = 0;
	while (bin < HRC6000_DT_BINS - 1 && dt >= edges_us[bin]) {
		bin++;
	}
	e->st.dt_hist[bin]++;
}

// The dedup state of a report taken (one per slot, the repeat check), whether or not it is delivered now
static void note_report(hrc6000_dmr_t *e, const hrc6000_rx_status_t *s, const dmr_burst_t *nb, uint32_t t)
{
	e->report_tick = e->tick;
	e->report_valid = true;
	e->t_report = t;
	e->last_rx_kind = s->kind;
	e->last_rx_index = s->voice_index;
	e->last_rx_sync = s->sync;
	memcpy(e->last_rx_payload, nb->payload, sizeof(e->last_rx_payload));
	if (s->kind == DMR_BURST_VOICE) {
		e->t_voice = t;
		e->voice_valid = true;
	}
}

static void held_drop(hrc6000_dmr_t *e, hrc6000_dmr_held_t *hd)
{
	if (hd->valid) {
		hd->valid = false;
		e->st.rx_lone++;
	}
}

// A new reception: capture the sticky LC so a stale one isn't taken for this call's (5.2). Also for a lone burst A
// held back, so the capture is from the start of the reception and not from when its B confirmed it
static void capture_if_fresh(hrc6000_dmr_t *e)
{
	bool fresh = !e->heard_any || (uint32_t)(e->tick - e->heard_tick) > RX_LOST_TICKS;	// not reset by restarts
	if (fresh) {
		hrc6000_read_burst(e->dev, HRC6000_PAGE_DATA, 0x00, e->stale_lc, sizeof(e->stale_lc));
		e->voice_since_start = 0;
	}
	e->heard_tick = e->tick;
	e->heard_any = true;
}

// Deliver a burst taken: reception bookkeeping, slot number, counters, the event. tick: the tick of its report (the
// slot that ended at tick - 1 ... the report is for the slot that just ended)
static void take(hrc6000_dmr_t *e, const hrc6000_rx_status_t *s, const dmr_burst_t *nb, uint32_t tick, bool in_step,
		 bool on_time, uint8_t dms, bool tx)
{
	bool voice = s->kind == DMR_BURST_VOICE;

	capture_if_fresh(e);

	// Slot numbers (see hrc6000_dmr.h): the report is for the slot that just ended
	uint8_t ph = (uint8_t)((tick - 1u) & 1u);
	if (s->sync == DMR_SYNC_BS) {
		e->bs_tick = e->tick;
		e->rmo_seen = true;
	} else if (on_time && !tx && !e->cfg.repeater) {
		int8_t want = s->sync == DMR_SYNC_TS2 ? 2 : 1;
		bool tdma = s->sync == DMR_SYNC_TS1 || s->sync == DMR_SYNC_TS2;
		if (e->phase_ts[ph] == 0 || (tdma && e->phase_ts[ph] != want)) {
			e->phase_ts[ph] = want;
			e->phase_ts[ph ^ 1u] = (int8_t)(3 - want);
		}
	}

	e->rx_tick = e->tick;
	e->rx_any = true;
	e->idle_restarted = false;
	if (voice && e->voice_since_start < 255u) {
		e->voice_since_start++;
	}
	e->st.last_cc = s->cc;
	e->st.last_sync = s->sync;
	e->st.last_ts = e->phase_ts[ph];
	if (!s->crc_ok) {
		e->st.rx_crc_err++;
	}
	switch (s->kind) {
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
	*b = *nb;
	b->kind = s->kind;
	b->voice_index = s->voice_index;
	b->sync = s->sync;
	b->cc = s->cc;
	b->ts = e->phase_ts[ph];
	b->crc_ok = s->crc_ok;
	b->pi = s->pi;
	b->in_step = in_step && !tx;	// what the chip says while it sends is suspect (7.8)
	if (voice) {
		if (s->voice_index == 5) {	// the embedded LC of B-E is assembled by F
			hrc6000_read_burst(e->dev, HRC6000_PAGE_DATA, 0x00, b->lc, sizeof(b->lc));
			bool changed = memcmp(b->lc, e->stale_lc, sizeof(b->lc)) != 0;
			b->lc_valid = s->crc_ok && (changed || e->voice_since_start > STALE_LC_VOICE);
		}
	} else if (s->kind == DMR_BURST_VOICE_LC_HEADER || s->kind == DMR_BURST_TERMINATOR_LC) {
		memcpy(b->lc, b->payload, sizeof(b->lc));
		b->lc_valid = s->crc_ok;
		if (s->crc_ok) {	// this call's LC from its own header: the embedded one may now equal it
			memcpy(e->stale_lc, b->lc, sizeof(e->stale_lc));
			e->voice_since_start = STALE_LC_VOICE + 1u;
		}
	}
	slot->ev.kind = DMR_EV_BURST_RX;
	slot->ev.ts = b->ts;
	slot->ev.arg = tick;	// DMR_EV_BURST_RX: the slot counter of the report (never 0); the stack orders bursts by it
	slot->ev.burst = NULL;
	slot->dms = dms;
	ev_commit(e);
}

// One received-data report. Returns whether the color code in 0x52 belongs to a burst (for the CC follower): not for
// voice (burst A has no CC on air; B-F under the CC filter get the filter's) nor for unusable reports.
// at_tick: serviced from the timeslot interrupt, before the tick advanced: the report's interrupt was not taken in its
// slot (no edge seen), so its delay is unknown; it still belongs to the slot that ended at e->tick
static bool rx_report(hrc6000_dmr_t *e, uint32_t t, uint8_t r52, bool at_tick)
{
	uint8_t r51 = r(e->dev, R_RX_TYPE);
	uint8_t r5f = r(e->dev, R_SYNC_TYPE);
	hrc6000_rx_status_t s;
	hrc6000_dmr_decode(r51, r52, r5f, &s);

	bool tx = e->tx_state != TXS_IDLE;
	uint32_t dt = t - e->t_tick;
	uint32_t dms = dt / 1000u;
	rx_evidence(e, r51, r5f, dt);
	e->st.last_report_ms = (uint8_t)(at_tick || dms > 255u ? 255u : dms);
	bool in_step = !at_tick && e->timing && dt >= IN_STEP_MIN_US && dt <= IN_STEP_MAX_US;
	// Within the slot it belongs to, early or late: good enough to name the slot (in_step is only the chip's normal
	// 1-12 ms; bench 2026-10-10 on 90ce091: reports 12-22 ms after the tick)
	bool on_time = !at_tick && e->timing && dt >= IN_STEP_MIN_US && dt < OUT_OF_STEP_US;
	e->report_dms = (uint8_t)(at_tick || dms > 254u ? 255u : dms);

	if (s.kind == HRC6000_KIND_NONE) {
		e->st.rx_rc++;
		return false;
	}
	bool cc_real = s.sync_class != 1;
	if (s.kind != DMR_BURST_VOICE) {
		e->st.last_data_r51 = r51;
	}
	if (e->timing && !at_tick && dt >= OUT_OF_STEP_US) {
		e->st.rx_out_of_step++;
		// The chip's timing is off the sender's (it locked to noise): resynchronize (gotchas 1.5)
		if (!tx && ++e->out_of_step_run >= OUT_OF_STEP_RESYNC) {
			e->restart_pending = true;
			e->restart_why = HRC6000_RS_OOS;
			e->out_of_step_run = 0;
		}
	} else if (in_step) {
		e->out_of_step_run = 0;
	}

	// One report per slot (gotchas 1.2)
	if (e->timing && e->report_valid && e->report_tick == e->tick) {
		e->st.rx_dup++;
		e->st.rx_dup_slot++;
		return cc_real;
	}
	// No rule on the report's delay alone: a report off the 1-12 ms window used to be dropped as an extra one if a
	// burst was taken in the last two slots, which on the bench (90ce091, reports 12-22 ms after the tick) dropped
	// every other voice burst (A, C, E). A second report for one burst is caught by the one-per-slot rule (same
	// tick), the repeat rules below (same number in the next slot) and the stale rule

	// The payload now: the repeat check below compares it
	dmr_burst_t nb;
	memset(&nb, 0, sizeof(nb));
	bool voice = s.kind == DMR_BURST_VOICE;
	nb.len = hrc6000_dmr_payload_len(s.kind);
	if (voice) {
		hrc6000_dmr_voice(e->dev, false, nb.payload);
	} else {
		hrc6000_read_burst(e->dev, HRC6000_PAGE_DATA, 0x00, nb.payload, nb.len);
	}

	// The same burst reported again at the next timeslot interrupt (bench 2026-10-09: every voice burst came twice,
	// both in step, 30 ms apart, so the 28 ms window let both through). A repeat: within 45 ms of the burst taken,
	// same kind and voice number, and for data the same payload (two calls on two slots never share one). TS1 / TS2
	// direct mode sync on different slots are two calls, not a repeat
	bool same = e->report_valid && s.kind == e->last_rx_kind
		    && !(s.sync != e->last_rx_sync && (s.sync == DMR_SYNC_TS1 || s.sync == DMR_SYNC_TS2))
		    && (voice ? s.voice_index == e->last_rx_index : memcmp(nb.payload, e->last_rx_payload, nb.len) == 0);
	if (same && (uint32_t)(t - e->t_report) < VOICE_DEDUP_US) {
		e->st.rx_dup++;
		e->st.rx_dup_repeat++;
		return cc_real;
	}
	// The same voice number in the very next slot, whatever the delays (a late report and the empty slot's)
	if (same && voice && s.voice_index != HRC6000_INDEX_UNKNOWN && (uint32_t)(e->tick - e->report_tick) <= 1u) {
		e->st.rx_dup++;
		e->st.rx_dup_extra++;
		return cc_real;
	}
	if (voice && e->voice_valid && (uint32_t)(t - e->t_voice) < VOICE_DEDUP_BOTH_US) {
		e->st.rx_dup++;		// any voice within 28 ms of the last (gotchas 1.2)
		e->st.rx_dup_repeat++;
		return cc_real;
	}

	// Voice by superframe, per slot phase (the report is for the slot that just ended): a class 1 report (voice
	// sync) is burst A and starts or restarts the superframe; a class 0 report is taken only while a superframe is
	// in progress on its slot, as the next burst. Its own number is used when it makes sense (a missed burst then
	// doesn't shift the rest); the number of the burst just taken is a stale report (dropped).
	// A voice sync with no header or superframe before it on its slot is held back, not delivered: only a burst B
	// on the same slot one frame later (class 0, number 2) makes it the start of a call (late entry); anything else
	// and it was noise or the other slot's echo of A (bench 2026-10-10: 6 such reports made 102 voice bursts of a
	// 96-burst call). Burst A has no color code on air (voice sync instead of the EMB), so it gets the superframe's
	uint8_t vph = (uint8_t)((e->tick - 1u) & 1u);
	hrc6000_dmr_vstate_t *vs = &e->vs[vph];
	hrc6000_dmr_held_t *hd = &e->held[vph];
	// The chip deframes only bursts of the color code in 0x1F (modem-hrc6000.md 5.4): voice it reports under the CC
	// filter is ours. 0x52 is not reliable for voice (A has no CC on air; bench 2026-10-10 on 90ce091: CC 5 in 0x52
	// for every voice burst of a CC 2 call, reports late in the slot)
	bool cc_fix = voice && e->cc_filter >= 0;
	if (voice) {
		bool live = vs->active && (uint32_t)(e->tick - vs->tick) <= RX_LOST_TICKS;
		if (s.sync_class == 1) {
			s.voice_index = 0;
			if (!live) {
				held_drop(e, hd);	// an earlier lone A that nothing followed
				hd->valid = true;
				hd->s = s;
				hd->nb = nb;
				hd->t = t;
				hd->tick = e->tick;
				hd->in_step = in_step;
				hd->on_time = on_time;
				hd->dms = e->report_dms;
				capture_if_fresh(e);
				note_report(e, &s, &nb, t);
				return false;
			}
			if (!cc_fix && s.cc != vs->cc) {
				e->st.rx_a_cc++;
				s.cc = vs->cc;
			}
		} else if (!live) {
			if (!(hd->valid && (uint32_t)(e->tick - hd->tick) == 2u && s.voice_index == 1u)) {
				e->st.rx_no_call++;	// stale status on the empty slot, or after the call: never voice
				return cc_real;
			}
			// B after a lone A: the A was a call's (late entry). Deliver it first, with B's color code
			hd->valid = false;
			hd->s.cc = cc_fix ? (uint8_t)e->cc_filter : s.cc;
			take(e, &hd->s, &hd->nb, hd->tick, hd->in_step, hd->on_time, hd->dms, tx);
			vs->active = true;
			vs->last = 0;
			vs->next = 1;
			vs->cc = s.cc;
			vs->tick = hd->tick;
		} else if (s.voice_index == HRC6000_INDEX_UNKNOWN) {
			s.voice_index = vs->next;
			e->st.rx_seq_fill++;
		} else if (s.voice_index == vs->last) {
			e->st.rx_stale++;
			return cc_real;
		} else if (s.voice_index != vs->next) {
			e->st.rx_seq_jump++;	// a burst was missed: keep the chip's number
		}
		if (s.sync_class != 1) {
			vs->cc = s.cc;
		}
		vs->active = true;
		vs->last = s.voice_index;
		vs->next = (uint8_t)((s.voice_index + 1u) % 6u);
		vs->tick = e->tick;
	} else {
		held_drop(e, hd);	// a lone A followed by data on its slot: not a call
		// A PI header comes between the voice LC header and burst A: it keeps a superframe opened by a header, and
		// anywhere else it opens nothing (bench 2026-10-10: terminators read late came as type 0, and the stale
		// reports after the call were then taken as voice). A call joined at its PI header starts at A (held for B)
		bool pi_in_header = s.kind == DMR_BURST_PI_HEADER && vs->active && vs->last == HRC6000_INDEX_UNKNOWN
				    && (uint32_t)(e->tick - vs->tick) <= RX_LOST_TICKS;
		if (s.kind == DMR_BURST_VOICE_LC_HEADER || pi_in_header) {
			vs->active = true;	// voice A comes next (with its sync)
			vs->last = HRC6000_INDEX_UNKNOWN;
			vs->next = 0;
			vs->cc = s.cc;
			vs->tick = e->tick;
		} else {
			vs->active = false;	// terminator or data: no voice on this slot until the next A or header
		}
	}

	if (cc_fix && s.cc != (uint8_t)e->cc_filter) {
		e->st.rx_a_cc++;
		s.cc = (uint8_t)e->cc_filter;
	}
	note_report(e, &s, &nb, t);
	take(e, &s, &nb, e->tick, in_step, on_time, e->report_dms, tx);
	return cc_real;
}

// --- Engine: ISRs ---

static void sys_service(hrc6000_dmr_t *e, uint32_t t, bool at_tick);

void hrc6000_dmr_isr_timeslot(hrc6000_dmr_t *e)
{
	if (!e->running) {
		return;
	}
	uint32_t t = now(e);
	// A report whose system interrupt was not taken in its slot (the line held low, no edge): service it now, before
	// the tick advances, so it is read before the chip moves on and is counted for the slot it belongs to (the
	// board's task would otherwise pend it later, in the next slot)
	if (e->ops->sys_line_low != NULL && e->ops->sys_line_low(e->ops->ctx)) {
		e->st.sys_at_tick++;
		sys_service(e, t, true);
	}
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
	for (int k = 0; k < 2; k++) {	// a lone burst A whose B (one frame later) did not come
		if (e->held[k].valid && (uint32_t)(e->tick - e->held[k].tick) > 2u) {
			held_drop(e, &e->held[k]);
		}
	}

	if (e->tx_state == TXS_IDLE) {
		uint32_t quiet = e->tick - e->rx_tick;
		if (e->cc_held && quiet > CC_RELEASE_TICKS) {
			e->cc_held = false;
			e->cc_matches = 0;
		}
		if (e->restart_pending) {
			do_restart(e, e->restart_why);
			push(e, DMR_EV_SYNC_LOST, 0, e->tick);
		} else if (e->rx_any && quiet > RX_LOST_TICKS) {
			do_restart(e, HRC6000_RS_LOST);		// a reception is over (5.5)
			e->idle_restarted = true;
		} else if (!e->rx_any && !e->idle_restarted && quiet > RX_IDLE_TICKS) {
			// Once per quiet period, not every 600 ms: a restart drops the sync, and one landing in a call's
			// header phase cost the headers (bench 2026-10-09: 43 restarts in 21 s, no header taken). A chip that
			// locked to noise is caught by the out-of-step rule instead
			do_restart(e, HRC6000_RS_IDLE);
			e->idle_restarted = true;
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

// The system interrupt's work: from its own ISR, or from the timeslot ISR when the line is still low there (at_tick)
static void sys_service(hrc6000_dmr_t *e, uint32_t t, bool at_tick)
{
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
		bool cc_real = true;
		if ((flags & INT_RX_DATA) != 0) {
			cc_real = rx_report(e, t, r52, at_tick);
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
		if (cc_real) {
			cc_follow(e, (uint8_t)(r52 >> 4), t);
		}
		w(e->dev, R_INT_CLEAR, flags);
		if (e->ops->sys_line_low == NULL || !e->ops->sys_line_low(e->ops->ctx)) {
			break;
		}
	}
}

void hrc6000_dmr_isr_sys(hrc6000_dmr_t *e)
{
	if (!e->running) {
		return;
	}
	uint32_t t = now(e);
	e->st.sys_ints++;
	sys_service(e, t, false);
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
	e->idle_restarted = false;
	memset(e->vs, 0, sizeof(e->vs));
	memset(e->held, 0, sizeof(e->held));
	hrc6000_dmr_set_sync(e->dev, DMR_SYNC_MS);
	if (e->cc_filter >= 0) {
		e->cc_cur = (uint8_t)e->cc_filter;
	}
	hrc6000_dmr_set_cc(e->dev, e->cc_cur);
	do_restart(e, HRC6000_RS_CMD);
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
	e->pop_dms = 255;
	if (ev->kind == DMR_EV_BURST_RX) {
		*burst = s->burst;
		ev->burst = burst;
		e->pop_dms = s->dms;
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
				do_restart(e, HRC6000_RS_STALL);
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
		do_restart(e, HRC6000_RS_CMD);
	}
	unlock(e);
}

void hrc6000_dmr_rx_restart(hrc6000_dmr_t *e)
{
	lock(e);
	if (e->running) {
		if (e->tx_state == TXS_IDLE) {
			do_restart(e, HRC6000_RS_CMD);
		} else {
			e->restart_pending = true;
			e->restart_why = HRC6000_RS_CMD;
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
		e->restart_why = HRC6000_RS_CMD;
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
