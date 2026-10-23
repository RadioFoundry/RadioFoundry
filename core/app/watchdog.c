// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The idle task's watchdog feed (watchdog.h, docs/spec/watchdog.md). Built only with CONFIG_WATCHDOG, which also turns
// on configUSE_IDLE_HOOK in every platform's FreeRTOSConfig.h.
#include "config.h"
#include "app/watchdog.h"
#include "hal_time.h"
#include "hal_watchdog.h"
#if CONFIG_CRASH_LOG
#include "crash/crash.h"
#endif

static watchdog_state_t st;

void watchdog_idle(uint32_t now_ms)
{
	if (st.seen) {
		uint32_t gap = now_ms - st.last_idle_ms;
		if (gap > st.max_gap_ms) {
			st.max_gap_ms = gap;
		}
	}
	st.seen = true;
	st.last_idle_ms = now_ms;
	if (st.starving) {
		return;
	}
	hal_watchdog_feed();
	st.last_fed_ms = now_ms;
	st.feeds++;
}

void watchdog_kick(uint32_t now_ms)
{
	if (st.starving) {
		return;
	}
	hal_watchdog_feed();
	st.last_fed_ms = now_ms;
}

bool watchdog_hung(uint32_t now_ms, uint32_t timeout_ms)
{
	if (!st.seen || st.starving || timeout_ms == 0) {
		return false;
	}
	return now_ms - st.last_fed_ms >= timeout_ms - timeout_ms / 4u;
}

#if CONFIG_CRASH_LOG
void watchdog_tick(void)
{
	uint32_t now = hal_millis();
	crash_alive(now);	// the uptime the next start reports (crash-log.md 1.3)
	if (watchdog_hung(now, hal_watchdog_timeout_ms())) {
		hal_crash_hang(now - st.last_fed_ms);
	}
}
#endif

void watchdog_starve(uint32_t now_ms)
{
	if (!st.starving) {
		hal_watchdog_feed();	// the full timeout from here
		st.starve_ms = now_ms;
	}
	st.starving = true;
}

void watchdog_clear_gap(void)
{
	st.max_gap_ms = 0;
}

const watchdog_state_t *watchdog_state(void)
{
	return &st;
}

// FreeRTOS calls it on every pass of the idle loop (configUSE_IDLE_HOOK). It must never block
void vApplicationIdleHook(void);
void vApplicationIdleHook(void)
{
	watchdog_idle(hal_millis());
}
