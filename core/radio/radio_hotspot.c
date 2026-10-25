// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The radio core's side of the hotspot (see radio_hotspot.h)
#include "config.h"
#if CONFIG_HOTSPOT	// the whole file
#include <stddef.h>
#include <string.h>
#include "radio/radio_hotspot.h"
#include "radio/morse.h"
#if CONFIG_SFR
#include "hotspot/hotspotSfr.h"
#endif

static radio_hs_post_fn post_fn;
static volatile bool hs_active;		// published: the radio is in hotspot mode
static volatile bool hs_carrier;	// published: hotspot carrier
static volatile int16_t hs_rssi;	// published: the last status poll's RSSI (dBm)
static volatile uint32_t hs_serial;
static uint32_t orphan_ms;		// hotspot mode without a running hotspot since, 0 = not
// CW ID handover: written by the cw_id op (host link task) only while cw_busy is false, read by the radio task
static char cw_text[MORSE_TEXT_MAX];
static volatile uint8_t cw_len;
static volatile bool cw_pending;	// from the op until the radio task has ended (or refused) the ID
static radio_hs_cw_stats_t cw_stats;

void radio_hs_init(radio_hs_post_fn post)
{
	post_fn = post;
	hs_active = false;
	hs_carrier = false;
	hs_rssi = 0;
	orphan_ms = 0;
	cw_len = 0;
	cw_pending = false;
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
#if CONFIG_SFR
	// The SFR mode's idle beacon follows the settings (rpt_beacon, on the roaming beacon interval and duration)
	hotspot_sfr_set_beacon(sm->set.rpt_beacon ? sm->set.roam_beacon_s : 0, sm->set.roam_beacon_len_s);
#endif
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

// SEND_CWID: the radio task keys it (FM, the board's TX tone); refused at once without hotspot mode
static bool hs_cw_id(const char *callsign, uint8_t len)
{
	if (!hs_active || cw_pending || len == 0) {
		return false;
	}
	if (len > MORSE_TEXT_MAX) {
		len = MORSE_TEXT_MAX;
	}
	memcpy(cw_text, callsign, len);
	cw_len = len;
	cw_pending = true;
	cw_stats.requests++;
	post(RREQ_HS_CWID, 0, 0, true);
	return true;
}

static bool hs_cw_busy(void)
{
	return cw_pending;
}

uint8_t radio_hs_cw_take(char *out, uint8_t cap)
{
	uint8_t n = cw_len;
	if (!cw_pending || n == 0) {
		return 0;
	}
	if (n > cap) {
		n = cap;
	}
	memcpy(out, cw_text, n);
	cw_len = 0;
	return n;
}

void radio_hs_cw_started(uint8_t wpm)
{
	cw_stats.wpm = wpm;
	cw_stats.active = true;
}

void radio_hs_cw_ended(bool sent, uint32_t ms)
{
	if (sent) {
		cw_stats.sent++;
		cw_stats.last_ms = ms;
	} else {
		cw_stats.refused++;
	}
	cw_stats.active = false;
	cw_len = 0;
	cw_pending = false;
}

void radio_hs_cw_get_stats(radio_hs_cw_stats_t *out)
{
	*out = cw_stats;
	out->active = cw_pending;
}

const hotspot_radio_ops_t radio_hs_ops = {
	.enter = hs_enter, .set_freq = hs_set_freq, .set_power = hs_set_power, .leave = hs_leave,
	.carrier = hs_carrier_fn, .changed = hs_changed, .rssi_dbm = hs_rssi_fn,
	.cw_id = hs_cw_id, .cw_busy = hs_cw_busy,	// keyed by the radio task (radio_sm.c cw_*), on boards with tx_tone
};

#endif	// CONFIG_HOTSPOT
