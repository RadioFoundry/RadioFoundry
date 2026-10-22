// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The split duplex TX half's continuous BS outbound on the HR_C7000 (see hrc7000_bs.h)
#include <string.h>
#include "hrc7000_bs.h"

#if defined(CONFIG_HOTSPOT_SPLIT) && CONFIG_HOTSPOT_SPLIT	// the whole file

#define WORK_MODE_PHY_CONT	0x02u	// physical layer, continuous mode (DS 9.5.3)
#define L2_PHY_TX_MASTER	0x82u	// txen, tx_master_mode: own TX slot timing (DS 9.2.2.48)
#define L2_PASSIVE		0xC0u
#define TXRX_SEND		0x80u	// txnextsloten (DS 9.2.2.49)
#define TXRX_AUTOTEST		0x01u
#define TXRX_NONE		0x00u
#define TXRX_SYNC_RELEASE	0x800000u
#define TXRX_TX_SYNC_RELEASE	0x400000u
#define SYS_PHY_RDY		(1u << 12)
#define RAM_HALF0		0x00u	// SEND_DATA0 / SEND_DATA1: one slot each, sent alternately (DS 9.4.1)
#define RAM_HALF1		0x24u
#define SLOT_US			30000u
#define LATE_US			33000u
#define STALL_US		200000u

// The idle message's information bits (ETSI TS 102 361-1 table D.2)
static const uint8_t IDLE_INFO[12] = { 0xFF, 0x83, 0xDF, 0x17, 0x32, 0x09, 0x4E, 0xD1, 0xE7, 0xCD, 0x8A, 0x91 };

static inline void w(hrc7000_dmr_t *e, uint32_t off, uint32_t v) { hrc7000_wr(e->dev, off, v); }
static inline uint32_t now(hrc7000_dmr_t *e) { return e->ops->now_us(e->ops->ctx); }
static void lock(hrc7000_dmr_t *e) { if (e->ops->lock != NULL) e->ops->lock(e->ops->ctx); }
static void unlock(hrc7000_dmr_t *e) { if (e->ops->unlock != NULL) e->ops->unlock(e->ops->ctx); }

static int8_t ts_of(const hrc7000_bs_t *bs, uint32_t tick)
{
	return (((tick - bs->base_tick) & 1u) != 0) ? 1 : 2;	// the slot set up at boundary tick - 1
}

static uint32_t txrx_send(const hrc7000_bs_t *bs)
{
	return TXRX_SEND | (((bs->cfg.variant & HRC7000_BS_V_AUTOTEST) != 0) ? TXRX_AUTOTEST : 0u);
}

static void pa_off(hrc7000_dmr_t *e, hrc7000_bs_t *bs)
{
	if (bs->pa) {
		e->ops->rf(e->ops->ctx, false);
		bs->pa = false;
		e->rf_tx = false;
	}
}

bool hrc7000_bs_start(hrc7000_bs_t *bs, hrc7000_dmr_t *e, const dmr_bs_config_t *cfg, hrc7000_bs_raw_fn raw_slot)
{
	lock(e);
	bool ok = e->running && (e->bs == NULL) && (e->tx_state == 0) && (raw_slot != NULL);

	if (ok) {
		memset(bs, 0, sizeof(*bs));
		bs->cfg = *cfg;
		bs->raw_slot = raw_slot;
		bs->half = ((cfg->variant & HRC7000_BS_V_HALF_MASK) == HRC7000_BS_V_HALF_ALT1) ? 1u : 0u;
		w(e, C7_LAYER2_TXRX_CTRL, TXRX_NONE);
		w(e, C7_LAYER2_SLOTON, 0);
		// Through FM, as every DMR entry: resets the state machine (5.1). Then the physical layer, own TX timing
		w(e, C7_WORK_MODE, C7_WORK_MODE_FM);
		w(e, C7_WORK_MODE, ((cfg->variant & HRC7000_BS_V_CONTINUOUS) != 0) ? WORK_MODE_PHY_CONT : C7_WORK_MODE_PHY);
		w(e, C7_LAYER2_CONTROL, L2_PHY_TX_MASTER);
		w(e, C7_AUDIO_BUFFER_CLR, 0x03);
		e->send_sync = DMR_SYNC_NONE;	// the bursts carry their own sync; the next normal one rewrites it
		bs->base_tick = e->tick;
		e->bs = bs;
		// A new slot clock: the receiver's one is not the outbound's. The stall watchdog counts from here
		e->timing = false;
		e->t_tick = now(e);
	}
	unlock(e);
	return ok;
}

