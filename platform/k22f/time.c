// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Millisecond clock from the DWT cycle counter, as on the STM32 radios: it runs before the scheduler and inside
// critical sections. Every read folds the cycles since the last one into whole milliseconds plus a remainder, so the
// 32-bit counter's wrap (every 45 s at 95 MHz) is never missed as long as something reads it more often (the RTOS
// tick hook does, every millisecond). The core clock can change at run time (clock.c: the HSRUN step down for flash
// programming, the fallback plans): k22f_time_clock_changed() folds what was counted at the old rate first, so the
// time keeps running without a jump. Every clock plan here has a whole number of cycles per millisecond.
#include "platform.h"
#include "hal_time.h"
#include "FreeRTOS.h"
#include "task.h"

static uint64_t ms;		// whole milliseconds
static uint32_t rem;		// cycles past the last whole millisecond
static uint32_t last;		// DWT->CYCCNT at the last fold
static uint32_t per_ms = K22F_FEI_HZ / 1000u;

// Interrupts off around it: the tick hook and the tasks both fold
static void fold(void)
{
	uint32_t now = DWT->CYCCNT;
	k22f_time_fold(&ms, &rem, now - last, per_ms);
	last = now;
}

void k22f_time_init(void)
{
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT->CYCCNT = 0;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
	last = 0;
	ms = 0;
	rem = 0;
	per_ms = SystemCoreClock / 1000u;
}

void k22f_time_clock_changed(void)
{
	uint32_t s = k22f_irq_save();
	fold();
	uint32_t next = SystemCoreClock / 1000u;
	rem = k22f_time_rescale(rem, per_ms, next);
	per_ms = next;
	k22f_irq_restore(s);
}

static uint64_t micros_now(void)
{
	uint32_t s = k22f_irq_save();
	fold();
	uint64_t us = ms * 1000u + rem * 1000u / per_ms;	// rem * 1000 < 2^32 below 4 GHz
	k22f_irq_restore(s);
	return us;
}

uint32_t hal_millis(void)
{
	uint32_t s = k22f_irq_save();
	fold();
	uint32_t m = (uint32_t)ms;
	k22f_irq_restore(s);
	return m;
}

uint32_t hal_micros(void)
{
	return (uint32_t)micros_now();
}

void hal_delay_us(uint32_t us)
{
	uint64_t end = micros_now() + us;
	while (micros_now() < end) {
	}
}

void hal_delay_ms(uint32_t ms_)
{
	if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
		vTaskDelay(pdMS_TO_TICKS(ms_));
	} else {
		hal_delay_us(ms_ * 1000u);
	}
}

void vApplicationTickHook(void)
{
	uint32_t s = k22f_irq_save();
	fold();
	k22f_irq_restore(s);
}
