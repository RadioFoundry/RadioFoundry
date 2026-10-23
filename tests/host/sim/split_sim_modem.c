// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// A simulated split duplex modem for host tests (see split_sim_modem.h)
#include <string.h>
#include "split_sim_modem.h"
#include "dmr/bsOutbound.h"

ssim_t ssim;

#define RX_COUNTER_BASE	100u

static bool start(const dmr_config_t *cfg, dmr_event_cb_t cb, void *ctx)
{
	ssim.started = true;
	ssim.cfg = *cfg;
	ssim.cb = cb;
	ssim.ctx = ctx;
	return true;
}

static void stop(void)
{
	ssim.started = false;
	ssim.bs = false;
	ssim.keyed = false;
}

static void configure(const dmr_config_t *cfg) { ssim.cfg = *cfg; }
static void rx_restart(void) { ssim.rx_restarts++; }
static void rx_lock_slot(int8_t ts) { ssim.locked_ts = ts; }
static bool tx_begin(dmr_timing_t timing, int8_t ts, dmr_access_t access) { return false; }
static bool tx_queue(const dmr_tx_burst_t *b) { return false; }
static uint8_t tx_queue_space(void) { return 0; }
static void tx_end(void) {}
static void tx_abort(void) {}
static void set_embedded_lc(const uint8_t lc[9]) {}
static void set_cc_filter(int8_t cc) {}
static bool is_transmitting(void) { return ssim.keyed; }

static uint32_t now_us(void) { return ssim.now_us; }

static bool slot_clock(uint32_t *counter, uint32_t *t_us, int8_t *ts)
{
	if (ssim.bs) {
		if ((ssim.counter == 0) || ssim.restarting) {
			return false;
		}
		*counter = ssim.counter;
		*t_us = ssim.t_tick;
		*ts = ssim.ts_tick;
		return true;
	}
	if (!ssim.rx_clock) {
		return false;
	}
	int32_t d = (int32_t)(ssim.now_us - ssim.rx_origin);
	int32_t slots = (d >= 0) ? d / 30000 : -((-d + 29999) / 30000);

	*counter = RX_COUNTER_BASE + (uint32_t)slots;
	*t_us = ssim.rx_origin + (uint32_t)(slots * 30000);
	*ts = 0;
	return true;
}

static bool bs_start(const dmr_bs_config_t *cfg)
{
	if (!ssim.started) {
		return false;
	}
	ssim.bs = true;
	ssim.keyed = false;
	ssim.bs_cfg = *cfg;
	ssim.bs_starts++;
	ssim.counter = 0;
	ssim.ts1_parity = 1;				// boundary 1 starts TS1
	ssim.next_tick_us = ssim.now_us + 1000u;	// the first boundary 1 ms after the start
	memset(ssim.qn, 0, sizeof(ssim.qn));
	return true;
}

static void bs_key(bool on)
{
	if (on && !ssim.keyed) {
		ssim.keyups++;
	}
	ssim.keyed = on;
	if (!on) {
		memset(ssim.qn, 0, sizeof(ssim.qn));
	}
}

static bool bs_queue(int8_t ts, const dmr_tx_burst_t *b, const dmr_cach_t *cach, const uint8_t *emb_lc)
{
	int k = (ts == 1) ? 0 : 1;

	if (!ssim.bs || (ssim.qn[k] >= SSIM_Q)) {
		return false;
	}
	ssim_air_t *a = &ssim.q[k][ssim.qn[k]++];

	memset(a, 0, sizeof(*a));
	a->ts = ts;
	a->b = *b;
	a->cach = *cach;
	a->emb_lc_valid = emb_lc != NULL;
	if (emb_lc != NULL) {
		memcpy(a->emb_lc, emb_lc, 9);
	}
	return true;
}

static bool bs_queue_at(uint32_t counter, int8_t ts, const dmr_tx_burst_t *b, const dmr_cach_t *cach,
			const uint8_t *emb_lc)
{
	int k = (ts == 1) ? 0 : 1;

	if (!bs_queue(ts, b, cach, emb_lc)) {
		return false;
	}
	ssim.q[k][ssim.qn[k] - 1u].at = true;
	ssim.q[k][ssim.qn[k] - 1u].at_counter = counter;
	return true;
}

