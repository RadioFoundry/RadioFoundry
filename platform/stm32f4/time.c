// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Millisecond clock from the DWT cycle counter, so it runs before the scheduler and inside critical sections.
// The 32-bit cycle counter wraps every 59 s at 72 MHz; it is folded into a 64-bit count on every read, and the RTOS
// tick hook reads it every millisecond, so no wrap is missed.
#include "platform.h"
#include "hal_time.h"
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

void stm32_time_init(void)
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
	(void)cycles_now();
}
