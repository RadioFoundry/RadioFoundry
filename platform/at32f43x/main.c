// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// AT32F43x entry: the board's early init, then the core application under FreeRTOS
#include "config.h"
#include "platform.h"
#include "board.h"
#include "hal_watchdog.h"
#include "FreeRTOS.h"
#include "task.h"

void at32_time_init(void);
void app_start(void);	// core/app: creates the tasks

int main(void)
{
	board_rf_safe();	// transmitter off before anything that could hang (docs/spec/crash-log.md 8)
#if CONFIG_WATCHDOG
	hal_watchdog_start();
#endif
	NVIC_SetPriorityGrouping(3u);	// all 4 bits preemption (FreeRTOS needs this)
	at32_time_init();
	board_early_init();	// sets the clock; its busy waits feed (hal_delay_ms before the scheduler)
	at32_time_init();	// the clock changed; restart the count from the new rate
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

// Without the crash log a crash stops the radio here
#if !CONFIG_CRASH_LOG
void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
	(void)task;
	(void)name;
	__disable_irq();
	board_rf_safe();
	for (;;) {
	}
}

void vApplicationMallocFailedHook(void)
{
	__disable_irq();
	board_rf_safe();
	for (;;) {
	}
}

void HardFault_Handler(void)
{
	__disable_irq();
	board_rf_safe();
	for (;;) {
	}
}

void NMI_Handler(void) __attribute__((alias("HardFault_Handler")));
#endif
