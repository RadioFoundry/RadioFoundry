// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The crash log on the debug link (CONFIG_CRASH_LOG, docs/spec/debuglink.md 3.19, docs/spec/crash-log.md): DL_CMD_CRASH
// to clear the record or to crash on purpose (a HIL test of the capture). The crash comes after the reply has gone
// out, as cmd_reboot's reset does. Without the crash log the command answers unavailable. The crash and reset view
// sections are dl_crash_view.c's.
#include "config.h"
#include "debuglink/debuglink.h"
#include "debuglink/debuglink_proto.h"

#if CONFIG_CRASH_LOG
#include "hal_time.h"
#include "crash/crash.h"
#include "debuglink/dlog.h"
#include "radio/radio.h"

#define CRASH_TEST_DELAY_MS	100u	// let the reply leave before the radio goes down

static uint8_t test_pending;		// DL_CRASH_TEST_* to run after the reply, 0 none

static bool on_air(void)
{
	radio_view_t v;
	radio_get_view(&v);
	return v.state == RS_TX;
}

size_t debuglink_cmd_crash(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap)
{
	(void)cap;
	if (len < 1) {
		out[0] = DL_ST_BAD_ARG;
		return 1;
	}
	if (req[0] == DL_CRASH_OP_CLEAR && len == 1) {
		crash_clear();
		out[0] = DL_ST_OK;
		return 1;
	}
	if (req[0] == DL_CRASH_OP_TEST && len == 6) {
		uint32_t token = (uint32_t)req[2] | (uint32_t)req[3] << 8 | (uint32_t)req[4] << 16 | (uint32_t)req[5] << 24;
		if (req[1] < DL_CRASH_TEST_TRAP || req[1] > DL_CRASH_TEST_HANG) {
			out[0] = DL_ST_BAD_ARG;
			return 1;
		}
		if (req[1] == DL_CRASH_TEST_HANG && !CONFIG_WATCHDOG) {
			out[0] = DL_ST_UNAVAILABLE;	// nothing would end the spin but a power cycle
			return 1;
		}
		if (token != DL_CRASH_TOKEN || on_air()) {
			out[0] = DL_ST_REFUSED;	// a stray frame, or the transmitter is keyed
			return 1;
		}
		dlog("debuglink: crash test %u in %u ms", req[1], (unsigned)CRASH_TEST_DELAY_MS);
		test_pending = req[1];
		out[0] = DL_ST_OK;
		return 1;
	}
	out[0] = DL_ST_BAD_ARG;
	return 1;
}

void debuglink_crash_service(void)
{
	if (test_pending == 0)
		return;
	uint8_t kind = test_pending;
	test_pending = 0;
	hal_delay_ms(CRASH_TEST_DELAY_MS);
	if (on_air())
		return;		// a PTT came in meanwhile
	if (kind == DL_CRASH_TEST_TRAP)
		__builtin_trap();	// an undefined instruction (CM4) / a breakpoint (CK803): a CPU exception
	if (kind == DL_CRASH_TEST_HANG) {
		for (;;) {	// the idle task stops running: the tick's hang check ends it
		}
	}
	crash_assert(__LINE__);
}

#else

size_t debuglink_cmd_crash(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap)
{
	(void)req;
	(void)len;
	(void)cap;
	out[0] = DL_ST_UNAVAILABLE;
	return 1;
}

void debuglink_crash_service(void)
{
}

#endif
