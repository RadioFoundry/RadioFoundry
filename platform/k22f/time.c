// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Millisecond clock from the DWT cycle counter, as on the STM32 radios: it runs before the scheduler and inside
// critical sections. The 32-bit count is folded into a 64-bit one on every read, and the RTOS tick hook reads it every
// millisecond, so no wrap is missed (it wraps every 45 s at 95 MHz). A clock change restarts the count (main.c).
#include "platform.h"
#include "hal_time.h"
#include "FreeRTOS.h"
#include "task.h"

static uint64_t cycles;
static uint32_t last;

static uint64_t cycles_now(void)
{
	uint32_t primask = k22f_irq_save();
	uint32_t now = DWT->CYCCNT;
	cycles += (uint32_t)(now - last);
	last = now;
	uint64_t c = cycles;
	k22f_irq_restore(primask);
	return c;
}

void k22f_time_init(void)
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
	} else {
		hal_delay_us(ms * 1000u);
	}
}

void vApplicationTickHook(void)
{
	(void)cycles_now();
}
