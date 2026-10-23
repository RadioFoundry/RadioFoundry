// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Feeding the hardware watchdog from the idle task (CONFIG_WATCHDOG, docs/spec/watchdog.md). The idle task runs only
// when every other task waits, so a feed from it says the whole system still gets through its work: a task that
// spins, a stuck critical section or an interrupt storm stops the feeds and the chip resets. Portable (no RTOS calls),
// host tested in tests/host/watchdogTest.c; the platform's registers are behind hal_watchdog.h.
//
// The hang check (with CONFIG_CRASH_LOG, docs/spec/crash-log.md 1.1): the hardware reset leaves no record, so the RTOS
// tick checks how long ago the last feed was. A quarter of the timeout before the reset, it saves a crash record of
// the task the tick interrupted (hal_crash_hang) and restarts. A hang with interrupts off still ends in the plain reset.
#ifndef WATCHDOG_H
#define WATCHDOG_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
	uint32_t feeds;		// idle task feeds since start
	uint32_t last_idle_ms;	// when the idle task last ran
	uint32_t max_gap_ms;	// the longest time the idle task stayed out since start (or watchdog_clear_gap)
	uint32_t last_fed_ms;	// the last feed: the idle task's or a long job's (watchdog_kick)
	bool seen;		// the idle task has run at least once
	bool starving;		// a bench test stopped the feeds: the chip resets within the timeout
	uint32_t starve_ms;	// when it did (the last feed before the reset)
} watchdog_state_t;

// The idle hook's work (vApplicationIdleHook calls it with hal_millis()): note the gap, feed unless starving
void watchdog_idle(uint32_t now_ms);
// Bench test: the idle task stops feeding for good (only the reset ends it); now_ms is noted, so the time since can
// be read until the reset
void watchdog_starve(uint32_t now_ms);
// Long work that keeps the idle task out on purpose (a busy loop over flash): feed and note it, so the hang check
// counts from here
void watchdog_kick(uint32_t now_ms);
// Restart the longest-gap measurement
void watchdog_clear_gap(void);
// The hang check's test: true once the last feed is at least three quarters of timeout_ms ago, after the idle task has
// run once and unless a bench test starves the watchdog on purpose (that test times the hardware reset). timeout_ms 0
// (not running) never hangs
bool watchdog_hung(uint32_t now_ms, uint32_t timeout_ms);
// From the RTOS tick interrupt (vPortTickHandler, SysTick_Handler), never the tick hook, which xTaskResumeAll also
// calls from a task: watchdog_hung, then hal_crash_hang. Only with CONFIG_CRASH_LOG
void watchdog_tick(void);
const watchdog_state_t *watchdog_state(void);

#endif