void hrc7000_bs_key(hrc7000_dmr_t *e, bool on)
{
	lock(e);
	hrc7000_bs_t *bs = e->bs;

	if (bs != NULL) {
		if (on && !bs->keyed) {
			bs->st.keyups++;
		}
		bs->keyed = on;
		if (!on) {
			bs->head[0] = bs->tail[0];
			bs->head[1] = bs->tail[1];
		}
	}
	unlock(e);
}

static uint8_t q_count(const hrc7000_bs_t *bs, int k)
{
	return (uint8_t)((bs->head[k] + HRC7000_BS_Q + 1u - bs->tail[k]) % (HRC7000_BS_Q + 1u));
}

bool hrc7000_bs_queue(hrc7000_dmr_t *e, int8_t ts, const dmr_tx_burst_t *b, const dmr_cach_t *cach,
		      const uint8_t *emb_lc)
{
	lock(e);
	hrc7000_bs_t *bs = e->bs;
	int k = (ts == 1) ? 0 : 1;
	bool ok = (bs != NULL) && (q_count(bs, k) < HRC7000_BS_Q);

	if (ok) {
		uint8_t at = bs->head[k];

		bs->q[k][at] = *b;
		bs->q_cach_valid[k][at] = cach != NULL;
		if (cach != NULL) {
			bs->q_cach[k][at] = *cach;
		}
		bs->q_lc_valid[k][at] = emb_lc != NULL;
		if (emb_lc != NULL) {
			memcpy(bs->q_lc[k][at], emb_lc, 9);
		}
		bs->head[k] = (uint8_t)((bs->head[k] + 1u) % (HRC7000_BS_Q + 1u));
		bs->st.cach_given += cach != NULL;
	}
	unlock(e);
	return ok;
}

uint8_t hrc7000_bs_space(const hrc7000_dmr_t *e, int8_t ts)
{
	const hrc7000_bs_t *bs = e->bs;

	if (bs == NULL) {
		return 0;
	}
	return (uint8_t)(HRC7000_BS_Q - q_count(bs, (ts == 1) ? 0 : 1));
}

// The chip back to layer 2 (WORK_MODE through FM, passive timing, both slot syncs released), PA off. The receiver is
// restarted by the caller
void hrc7000_bs_release(hrc7000_dmr_t *e)
{
	hrc7000_bs_t *bs = e->bs;

	if (bs == NULL) {
		return;
	}
	w(e, C7_LAYER2_TXRX_CTRL, TXRX_NONE);
	pa_off(e, bs);
	w(e, C7_WORK_MODE, C7_WORK_MODE_FM);
	w(e, C7_WORK_MODE, C7_WORK_MODE_DMR);
	w(e, C7_LAYER2_CONTROL, L2_PASSIVE);
	w(e, C7_LAYER2_TXRX_CTRL, TXRX_SYNC_RELEASE | TXRX_TX_SYNC_RELEASE);
	w(e, C7_LAYER2_TXRX_CTRL, TXRX_NONE);
	e->bs = NULL;
}

void hrc7000_bs_stop(hrc7000_dmr_t *e)
{
	lock(e);
	hrc7000_bs_release(e);
	unlock(e);
	hrc7000_dmr_rx_restart(e);	// the slot engine receives again
}

bool hrc7000_bs_keyed(const hrc7000_dmr_t *e)
{
	return (e->bs != NULL) && e->bs->pa;
}

// --- ISR side ---

static void write_slot(hrc7000_dmr_t *e, hrc7000_bs_t *bs, const uint8_t *raw)
{
	uint8_t policy = bs->cfg.variant & HRC7000_BS_V_HALF_MASK;

	for (uint8_t i = 0; i < HRC7000_BS_RAW_BYTES; i++) {
		if ((policy == HRC7000_BS_V_HALF_BOTH) || (bs->half == 0)) {
			e->dev->buf[C7_TX_RAM + RAM_HALF0 + i] = raw[i];
		}
		if ((policy == HRC7000_BS_V_HALF_BOTH) || (bs->half == 1)) {
			e->dev->buf[C7_TX_RAM + RAM_HALF1 + i] = raw[i];
		}
	}
	bs->half ^= 1u;
}

// The slot after the boundary that just passed: its burst and CACH, built by the core
static void setup(hrc7000_dmr_t *e, hrc7000_bs_t *bs, int8_t ts, const dmr_tx_burst_t *b, const uint8_t *lc,
		  const dmr_cach_t *cach)
{
	int k = ts - 1;
	uint8_t raw[HRC7000_BS_RAW_BYTES];

	if ((b->kind == DMR_BURST_VOICE) && ((b->voice_index % 6u) == 0) && (lc != NULL)) {
		memcpy(bs->lc[k], lc, 9);
		bs->lc_valid[k] = true;
	}
	if (!bs->raw_slot(b, cach, ts, bs->cfg.cc, bs->lc_valid[k] ? bs->lc[k] : NULL, raw)) {
		bs->st.uncoded++;
	}
	write_slot(e, bs, raw);
	bs->st.cach_written += cach != NULL;
	w(e, C7_LAYER2_TXRX_CTRL, txrx_send(bs));
	bs->st.bursts++;
}

