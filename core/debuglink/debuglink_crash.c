// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The crash log on the debug link (CONFIG_CRASH_LOG, docs/spec/debuglink.md 3.19, docs/spec/crash-log.md): the crash
// view section with the record kept from before this start, and DL_CMD_CRASH to clear it or to crash on purpose (a
// HIL test of the capture). The crash comes after the reply has gone out, as cmd_reboot's reset does. Without the
// crash log the section says "crash.built=0" and the command answers unavailable.
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

void debuglink_view_crash(dl_text_t *t)
{
	const crash_record_t *r = crash_last();
	dl_text(t, "crash.built=1\ncrash.valid=%u\n", r != NULL);
	if (r == NULL)
		return;
	char task[CRASH_TASK_LEN + 1];
	for (int k = 0; k < CRASH_TASK_LEN; k++)
		task[k] = r->task[k];
	task[CRASH_TASK_LEN] = 0;
	dl_text(t, "crash.arch=%u\ncrash.kind=%u\ncrash.cause=%u\ncrash.flags=0x%x\ncrash.count=%u\n", r->arch, r->kind,
		(unsigned)r->cause, r->flags, r->count);
	dl_text(t, "crash.pc=0x%08x\ncrash.lr=0x%08x\ncrash.sp=0x%08x\ncrash.psr=0x%08x\n", (unsigned)r->pc,
		(unsigned)r->lr, (unsigned)r->sp, (unsigned)r->psr);
	dl_text(t, "crash.status=0x%08x,0x%08x,0x%08x,0x%08x\n", (unsigned)r->status[0], (unsigned)r->status[1],
		(unsigned)r->status[2], (unsigned)r->status[3]);
	dl_text(t, "crash.task=%s\ncrash.uptime_ms=%u\ncrash.build=%08x\n", task, (unsigned)r->uptime_ms,
		(unsigned)r->build);
	for (int k = 0; k < 14; k++)
		dl_text(t, "crash.r%d=0x%08x\n", k, (unsigned)r->r[k]);
	if (!(r->flags & CRASH_F_STACK))
		return;
	dl_text(t, "crash.stack=");
	for (int k = 0; k < CRASH_STACK_WORDS; k++)
		dl_text(t, k ? ",%08x" : "%08x", (unsigned)r->stack[k]);
	dl_text(t, "\n");
}

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

void debuglink_view_crash(dl_text_t *t)
{
	dl_text(t, "crash.built=0\n");
}

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
