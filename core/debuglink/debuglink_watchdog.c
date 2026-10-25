// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The hardware watchdog on the debug link (CONFIG_WATCHDOG, docs/spec/debuglink.md 3.21, docs/spec/watchdog.md): the
// watchdog view section, and DL_CMD_WATCHDOG's bench tests, which stop the feeds in one of three ways (or re-arm with
// a raw load first, to measure an undocumented counter clock) so the reset can be seen and timed. A test starts after
// the reply has gone out, as the crash test does, and never while the radio transmits.
#include "config.h"
#include "debuglink/debuglink.h"
#include "debuglink/debuglink_proto.h"

#if CONFIG_WATCHDOG
#include "hal_time.h"
#include "hal_watchdog.h"
#include "app/watchdog.h"
#include "debuglink/dlog.h"
#include "radio/radio.h"
#include "FreeRTOS.h"
#include "task.h"

#define WDT_TEST_DELAY_MS	100u	// let the reply leave first

static uint8_t test_pending;		// DL_WDT_TEST_* to start after the reply, 0 none
static bool load_pending;
static uint32_t load_arg[3];

void debuglink_view_watchdog(dl_text_t *t)
{
	const watchdog_state_t *s = watchdog_state();
	uint32_t regs[HAL_WATCHDOG_REGS];
	uint8_t n = hal_watchdog_regs(regs);
	dl_text(t, "watchdog.built=1\nwatchdog.timeout_ms=%u\nwatchdog.feeds=%u\nwatchdog.starving=%u\n",
		(unsigned)hal_watchdog_timeout_ms(), (unsigned)s->feeds, s->starving);
	dl_text(t, "watchdog.idle_age_ms=%u\nwatchdog.max_gap_ms=%u\n",
		s->seen ? (unsigned)(hal_millis() - s->last_idle_ms) : 0u, (unsigned)s->max_gap_ms);
	if (s->starving) {
		// Polled until the link goes quiet: the last answer bounds the timeout from below
		dl_text(t, "watchdog.starved_ms=%u\n", (unsigned)(hal_millis() - s->starve_ms));
	}
	dl_text(t, "watchdog.regs=");
	for (uint8_t i = 0; i < n; i++) {
		dl_text(t, i ? ",0x%08x" : "0x%08x", (unsigned)regs[i]);
	}
	dl_text(t, "\n");
}

static bool on_air(void)
{
	radio_view_t v;
	radio_get_view(&v);
	return v.state == RS_TX;
}

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

size_t debuglink_cmd_watchdog(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap)
{
	(void)cap;
	if (len < 1) {
		out[0] = DL_ST_BAD_ARG;
		return 1;
	}
	if (req[0] == DL_WDT_OP_CLEAR_GAP && len == 1) {
		watchdog_clear_gap();
		out[0] = DL_ST_OK;
		return 1;
	}
	if (req[0] == DL_WDT_OP_TEST && len == 6) {
		if (req[1] < DL_WDT_TEST_STARVE || req[1] > DL_WDT_TEST_IRQ_OFF) {
			out[0] = DL_ST_BAD_ARG;
			return 1;
		}
		if (get32(req + 2) != DL_WDT_TOKEN || on_air()) {
			out[0] = DL_ST_REFUSED;		// a stray frame, or the transmitter is keyed
			return 1;
		}
		dlog("debuglink: watchdog test %u in %u ms", req[1], (unsigned)WDT_TEST_DELAY_MS);
		test_pending = req[1];
		out[0] = DL_ST_OK;
		return 1;
	}
	if (req[0] == DL_WDT_OP_LOAD && len == 17) {
		if (!hal_watchdog_test_load(NULL)) {
			out[0] = DL_ST_UNAVAILABLE;
			return 1;
		}
		if (get32(req + 13) != DL_WDT_TOKEN || on_air()) {
			out[0] = DL_ST_REFUSED;
			return 1;
		}
		for (int i = 0; i < 3; i++) {
			load_arg[i] = get32(req + 1 + 4 * i);
		}
		dlog("debuglink: watchdog load 0x%x 0x%x 0x%x in %u ms", (unsigned)load_arg[0], (unsigned)load_arg[1],
		     (unsigned)load_arg[2], (unsigned)WDT_TEST_DELAY_MS);
		load_pending = true;
		out[0] = DL_ST_OK;
		return 1;
	}
	out[0] = DL_ST_BAD_ARG;
	return 1;
}

void debuglink_watchdog_service(void)
{
	if (test_pending == 0 && !load_pending) {
		return;
	}
	uint8_t kind = load_pending ? DL_WDT_TEST_STARVE : test_pending;
	bool load = load_pending;
	test_pending = 0;
	load_pending = false;
	hal_delay_ms(WDT_TEST_DELAY_MS);
	if (on_air()) {
		return;		// a PTT came in meanwhile
	}
	if (load) {
		if (!hal_watchdog_test_load(load_arg)) {
			dlog("debuglink: watchdog load: not on this platform");
			return;
		}
		dlog("debuglink: watchdog re-armed at t=%u ms, feeds stop", (unsigned)hal_millis());
	}
	watchdog_starve(hal_millis());
	if (kind == DL_WDT_TEST_SPIN) {
		for (;;) {
		}
	}
	if (kind == DL_WDT_TEST_IRQ_OFF) {
		taskDISABLE_INTERRUPTS();
		for (;;) {
		}
	}
}

#else

void debuglink_view_watchdog(dl_text_t *t)
{
	dl_text(t, "watchdog.built=0\n");
}

size_t debuglink_cmd_watchdog(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap)
{
	(void)req;
	(void)len;
	(void)cap;
	out[0] = DL_ST_UNAVAILABLE;
	return 1;
}

void debuglink_watchdog_service(void)
{
}

#endif
