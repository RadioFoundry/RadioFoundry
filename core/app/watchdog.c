// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The idle task's watchdog feed (watchdog.h, docs/spec/watchdog.md). Built only with CONFIG_WATCHDOG, which also turns
// on configUSE_IDLE_HOOK in every platform's FreeRTOSConfig.h.
#include "app/watchdog.h"
#include "hal_time.h"
#include "hal_watchdog.h"

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
	st.feeds++;
}

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
