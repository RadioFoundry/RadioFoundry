// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// K22F entry (from startup.c): time base, the board's early init (pins, clocks), then the core under FreeRTOS
#include "platform.h"
#include "board.h"
#include "config.h"
#include "FreeRTOS.h"
#include "task.h"

void app_start(void);	// core/app: creates the tasks

int main(void)
{
	NVIC_SetPriorityGrouping(0);	// all 4 bits preemption (FreeRTOS needs this)
	k22f_time_init();
	board_early_init();		// the clock switch keeps the time base going (time.c)
	app_start();
	vTaskStartScheduler();
	for (;;) {
	}
}

// Without the crash log a crash stops the radio here (with it: crash_k22f.c and core/crash)
#if !CONFIG_CRASH_LOG
void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
	(void)task;
	(void)name;
	__disable_irq();
	for (;;) {
	}
}

void vApplicationMallocFailedHook(void)
{
	__disable_irq();
	for (;;) {
	}
}
#endif
