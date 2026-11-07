// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Millisecond clock from the DWT cycle counter, so it runs before the scheduler and inside critical sections.
// The 32-bit cycle counter wraps every 35 s at 120 MHz; its progress is folded into running microsecond and
// millisecond counts on every read (time_fold.h: 32-bit divides, no 64-bit libgcc call), and the RTOS tick hook reads
// it every millisecond, so no wrap is missed. hal_delay_us spins on the counter alone: the bit-banged buses wait 1 us
// per half bit, and a fold on every poll made that 2.2 us.
#include "config.h"
#include "platform.h"
#include "hal_time.h"
#include "hal_watchdog.h"
#if CONFIG_WATCHDOG && CONFIG_CRASH_LOG
#include "app/watchdog.h"
#endif
#include "FreeRTOS.h"
#include "task.h"
#include "time_fold.h"

static time_fold_t clk;

// Folded up to now; the caller's reads under the same mask
static void fold(void)
{
	time_fold(&clk, DWT->CYCCNT, SystemCoreClock / 1000000u);
}

void gd32_time_init(void)
{
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT->CYCCNT = 0;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
	time_fold_init(&clk, 0);
}

uint32_t hal_millis(void)
{
	uint32_t primask = __get_PRIMASK();
	__disable_irq();
	fold();
	uint32_t ms = clk.ms;
	__set_PRIMASK(primask);
	return ms;
}

uint32_t hal_micros(void)
{
	uint32_t primask = __get_PRIMASK();
	__disable_irq();
	fold();
	uint32_t us = clk.us;
	__set_PRIMASK(primask);
	return us;
}

// In pieces of at most a second, so the cycle count of each fits 32 bits; a long wait folds after each piece, as a
// wait with the interrupts off (no tick hook) could pass a counter wrap
#define DELAY_PIECE_US	1000000u

void hal_delay_us(uint32_t us)
{
	uint32_t per_us = SystemCoreClock / 1000000u;
	while (us > 0) {
		uint32_t n = us < DELAY_PIECE_US ? us : DELAY_PIECE_US;
		us -= n;
		uint32_t start = DWT->CYCCNT;
		uint32_t cycles = n * per_us;
		while ((uint32_t)(DWT->CYCCNT - start) < cycles) {
		}
		if (n >= 1000u) {
			(void)hal_millis();
		}
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
	(void)hal_millis();
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
