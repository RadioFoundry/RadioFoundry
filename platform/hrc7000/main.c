// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// HR_C7000 entry (from crt0.S): clocks, interrupts, time base, the board's early init, then the core under FreeRTOS
#include "platform.h"
#include "board.h"
#include "FreeRTOS.h"
#include "task.h"

void app_start(void);	// core/app: creates the tasks

int main(void)
{
	hrc_clock_init();
	hrc_irq_init();
	hrc_time_init();
	board_early_init();
	app_start();
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
	for (;;) {
	}
}

void vApplicationMallocFailedHook(void)
{
	__asm__ volatile ("psrclr ie");
	for (;;) {
	}
}
#endif
