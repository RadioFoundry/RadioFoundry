// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Millisecond clock from the DWT cycle counter, so it runs before the scheduler and inside critical sections.
// The 32-bit cycle counter wraps every 35 s at 120 MHz; it is folded into a 64-bit count on every read, and the RTOS
// tick hook reads it every millisecond, so no wrap is missed.
#include "config.h"
#include "platform.h"
#include "hal_time.h"
#include "hal_watchdog.h"
#if CONFIG_WATCHDOG && CONFIG_CRASH_LOG
#include "app/watchdog.h"
#endif
#include "FreeRTOS.h"
#include "task.h"

static uint64_t cycles;
static uint32_t last;

static uint64_t cycles_now(void)
{
	uint32_t primask = __get_PRIMASK();
	__disable_irq();
	uint32_t now = DWT->CYCCNT;
	cycles += (uint32_t)(now - last);
	last = now;
	uint64_t c = cycles;
	__set_PRIMASK(primask);
	return c;
}

void at32_time_init(void)
{
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT->CYCCNT = 0;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
	last = 0;
	cycles = 0;
}

uint32_t hal_millis(void)
{
	return (uint32_t)(cycles_now() / (SystemCoreClock / 1000u));
}

uint32_t hal_micros(void)
{
	return (uint32_t)(cycles_now() / (SystemCoreClock / 1000000u));
}

void hal_delay_us(uint32_t us)
{
	uint64_t end = cycles_now() + (uint64_t)us * (SystemCoreClock / 1000000u);
	while (cycles_now() < end) {
	}
}

void hal_delay_ms(uint32_t ms)
{
	if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
		vTaskDelay(pdMS_TO_TICKS(ms));
#if CONFIG_WATCHDOG
	} else if (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED) {
		for (uint32_t i = 0; i < ms; i++) {	// start-up: no idle task yet, so the waits feed the watchdog
			hal_delay_us(1000u);
			hal_watchdog_feed();
		}
#endif
	} else {
		hal_delay_us(ms * 1000u);
	}
}

void vApplicationTickHook(void)
{
	(void)cycles_now();
}

// The tick interrupt: the hang check first (crash-log.md 1.1), as on the STM32F4 (platform/stm32f4/time.c has why)
void xPortSysTickHandler(void);
void SysTick_Handler(void);
void SysTick_Handler(void)
{
#if CONFIG_WATCHDOG && CONFIG_CRASH_LOG
	watchdog_tick();
#endif
	xPortSysTickHandler();
}