// A slot boundary, from whichever interrupt the physical layer gives first in a slot (the timeslot vectors, PHY_RDY)
static void boundary(hrc7000_dmr_t *e, hrc7000_bs_t *bs)
{
	uint32_t t = now(e);

	if (e->timing) {
		uint32_t gap = t - e->t_tick;

		if (gap < SLOT_US / 2u) {
			return;		// a second interrupt in the same slot
		}
		if (bs->on_air) {
			if (gap > LATE_US) {
				bs->st.late++;
			}
			if (gap > bs->st.max_gap_us) {
				bs->st.max_gap_us = gap;
			}
		}
	}
	e->tick++;
	e->st.ticks++;
	e->t_tick = t;
	e->timing = true;
	bs->st.slots++;
	// The slot set up at this boundary goes on air in the slot that starts about 5 ms later, not in the one after
	// (SDR bench 2026-10-21: the epoch was one slot early, and the RX half labeled a subscriber's TS2 call TS1)
	int8_t next = ts_of(bs, e->tick + 1u);
	bs->ts_tick = next;

	if (!bs->keyed) {
		w(e, C7_LAYER2_TXRX_CTRL, TXRX_NONE);
		if (bs->on_air) {
			bs->on_air = false;	// the slot set up last is on air now: the PA goes off after it
		} else {
			pa_off(e, bs);
		}
		return;
	}
	if (!bs->on_air && (next != 1)) {
		w(e, C7_LAYER2_TXRX_CTRL, TXRX_NONE);	// a key-up starts with an outbound TS1 slot (hal_dmr_modem.h)
		return;
	}
	bs->on_air = true;
	int k = next - 1;

	if (q_count(bs, k) > 0) {
		uint8_t at = bs->tail[k];

		setup(e, bs, next, &bs->q[k][at], bs->q_lc_valid[k][at] ? bs->q_lc[k][at] : NULL,
		      bs->q_cach_valid[k][at] ? &bs->q_cach[k][at] : NULL);
		bs->tail[k] = (uint8_t)((at + 1u) % (HRC7000_BS_Q + 1u));
	} else {
		dmr_tx_burst_t idle;

		memset(&idle, 0, sizeof(idle));
		idle.kind = DMR_BURST_IDLE;
		idle.len = sizeof(IDLE_INFO);
		memcpy(idle.payload, IDLE_INFO, sizeof(IDLE_INFO));
		setup(e, bs, next, &idle, NULL, NULL);
		bs->st.underruns++;
	}
}

void hrc7000_bs_isr_timeslot(hrc7000_dmr_t *e)
{
	hrc7000_bs_t *bs = e->bs;

	bs->st.ts_ints++;
	uint32_t before = e->tick;
	boundary(e, bs);
	bs->st.ts_ticks += e->tick != before;
}

void hrc7000_bs_isr_sys(hrc7000_dmr_t *e, uint32_t list)
{
	if ((list & SYS_PHY_RDY) != 0) {
		e->bs->st.phy_ints++;
		boundary(e, e->bs);
	}
}

void hrc7000_bs_isr_rftx(hrc7000_dmr_t *e)
{
	hrc7000_bs_t *bs = e->bs;

	bs->st.rftx_ints++;
	if (!bs->keyed || bs->pa) {
		return;		// keyed once for the whole outbound
	}
	e->ops->rf(e->ops->ctx, true);
	e->ops->pa_arm(e->ops->ctx);	// the PA one-shot keys the PA; it stays up until pa_off
	bs->pa = true;
	e->rf_tx = true;
	e->t_rf_tx = now(e);
}

void hrc7000_bs_poll(hrc7000_dmr_t *e)
{
	hrc7000_bs_t *bs = e->bs;

	// The slot clock stopped, or never started: never leave the PA up (or a key-up pending) without it
	if ((e->timing || bs->keyed || bs->pa) && ((uint32_t)(now(e) - e->t_tick) > STALL_US)) {
		e->timing = false;
		if (bs->keyed || bs->pa) {
			bs->st.stalls++;
		}
		bs->keyed = false;
		bs->on_air = false;
		pa_off(e, bs);
		w(e, C7_LAYER2_TXRX_CTRL, TXRX_NONE);
	}
}

#endif	// CONFIG_HOTSPOT_SPLIT
