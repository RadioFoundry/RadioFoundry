// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Simulated DMR modem for the host tests (see dmr_sim_modem.h)
#include <string.h>
#include "dmr_sim_modem.h"
#include "dmr/call/dmr_lc.h"

sim_modem_t sim;

static void emit(uint8_t kind, int8_t ts, uint32_t arg, const dmr_burst_t *b)
{
	dmr_event_t ev = { .kind = kind, .ts = ts, .arg = arg, .burst = b };

	if (sim.cb)
		sim.cb(&ev, sim.ctx);
}

static bool start(const dmr_config_t *cfg, dmr_event_cb_t cb, void *ctx)
{
	if (sim.refuse_start)
		return false;
	sim.started = true;
	sim.cfg = *cfg;
	sim.cb = cb;
	sim.ctx = ctx;
	return true;
}

static void stop(void) { sim.started = false; sim.cb = NULL; }
static void configure(const dmr_config_t *cfg) { sim.cfg = *cfg; }
static void rx_restart(void) {}
static void rx_lock_slot(int8_t ts) { sim.locked_ts = ts; }

static bool tx_begin(dmr_timing_t t, int8_t ts, dmr_access_t a)
{
	if (sim.tx_begins++ == 0) {
		sim.first_timing = t;
		sim.first_ts = ts;
	}
	sim.last_timing = t;
	sim.last_ts = ts;
	sim.last_access = a;
	if (sim.refuse_tx)
		return false;
	sim.tx_active = true;
	sim.tx_ending = false;
	sim.q_head = sim.q_count = 0;
	sim.tx_parity = (int8_t)((sim.slot + 1) & 1);	// first burst in the next slot
	return true;
}

static bool tx_queue(const dmr_tx_burst_t *b)
{
	if (!sim.tx_active || sim.q_count >= SIM_QUEUE_DEPTH)
		return false;
	sim.q[(sim.q_head + sim.q_count) % SIM_QUEUE_DEPTH] = *b;
	sim.q_count++;
	return true;
}

static int8_t tx_fill(const dmr_tx_burst_t *b)
{
	for (uint8_t i = 0; i < sim.q_count; i++) {
		dmr_tx_burst_t *q = &sim.q[(sim.q_head + i) % SIM_QUEUE_DEPTH];
		if (q->kind == DMR_BURST_EMPTY) {
			*q = *b;
			sim.fills++;
			return (int8_t)(sim.q_count - 1 - i);
		}
	}
	return -1;
}

static uint8_t tx_queue_space(void) { return (uint8_t)(SIM_QUEUE_DEPTH - sim.q_count); }
static void tx_end(void) { sim.tx_ends++; sim.tx_ending = true; }

static void tx_abort(void)
{
	sim.tx_aborts++;
	sim.tx_active = false;
	sim.q_count = 0;
}

static void set_embedded_lc(const uint8_t lc[9])
{
	memcpy(sim.pending_emb_lc, lc, 9);
	sim.pending_emb = true;
	memcpy(sim.emb_lc, lc, 9);
	sim.emb_lc_set = true;
}

static void tx_listen(void)
{
	if (sim.tx_active)
		sim.tx_listens++;
}

static void set_cc_filter(int8_t cc) { sim.cc_filter = cc; }
static bool is_transmitting(void) { return sim.tx_active; }

const hal_dmr_modem_t sim_modem = {
	.start = start, .stop = stop, .configure = configure, .rx_restart = rx_restart, .rx_lock_slot = rx_lock_slot,
	.tx_begin = tx_begin, .tx_queue = tx_queue, .tx_queue_space = tx_queue_space, .tx_end = tx_end,
	.tx_abort = tx_abort, .set_embedded_lc = set_embedded_lc, .set_cc_filter = set_cc_filter,
	.is_transmitting = is_transmitting, .tx_listen = tx_listen, .tx_fill = tx_fill,
};

void sim_reset(void)
{
	memset(&sim, 0, sizeof(sim));
}

void sim_rx(const dmr_burst_t *b)
{
	emit(DMR_EV_BURST_RX, b->ts, 0, b);
}

void sim_event(uint8_t kind, int8_t ts)
{
	emit(kind, ts, 0, NULL);
}

static void send_one(void)
{
	dmr_tx_burst_t b = sim.q[sim.q_head];

	sim.q_head = (uint8_t)((sim.q_head + 1) % SIM_QUEUE_DEPTH);
	sim.q_count--;
	emit(DMR_EV_TX_SLOT_STARTED, 0, sim.slot, NULL);
	if (b.kind == DMR_BURST_EMPTY) {
		sim.empty_slots++;	// nothing on air: no air[] entry
		return;
	}
	sim_air_t *a = sim.air_count < SIM_AIR_MAX ? &sim.air[sim.air_count++] : NULL;
	if (b.kind == DMR_BURST_VOICE && b.voice_index == 0 && sim.pending_emb) {
		memcpy(sim.cur_emb_lc, sim.pending_emb_lc, 9);	// new embedded LC from this superframe
		sim.pending_emb = false;
	}
	if (a) {
		memset(a, 0, sizeof(*a));
		a->kind = b.kind;
		a->voice_index = b.voice_index;
		a->host_crc = b.host_crc;
		a->sync = b.sync_override;
		a->len = b.len;
		memcpy(a->payload, b.payload, b.len);
		a->slot = sim.slot;
		if (b.kind == DMR_BURST_VOICE) {
			a->lcss = dmr_emb_lcss(b.voice_index);
			if (b.voice_index >= 1 && b.voice_index <= 4) {
				uint8_t frag[4][4];

				dmr_emb_lc_encode(sim.cur_emb_lc, frag);
				memcpy(a->emb, frag[b.voice_index - 1], 4);
			}
		}
	}
	emit(DMR_EV_TX_SLOT_ENDED, 0, sim.slot, NULL);
	if (b.kind == DMR_BURST_CSBK && b.payload[0] == 0xB8 && sim.wake_repeater_on_csbk)
		sim.repeater = true;
}

void sim_slot(void)
{
	if (sim.stalled)
		return;
	sim.slot++;
	int8_t ts = sim.repeater ? (int8_t)(1 + (sim.slot & 1)) : 0;

	emit(DMR_EV_SLOT_TICK, ts, sim.slot, NULL);
	if (!sim.tx_active || (int8_t)(sim.slot & 1) != sim.tx_parity)
		return;
	if (sim.reject_next) {
		sim.reject_next = false;
		sim.tx_active = false;
		sim.q_count = 0;
		emit(DMR_EV_TX_REJECTED, 0, 0, NULL);
		return;
	}
	if (sim.q_count) {
		send_one();
		if (sim.q_count == 0 && sim.tx_ending) {
			sim.tx_active = false;
			emit(DMR_EV_TX_DONE, 0, sim.slot, NULL);
		}
	} else if (sim.tx_ending) {
		sim.tx_active = false;
		emit(DMR_EV_TX_DONE, 0, sim.slot, NULL);
	} else {
		sim.underflows++;	// a real modem would send silence here
	}
}