static bool bs_restart(uint32_t t_us, int8_t ts)
{
	if (!ssim.bs || ssim.keyed) {
		return false;
	}
	int32_t d = (int32_t)(t_us - ssim.now_us) % (int32_t)SSIM_SLOT_US;	// the next t_us + k x 30 ms from now
	uint32_t ahead = (uint32_t)((d < 0) ? d + (int32_t)SSIM_SLOT_US : d);

	if (ahead < 2000u) {
		ahead += SSIM_SLOT_US;
	}
	ssim.next_tick_us = ssim.now_us + ahead + (uint32_t)ssim.restart_err_us;
	if (ssim.restart_flip) {
		ssim.restart_err_us = -ssim.restart_err_us;
	}
	ssim.ts1_parity = (uint8_t)(((ssim.counter + 1u) & 1u) ^ ((ts == 1) ? 0u : 1u));
	ssim.on_air = false;
	ssim.restarting = true;
	ssim.restarts++;
	ssim.slew_pending = 0;
	return true;
}

// One slot from the next boundary longer by the request, in whole steps of SSIM_SLEW_STEP, at most SSIM_SLEW_MAX
static int32_t bs_slew(int32_t us)
{
	if (!ssim.bs || (ssim.slew_pending != 0)) {
		return 0;
	}
	int32_t n = (us >= 0) ? (us + SSIM_SLEW_STEP / 2) / SSIM_SLEW_STEP : -((-us + SSIM_SLEW_STEP / 2) / SSIM_SLEW_STEP);
	int32_t v = n * SSIM_SLEW_STEP;

	v = (v > SSIM_SLEW_MAX) ? SSIM_SLEW_MAX : ((v < -SSIM_SLEW_MAX) ? -SSIM_SLEW_MAX : v);
	ssim.slew_pending = v;
	ssim.slew_calls++;
	return v;
}

static uint8_t bs_space(int8_t ts)
{
	return (uint8_t)(SSIM_Q - ssim.qn[(ts == 1) ? 0 : 1]);
}

static void bs_stop(void)
{
	ssim.bs = false;
	ssim.keyed = false;
	ssim.counter = 0;
	ssim.bs_stops++;
	memset(ssim.qn, 0, sizeof(ssim.qn));
}

static bool bs_keyed(void) { return ssim.keyed; }

const hal_dmr_modem_t ssim_modem = {
	.start = start, .stop = stop, .configure = configure, .rx_restart = rx_restart, .rx_lock_slot = rx_lock_slot,
	.tx_begin = tx_begin, .tx_queue = tx_queue, .tx_queue_space = tx_queue_space, .tx_end = tx_end,
	.tx_abort = tx_abort, .set_embedded_lc = set_embedded_lc, .set_cc_filter = set_cc_filter,
	.is_transmitting = is_transmitting,
	.now_us = now_us, .slot_clock = slot_clock, .bs_start = bs_start, .bs_key = bs_key, .bs_queue = bs_queue,
	.bs_space = bs_space, .bs_stop = bs_stop, .bs_keyed = bs_keyed, .bs_cach = DMR_BS_CACH_OURS,
	.bs_queue_at = bs_queue_at, .bs_restart = bs_restart, .bs_air_delay_us = SSIM_AIR_DELAY,
	.bs_slew = bs_slew, .bs_slew_step_us = SSIM_SLEW_STEP,
};

// Without slewing (the HR_C6000's drivers)
const hal_dmr_modem_t ssim_modem_noslew = {
	.start = start, .stop = stop, .configure = configure, .rx_restart = rx_restart, .rx_lock_slot = rx_lock_slot,
	.tx_begin = tx_begin, .tx_queue = tx_queue, .tx_queue_space = tx_queue_space, .tx_end = tx_end,
	.tx_abort = tx_abort, .set_embedded_lc = set_embedded_lc, .set_cc_filter = set_cc_filter,
	.is_transmitting = is_transmitting,
	.now_us = now_us, .slot_clock = slot_clock, .bs_start = bs_start, .bs_key = bs_key, .bs_queue = bs_queue,
	.bs_space = bs_space, .bs_stop = bs_stop, .bs_keyed = bs_keyed, .bs_cach = DMR_BS_CACH_OURS,
	.bs_queue_at = bs_queue_at, .bs_restart = bs_restart, .bs_air_delay_us = SSIM_AIR_DELAY,
};

