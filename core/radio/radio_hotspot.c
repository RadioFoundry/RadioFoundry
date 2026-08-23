// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The radio core's side of the hotspot (see radio_hotspot.h)
#include "config.h"
#if CONFIG_HOTSPOT	// the whole file
#include <stddef.h>
#include "radio/radio_hotspot.h"

static radio_hs_post_fn post_fn;
static volatile bool hs_active;		// published: the radio is in hotspot mode
static volatile bool hs_carrier;	// published: hotspot carrier
static volatile int16_t hs_rssi;	// published: the last status poll's RSSI (dBm)
static volatile uint32_t hs_serial;
static uint32_t orphan_ms;		// hotspot mode without a running hotspot since, 0 = not

void radio_hs_init(radio_hs_post_fn post)
{
	post_fn = post;
	hs_active = false;
	hs_carrier = false;
	hs_rssi = 0;
	orphan_ms = 0;
}

static void post(uint8_t type, int32_t arg, uint32_t arg2, bool must)
{
	radio_req_t req = { .type = type, .arg = arg, .arg2 = arg2 };

	if (post_fn != NULL) {
		post_fn(&req, must);
	}
}

void radio_hs_publish(const radio_sm_t *sm)
{
	hs_active = sm->hotspot;
#if CONFIG_DMR
	hs_carrier = sm->hs_carrier;
#endif
	hs_rssi = sm->view.rssi_dbm;
}

void radio_hs_watch(radio_sm_t *sm, uint32_t now_ms)
{
	if (!sm->hotspot || hotspot_running()) {
		orphan_ms = 0;
		return;
	}
	if (orphan_ms == 0) {
		orphan_ms = (now_ms != 0) ? now_ms : 1;
	} else if ((now_ms - orphan_ms) >= RADIO_HS_ORPHAN_MS) {
		radio_req_t req = { .type = RREQ_HS_LEAVE };

		radio_sm_request(sm, &req, now_ms);
		orphan_ms = 0;
	}
}

uint32_t radio_hs_serial(void)
{
	return hs_serial;
}

// --- hotspot_radio_ops_t, called from the host link task. None of them waits ---

// The request runs in the radio task, which is above the host link task: it has normally been handled by the time
// the post returns. If not, false now and the hotspot asks again later (a repeat while in hotspot mode is a no-op)
static bool hs_enter(uint32_t rx_hz)
{
	if (hs_active) {
		return true;
	}
	post(RREQ_HS_ENTER, (int32_t)rx_hz, 0, false);
	return hs_active;
}

static bool hs_set_freq(uint32_t rx_hz, uint32_t tx_hz)
{
	if (!radio_sm_hotspot_freq_ok(rx_hz, tx_hz)) {
		return false;
	}
	post(RREQ_HS_FREQ, (int32_t)rx_hz, tx_hz, false);
	return true;
}

static void hs_set_power(uint8_t rf_level)
{
	post(RREQ_HS_POWER, rf_level, 0, false);
}

static void hs_leave(void)
{
	post(RREQ_HS_LEAVE, 0, 0, true);	// must not be lost: the channel comes back with it
}

static bool hs_carrier_fn(void)
{
	return hs_carrier;
}

static int16_t hs_rssi_fn(void)
{
	return hs_active ? hs_rssi : 0;
}

static void hs_changed(void)
{
	hs_serial++;
}

const hotspot_radio_ops_t radio_hs_ops = {
	.enter = hs_enter, .set_freq = hs_set_freq, .set_power = hs_set_power, .leave = hs_leave,
	.carrier = hs_carrier_fn, .changed = hs_changed, .rssi_dbm = hs_rssi_fn,
	.cw_id = NULL, .cw_busy = NULL,	// no FM Morse keyer yet: the hotspot ACKs SEND_CWID and counts it unsent
};

#endif	// CONFIG_HOTSPOT
