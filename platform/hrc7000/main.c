// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// HR_C7000 entry (from crt0.S): transmitter off, clocks, interrupts, time base, the board's early init, then the core under FreeRTOS
#include "config.h"
#include "platform.h"
#include "board.h"
#include "hal_watchdog.h"
#include "FreeRTOS.h"
#include "task.h"

void app_start(void);	// core/app: creates the tasks

int main(void)
{
	board_rf_safe();	// transmitter off before anything that could hang (a soft reset may keep the pin levels)
	hrc_clock_init();	// the watchdog off whatever the loader left, then the clocks it counts from
#if CONFIG_WATCHDOG
	hal_watchdog_start();	// from here on: the idle task feeds it once the scheduler runs, main() until then
#endif
	hrc_irq_init();
	hrc_time_init();
	board_early_init();	// its busy waits feed (hal_delay_ms before the scheduler)
#if CONFIG_WATCHDOG
	hal_watchdog_feed();
#endif
	app_start();
#if CONFIG_WATCHDOG
	hal_watchdog_feed();	// the start-up task's first work runs before the idle task does
#endif
	vTaskStartScheduler();
	for (;;) {
	}
}

// Without the crash log a crash stops the radio here (core/crash and crash_hrc.c otherwise: kept and restarted)
#if !CONFIG_CRASH_LOG
void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
	(void)task;
	(void)name;
	__asm__ volatile ("psrclr ie");
	hrc_rf_safe();
	for (;;) {
	}
}

void vApplicationMallocFailedHook(void)
{
	__asm__ volatile ("psrclr ie");
	hrc_rf_safe();
	for (;;) {
	}
}
#endif
