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

void HardFault_Handler(void)
{
	__disable_irq();
	for (;;) {
	}
}
