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
	uint32_t last_pass_ms;	// the host link task's last loop pass (watchdog_task_pass)
	bool passed;		// it has passed once: the stall check runs from then on
	uint32_t link_limit_ms;	// the link watch: restart after this long without a debug frame, 0 off
	uint32_t link_last_ms;	// its last frame
} watchdog_state_t;

// The stall check's limit: the host link's task blocked this long (waiting, so the idle task still feeds) is stuck. Far
// above any request it serves; a stuck task otherwise leaves the radio running with its computer link dead for good
#define WATCHDOG_STALL_MS	60000u

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
// The host link's task, on every pass of its loop (5 ms at most apart while it works)
void watchdog_task_pass(uint32_t now_ms);
// The stall check's test: true once the host link's task last passed WATCHDOG_STALL_MS ago or more, after its first
// pass and unless a bench test starves the watchdog
bool watchdog_stalled(uint32_t now_ms);
// The link watch (DL_CMD_LINK_HOLD): a debug client asks that the radio restart if no valid frame comes for limit_ms
// (0 stops it), so a dead link recovers without a power cycle. cause (from the tick interrupt) gives the record's cause
void watchdog_link_watch(uint32_t now_ms, uint32_t limit_ms, uint32_t (*cause)(uint32_t now_ms));
void watchdog_link_frame(uint32_t now_ms);
bool watchdog_link_lost(uint32_t now_ms);
// The hang check's test: true once the last feed is at least three quarters of timeout_ms ago, after the idle task has
// run once and unless a bench test starves the watchdog on purpose (that test times the hardware reset). timeout_ms 0
// (not running) never hangs
bool watchdog_hung(uint32_t now_ms, uint32_t timeout_ms);
// From the RTOS tick interrupt (vPortTickHandler, SysTick_Handler), never the tick hook, which xTaskResumeAll also
// calls from a task: watchdog_hung and watchdog_stalled, then hal_crash_hang. Only with CONFIG_CRASH_LOG
void watchdog_tick(void);
const watchdog_state_t *watchdog_state(void);

#endif