const hal_dmr_modem_t ssim_modem_plain = {
	.start = start, .stop = stop, .configure = configure, .rx_restart = rx_restart, .rx_lock_slot = rx_lock_slot,
	.tx_begin = tx_begin, .tx_queue = tx_queue, .tx_queue_space = tx_queue_space, .tx_end = tx_end,
	.tx_abort = tx_abort, .set_embedded_lc = set_embedded_lc, .set_cc_filter = set_cc_filter,
	.is_transmitting = is_transmitting,
};

void ssim_reset(void)
{
	memset(&ssim, 0, sizeof(ssim));
	ssim.now_us = 5000000u;
}

// One outbound slot boundary: the slot starting now gets the head of its queue (or the driver's idle repeat)
static void tick(uint32_t t)
{
	ssim.counter++;
	ssim.t_tick = t;
	ssim.restarting = false;
	ssim.slew_now = ssim.slew_pending;	// the slot starting here takes the pending change
	ssim.slew_pending = 0;
	if (ssim.slew_now != 0) {
		ssim.slew_slots++;
		ssim.slew_largest = (ssim.slew_now > ssim.slew_largest) ? ssim.slew_now :
				    ((-ssim.slew_now > ssim.slew_largest) ? -ssim.slew_now : ssim.slew_largest);
	}
	ssim.ts_tick = (((ssim.counter ^ ssim.ts1_parity) & 1u) == 0) ? 1 : 2;
	if (!ssim.keyed) {
		ssim.on_air = false;
		return;
	}
	if (!ssim.on_air && (ssim.ts_tick != 1)) {
		return;		// a key-up starts with an outbound TS1 slot (hal_dmr_modem.h bs_key)
	}
	ssim.on_air = true;
	int k = ssim.ts_tick - 1;
	ssim_air_t a;

	// Bursts for a numbered slot that has passed are dropped; one for a later slot waits (idle now)
	while ((ssim.qn[k] > 0) && ssim.q[k][0].at && ((int32_t)(ssim.q[k][0].at_counter - ssim.counter) < 0)) {
		memmove(&ssim.q[k][0], &ssim.q[k][1], sizeof(ssim_air_t) * (SSIM_Q - 1u));
		ssim.qn[k]--;
		ssim.late++;
	}
	if ((ssim.qn[k] > 0) && (!ssim.q[k][0].at || (ssim.q[k][0].at_counter == ssim.counter))) {
		a = ssim.q[k][0];
		memmove(&ssim.q[k][0], &ssim.q[k][1], sizeof(ssim_air_t) * (SSIM_Q - 1u));
		ssim.qn[k]--;
	} else {
		memset(&a, 0, sizeof(a));
		a.b.kind = DMR_BURST_IDLE;
		a.underrun = true;
		ssim.underruns++;
	}
	a.t_us = t;
	a.counter = ssim.counter;
	a.ts = ssim.ts_tick;
	if (ssim.air_n < SSIM_AIR_MAX) {
		ssim.air[ssim.air_n++] = a;
	}
}

void ssim_advance_to(uint32_t t_us)
{
	while (ssim.bs && ((int32_t)(t_us - ssim.next_tick_us) >= 0)) {
		ssim.now_us = ssim.next_tick_us;
		tick(ssim.next_tick_us);
		ssim.next_tick_us += (uint32_t)((int32_t)SSIM_SLOT_US + ssim.slew_now);
		ssim.slew_now = 0;
	}
	ssim.now_us = t_us;
}

// The RX half's chip timing: boundaries every 30 ms from the first burst's start (passive timing locked to it)

void ssim_rx_burst(const dmr_burst_t *b, uint32_t t_start_us)
{
	dmr_event_t ev = { .kind = DMR_EV_BURST_RX, .ts = 0, .burst = b };

	if (!ssim.rx_clock) {
		ssim.rx_clock = true;
		ssim.rx_origin = t_start_us;
	}
	int32_t d = (int32_t)(t_start_us - ssim.rx_origin);
	int32_t slots = (d >= 0) ? (d + 15000) / 30000 : -((-d + 15000) / 30000);
	int32_t off = d - slots * 30000;

	if ((off > 500) || (off < -500)) {
		ssim.rx_origin = t_start_us;	// another signal: the chip's timing locks to it
		slots = 0;
	}

	ev.arg = RX_COUNTER_BASE + (uint32_t)slots + 1u;	// reported at the next boundary
	if (ssim.cb != NULL) {
		ssim.cb(&ev, ssim.ctx);
	}
}
