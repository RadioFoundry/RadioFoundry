// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// DM-1701 DMR modem (HR_C6000 in DMR mode, its interrupts, per-burst RF switching): STUB, to be replaced
#include "dm1701.h"

static bool start(const dmr_config_t *cfg, dmr_event_cb_t cb, void *ctx) { (void)cfg; (void)cb; (void)ctx; return false; }
static void stop(void) {}
static void configure(const dmr_config_t *cfg) { (void)cfg; }
static void rx_restart(void) {}
static void rx_lock_slot(int8_t ts) { (void)ts; }
static bool tx_begin(dmr_timing_t t, int8_t ts, dmr_access_t a) { (void)t; (void)ts; (void)a; return false; }
static bool tx_queue(const dmr_tx_burst_t *b) { (void)b; return false; }
static uint8_t tx_queue_space(void) { return 0; }
static void tx_end(void) {}
static void tx_abort(void) {}
static void set_embedded_lc(const uint8_t lc[9]) { (void)lc; }
static void set_cc_filter(int8_t cc) { (void)cc; }
static bool is_transmitting(void) { return false; }

const hal_dmr_modem_t dm1701_dmr = {
	.start = start, .stop = stop, .configure = configure, .rx_restart = rx_restart, .rx_lock_slot = rx_lock_slot,
	.tx_begin = tx_begin, .tx_queue = tx_queue, .tx_queue_space = tx_queue_space, .tx_end = tx_end,
	.tx_abort = tx_abort, .set_embedded_lc = set_embedded_lc, .set_cc_filter = set_cc_filter,
	.is_transmitting = is_transmitting,
};
