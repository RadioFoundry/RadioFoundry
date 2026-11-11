// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Time base (docs/spec/hardware-dm32.md 3.4): timer 2 runs free at APB 84 MHz with its interrupt masked and is read as
// a cycle counter, so the millisecond clock works before the scheduler and inside critical sections. Its 32-bit count
// wraps every 51 s; every read folds the cycles since the last one into microsecond and millisecond counts (32 bits,
// the remainders carried: no 64-bit division) and the RTOS tick hook reads it every millisecond.
// Timer 1 is the RTOS tick, timer 3 the DMR PA one-shot (hrc_timer_oneshot), timer 4 the AFSK FIFO top-up.
#include "config.h"
#include "platform.h"
#include "hal_time.h"
#include "hal_watchdog.h"
#include "FreeRTOS.h"
#include "task.h"

#define CYCLES_PER_US	(HRC_APB_HZ / 1000000u)

extern void vPortTickHandler(void);

static uint32_t last;		// the cycle count at the last read
static uint32_t now_us, us_cycles;	// microseconds, and the cycles past the last one
static uint32_t now_ms, ms_us;	// milliseconds, and the microseconds past the last one

// Folds the cycles since the last call in (interrupts off: any task or interrupt may read the clock)
static void fold(void)
{
	uint32_t psr = hrc_irq_save();
	uint32_t now = hrc_cycles();
	uint32_t d = now - last;
	last = now;
	uint32_t q = d / CYCLES_PER_US;
	uint32_t r = d % CYCLES_PER_US + us_cycles;
	if (r >= CYCLES_PER_US) {
		r -= CYCLES_PER_US;
		q++;
	}
	now_us += q;
	us_cycles = r;
	uint32_t t = ms_us + q;
	now_ms += t / 1000u;
	ms_us = t % 1000u;
	hrc_irq_restore(psr);
}

void hrc_time_init(void)
{
	TMR_CTRL(TIMER_CLOCK) = TMR_CTRL_INT_MASK;	// stopped, free-running mode (counts down from 0xFFFFFFFF, wraps)
	TMR_LOAD(TIMER_CLOCK) = 0xFFFFFFFFu;
	TMR_CTRL(TIMER_CLOCK) = TMR_CTRL_INT_MASK | TMR_CTRL_EN;
	last = hrc_cycles();
	now_us = us_cycles = now_ms = ms_us = 0;
}

uint32_t hrc_micros(void)
{
	fold();
	return now_us;
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
	while (n > 0) {
		uint32_t step = n < 1000u ? n : 1000u;	// in steps: the count can't overflow, and the fold sees every wrap
		uint32_t end = hal_micros() + step;
		while ((int32_t)(hal_micros() - end) < 0) {
		}
		n -= step;
	}
}

void hal_delay_ms(uint32_t ms)
{
	if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING && !hrc_in_isr()) {
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

void hrc_tick_start(uint32_t hz)
{
	TMR_CTRL(TIMER_TICK) = TMR_CTRL_INT_MASK;
	TMR_LOAD(TIMER_TICK) = HRC_APB_HZ / hz;
	TMR_CTRL(TIMER_TICK) = TMR_CTRL_PERIODIC;
	hrc_timer_ack(TIMER_TICK);
	TMR_CTRL(TIMER_TICK) = TMR_CTRL_PERIODIC | TMR_CTRL_EN;
	hrc_irq_register(IRQ_TIMER1, vPortTickHandler);
	hrc_irq_enable(IRQ_TIMER1, true);
}

void hrc_timer_oneshot(uint8_t n, uint32_t us)
{
	// The timers have no one-shot mode: free-running mode with the handler stopping it at the first expiry
	TMR_CTRL(n) = TMR_CTRL_INT_MASK;
	TMR_LOAD(n) = us * CYCLES_PER_US;
	hrc_timer_ack(n);
	TMR_CTRL(n) = TMR_CTRL_EN;
}

void hrc_timer_periodic(uint8_t n, uint32_t us)
{
	TMR_CTRL(n) = TMR_CTRL_INT_MASK;
	TMR_LOAD(n) = us * CYCLES_PER_US;
	TMR_CTRL(n) = TMR_CTRL_PERIODIC;
	hrc_timer_ack(n);
	TMR_CTRL(n) = TMR_CTRL_PERIODIC | TMR_CTRL_EN;
}

void hrc_timer_stop(uint8_t n)
{
	TMR_CTRL(n) = TMR_CTRL_INT_MASK;
	hrc_timer_ack(n);
}

void vApplicationTickHook(void)
{
	fold();
	hrc_irq_stack_check();
}
