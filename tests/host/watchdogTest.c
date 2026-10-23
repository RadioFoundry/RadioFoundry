// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The hardware watchdog's portable side (docs/spec/watchdog.md): the idle task's feed, the longest idle gap, the bench
// starve (core/app/watchdog.c), and the debug link's watchdog section and DL_CMD_WATCHDOG
// (core/debuglink/debuglink_watchdog.c, docs/spec/debuglink.md 3.21). The platform (hal_watchdog.h), the clock, the
// radio's view and the log are faked
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "app/watchdog.h"
#include "debuglink/debuglink.h"
#include "debuglink/debuglink_proto.h"
#include "hal_watchdog.h"
#include "hal_crash.h"

static int failures, checks;

#define CHECK(cond)                                                                                                \
	do {                                                                                                       \
		checks++;                                                                                          \
		if (!(cond)) {                                                                                     \
			failures++;                                                                                \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                     \
		}                                                                                                  \
	} while (0)

// --- fakes ---

static uint32_t now_ms, delayed_ms;
static unsigned feeds;
static bool load_supported, load_called;
static uint32_t load_got[3];
static radio_state_t fake_state = RS_RX;

uint32_t hal_millis(void) { return now_ms; }
void hal_delay_ms(uint32_t ms) { delayed_ms += ms; now_ms += ms; }
void hal_watchdog_feed(void) { feeds++; }
static uint32_t timeout_ms = 4000;
uint32_t hal_watchdog_timeout_ms(void) { return timeout_ms; }

// The platform's hang capture never returns: back to the test that ticked
static jmp_buf hang_jmp;
static unsigned hangs;
static uint32_t hang_ms;
// The restart cause's uptime (core/crash/crash.c): what the tick last kept
static uint32_t alive_ms;
void crash_alive(uint32_t ms) { alive_ms = ms; }

void hal_crash_hang(uint32_t ms)
{
	hangs++;
	hang_ms = ms;
	longjmp(hang_jmp, 1);
}

// One RTOS tick at the clock's time; true if the hang check fired
static bool tick(void)
{
	if (setjmp(hang_jmp) != 0)
		return true;
	watchdog_tick();
	return false;
}

uint8_t hal_watchdog_regs(uint32_t *out)
{
	out[0] = 0x11;
	out[1] = 0x2200;
	return 2;
}

bool hal_watchdog_test_load(const uint32_t arg[3])
{
	if (arg == NULL || !load_supported)
		return load_supported;
	load_called = true;
	memcpy(load_got, arg, sizeof load_got);
	return true;
}

void radio_get_view(radio_view_t *v)
{
	memset(v, 0, sizeof *v);
	v->state = fake_state;
}

void dlog(const char *fmt, ...)
{
	(void)fmt;
}

void vApplicationIdleHook(void);

// --- helpers ---

static uint8_t cmd(const uint8_t *req, uint16_t len)
{
	uint8_t out[16];
	size_t n = debuglink_cmd_watchdog(req, len, out, sizeof out);
	CHECK(n == 1);
	return out[0];
}

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static uint8_t test_cmd(uint8_t kind, uint32_t token)
{
	uint8_t req[6] = { DL_WDT_OP_TEST, kind };
	put32(req + 2, token);
	return cmd(req, sizeof req);
}

static bool has(const char *text, const char *line)
{
	return strstr(text, line) != NULL;
}

static void view(char *buf, size_t cap)
{
	dl_text_t t;
	dl_text_init(&t, buf, cap - 1);
	debuglink_view_watchdog(&t);
	buf[t.len] = 0;
}

// --- tests ---

