// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Millisecond clock from the DWT cycle counter, so it runs before the scheduler and inside critical sections.
// The 32-bit cycle counter wraps every 59 s at 72 MHz; every read folds the cycles since the last one into microsecond
// and millisecond counts (32 bits, the remainders carried: no 64-bit division), and the RTOS tick hook reads it every
// millisecond, so no wrap is missed.
#include "config.h"
#include "platform.h"
#include "hal_time.h"
#include "hal_watchdog.h"
#if CONFIG_WATCHDOG && CONFIG_CRASH_LOG
#include "app/watchdog.h"
#endif
#include "FreeRTOS.h"
#include "task.h"

static uint32_t last;		// the cycle count at the last read
static uint32_t now_us, us_cycles;	// microseconds, and the cycles past the last one
static uint32_t now_ms, ms_us;	// milliseconds, and the microseconds past the last one

// Folds the cycles since the last call in (interrupts off: any task or interrupt may read the clock)
static void fold(void)
{
	uint32_t primask = __get_PRIMASK();
	__disable_irq();
	uint32_t per_us = SystemCoreClock / 1000000u;
	uint32_t now = DWT->CYCCNT;
	uint32_t d = now - last;
	last = now;
	uint32_t q = d / per_us;
	uint32_t r = d % per_us + us_cycles;
	if (r >= per_us) {
		r -= per_us;
		q++;
	}
	now_us += q;
	us_cycles = r;
	uint32_t t = ms_us + q;
	now_ms += t / 1000u;
	ms_us = t % 1000u;
	__set_PRIMASK(primask);
}

void stm32_time_init(void)
{
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT->CYCCNT = 0;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
	last = 0;
	now_us = us_cycles = now_ms = ms_us = 0;
}

uint32_t hal_millis(void)
{
	fold();
	return now_ms;
}

uint32_t hal_micros(void)
{
	fold();
	return now_us;
}

void hal_delay_us(uint32_t n)
{
	uint32_t per_us = SystemCoreClock / 1000000u;
	while (n > 0) {
		uint32_t step = n < 1000u ? n : 1000u;	// in steps: the count can't overflow, and the fold sees every wrap
		uint32_t start = DWT->CYCCNT;
		while (DWT->CYCCNT - start < step * per_us) {
		}
		n -= step;
		fold();
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

// The ST HAL's time base: no timer of its own
HAL_StatusTypeDef HAL_InitTick(uint32_t TickPriority)
{
	(void)TickPriority;
	return HAL_OK;
}

uint32_t HAL_GetTick(void)
{
	return hal_millis();
}

void HAL_Delay(uint32_t Delay)
{
	hal_delay_ms(Delay);
}

void vApplicationTickHook(void)
{
	fold();
	stm32_irq_stack_check();
}

// The tick interrupt: the hang check first (crash-log.md 1.1), here rather than in the tick hook, which xTaskResumeAll
// also calls from a task for ticks that came while the scheduler was suspended; the check needs the interrupted
// task's frame on the PSP
void xPortSysTickHandler(void);
void SysTick_Handler(void);
void SysTick_Handler(void)
{
#if CONFIG_WATCHDOG && CONFIG_CRASH_LOG
	watchdog_tick();
#endif
	xPortSysTickHandler();
}
