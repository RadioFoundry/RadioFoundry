// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// STM32F4 entry: platform set-up, the board's early init, then the core application under FreeRTOS
#include "platform.h"
#include "board.h"
#include "FreeRTOS.h"
#include "task.h"

void stm32_time_init(void);
void app_start(void);	// core/app: creates the tasks

int main(void)
{
	board_rf_safe();	// transmitter off before anything that could hang (docs/spec/crash-log.md 8)
	stm32_sysboot_check();	// REBOOT ROM-DFU: into the ROM bootloader before anything is set up
	HAL_Init();
	HAL_NVIC_SetPriorityGrouping(NVIC_PRIORITYGROUP_4);	// all 4 bits preemption (FreeRTOS needs this)
	stm32_time_init();
	board_early_init();
	stm32_time_init();		// the clock changed; restart the count from the new rate
	app_start();
	vTaskStartScheduler();
	for (;;) {
	}
}

// Without the crash log a crash stops the radio here (core/crash and crash_stm32.c otherwise: kept and restarted)
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