// The idle task feeds on every pass and measures how long it stayed away; the first pass has no gap to measure
static void test_idle_feeds(void)
{
	const watchdog_state_t *s = watchdog_state();
	CHECK(!s->seen && s->feeds == 0);
	CHECK(!watchdog_hung(500000, 4000));		// the idle task hasn't run yet: no feed to count from
	now_ms = 1000;
	vApplicationIdleHook();			// through the FreeRTOS hook, at hal_millis()
	CHECK(feeds == 1 && s->feeds == 1 && s->seen && s->last_idle_ms == 1000 && s->max_gap_ms == 0);
	watchdog_idle(1010);
	watchdog_idle(2510);			// a busy task kept the idle task out for 1.5 s
	watchdog_idle(2520);
	CHECK(feeds == 4 && s->max_gap_ms == 1500);
	watchdog_idle(3000);			// a shorter gap leaves the longest
	CHECK(s->max_gap_ms == 1500);
	watchdog_clear_gap();
	CHECK(s->max_gap_ms == 0);
	watchdog_idle(3020);
	CHECK(s->max_gap_ms == 20);
	// The millisecond clock wraps: the gap is still right
	watchdog_idle(0xFFFFFF00u);
	watchdog_clear_gap();
	watchdog_idle(0x00000064u);
	CHECK(s->max_gap_ms == 0x164);
	now_ms = 0x64;
}

// The hang check (crash-log.md 1.1): three quarters of the timeout after the last feed, from the idle task or a long
// job's kick, the tick hands the time since to the platform's capture; never before the idle task's first pass, never
// with the watchdog stopped
static void test_hang_check(void)
{
	uint32_t saved_now = now_ms;
	now_ms = 10000;
	watchdog_idle(now_ms);
	now_ms += 2999;
	CHECK(!tick() && hangs == 0);
	CHECK(alive_ms == now_ms);	// every tick keeps the uptime, so a watchdog reset still says how long the run was
	now_ms += 1;
	CHECK(tick() && hangs == 1 && hang_ms == 3000);	// 4 s timeout: a record at 3 s, a second before the reset
	watchdog_idle(now_ms);
	now_ms += 2000;
	unsigned before = feeds;
	watchdog_kick(now_ms);				// a long job feeds: the count starts again there
	CHECK(feeds == before + 1 && watchdog_state()->last_fed_ms == now_ms);
	now_ms += 2999;
	CHECK(!tick());
	now_ms += 1;
	CHECK(tick() && hangs == 2 && hang_ms == 3000);
	timeout_ms = 5000;				// the STM32 IWDG: 3.75 s
	CHECK(!watchdog_hung(now_ms + 749, 5000) && watchdog_hung(now_ms + 750, 5000));
	timeout_ms = 0;					// not running: no hang
	now_ms += 60000;
	CHECK(!tick() && hangs == 2);
	timeout_ms = 4000;
	// The millisecond clock wraps between the feed and the tick
	now_ms = 0xFFFFFF00u;
	watchdog_idle(now_ms);
	now_ms = 0x00000B00u;				// 0xC00 = 3072 ms later
	CHECK(tick() && hang_ms == 0xC00u);
	now_ms = saved_now;
	watchdog_idle(now_ms);
	watchdog_clear_gap();				// the gaps above aren't the next test's
}

// The view: the keys a HIL test reads
static void test_view(void)
{
	char buf[512];
	now_ms += 30;
	view(buf, sizeof buf);
	CHECK(has(buf, "watchdog.built=1\n"));
	CHECK(has(buf, "watchdog.timeout_ms=4000\n"));
	CHECK(has(buf, "watchdog.starving=0\n"));
	CHECK(!has(buf, "watchdog.starved_ms="));
	CHECK(has(buf, "watchdog.idle_age_ms=30\n"));
	CHECK(has(buf, "watchdog.max_gap_ms=356\n"));
	CHECK(has(buf, "watchdog.regs=0x00000011,0x00002200\n"));
}

// The guards: length, kind, token, the transmitter. Nothing starts and nothing waits
static void test_refusals(void)
{
	uint8_t empty = 0;
	CHECK(cmd(&empty, 0) == DL_ST_BAD_ARG);
	uint8_t clear_long[2] = { DL_WDT_OP_CLEAR_GAP, 0 };
	CHECK(cmd(clear_long, 2) == DL_ST_BAD_ARG);
	uint8_t unknown = 7;
	CHECK(cmd(&unknown, 1) == DL_ST_BAD_ARG);
	CHECK(test_cmd(0, DL_WDT_TOKEN) == DL_ST_BAD_ARG);
	CHECK(test_cmd(DL_WDT_TEST_IRQ_OFF + 1, DL_WDT_TOKEN) == DL_ST_BAD_ARG);
	CHECK(test_cmd(DL_WDT_TEST_STARVE, DL_WDT_TOKEN ^ 1u) == DL_ST_REFUSED);
	fake_state = RS_TX;
	CHECK(test_cmd(DL_WDT_TEST_STARVE, DL_WDT_TOKEN) == DL_ST_REFUSED);
	fake_state = RS_RX;
	uint8_t short_test[5] = { DL_WDT_OP_TEST, DL_WDT_TEST_STARVE, 0x57, 0x44, 0x4F };
	CHECK(cmd(short_test, sizeof short_test) == DL_ST_BAD_ARG);
	// LOAD where the platform has no such test (the STM32): unavailable, whatever the token
	uint8_t load[17] = { DL_WDT_OP_LOAD };
	put32(load + 13, DL_WDT_TOKEN);
	load_supported = false;
	CHECK(cmd(load, sizeof load) == DL_ST_UNAVAILABLE);
	load_supported = true;
	put32(load + 13, 0);
	CHECK(cmd(load, sizeof load) == DL_ST_REFUSED);
	debuglink_watchdog_service();
	CHECK(delayed_ms == 0 && !load_called && !watchdog_state()->starving);
	uint8_t clear = DL_WDT_OP_CLEAR_GAP;
	CHECK(cmd(&clear, 1) == DL_ST_OK && watchdog_state()->max_gap_ms == 0);
}

// A test armed while receiving, then a PTT before it starts: dropped, the feeds go on
static void test_ptt_cancels(void)
{
	CHECK(test_cmd(DL_WDT_TEST_STARVE, DL_WDT_TOKEN) == DL_ST_OK);
	fake_state = RS_TX;
	debuglink_watchdog_service();
	fake_state = RS_RX;
	CHECK(delayed_ms == 100 && !watchdog_state()->starving);
	debuglink_watchdog_service();		// nothing pending any more
	CHECK(delayed_ms == 100);
}

// LOAD: the platform gets the three raw words after the reply has gone out, then the idle task stops feeding
static void test_load_then_starve(void)
{
	uint8_t load[17] = { DL_WDT_OP_LOAD };
	put32(load + 1, 0x00001234u);
	put32(load + 5, 0x00005678u);
	put32(load + 9, 0xFFFFFFFFu);
	put32(load + 13, DL_WDT_TOKEN);
	CHECK(cmd(load, sizeof load) == DL_ST_OK);
	CHECK(!load_called);			// nothing before the service hook
	debuglink_watchdog_service();
	CHECK(load_called && load_got[0] == 0x1234u && load_got[1] == 0x5678u && load_got[2] == 0xFFFFFFFFu);
	CHECK(watchdog_state()->starving && watchdog_state()->starve_ms == now_ms);
	unsigned before = feeds, counted = watchdog_state()->feeds;
	watchdog_idle(now_ms + 5);
	watchdog_idle(now_ms + 4000);
	CHECK(feeds == before && watchdog_state()->feeds == counted);	// starving: no feed, the gap still measured
	CHECK(watchdog_state()->max_gap_ms == 3995);
	watchdog_kick(now_ms + 4001);			// a long job's kick doesn't feed a starve either
	CHECK(feeds == before);
	unsigned hangs_before = hangs;
	now_ms += 10000;
	CHECK(!tick() && hangs == hangs_before);	// the starve times the hardware reset: no hang record ahead of it
	now_ms -= 10000;
	char buf[512];
	now_ms += 2500;
	view(buf, sizeof buf);
	CHECK(has(buf, "watchdog.starving=1\n"));
	CHECK(has(buf, "watchdog.starved_ms=2500\n"));	// since the feeds stopped (the service's 100 ms wait is before)
}

int main(void)
{
	test_idle_feeds();
	test_view();
	test_refusals();
	test_ptt_cancels();
	test_hang_check();
	test_load_then_starve();	// last: the starve is for good
	printf("watchdogTest: %d checks, %d failures\n", checks, failures);
	return failures != 0;
}
