// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Time base (docs/spec/hardware-dm32.md 3.4): timer 2 runs free at APB 84 MHz with its interrupt masked and is read as
// a cycle counter, so the millisecond clock works before the scheduler and inside critical sections. Its 32-bit count
// wraps every 51 s; it is folded into a 64-bit count on every read and the RTOS tick hook reads it every millisecond.
// Timer 1 is the RTOS tick, timer 3 the DMR PA one-shot (hrc_timer_oneshot), timer 4 the AFSK FIFO top-up.
#include "config.h"
#include "platform.h"
#include "hal_time.h"
#include "hal_watchdog.h"
#include "FreeRTOS.h"
#include "task.h"

#define CYCLES_PER_US	(HRC_APB_HZ / 1000000u)

extern void vPortTickHandler(void);

static uint64_t cycles;
static uint32_t last;

static uint64_t cycles_now(void)
{
	uint32_t psr = hrc_irq_save();
	uint32_t now = hrc_cycles();
	cycles += (uint32_t)(now - last);
	last = now;
	uint64_t c = cycles;
	hrc_irq_restore(psr);
	return c;
}

void hrc_time_init(void)
{
	TMR_CTRL(TIMER_CLOCK) = TMR_CTRL_INT_MASK;	// stopped, free-running mode (counts down from 0xFFFFFFFF, wraps)
	TMR_LOAD(TIMER_CLOCK) = 0xFFFFFFFFu;
	TMR_CTRL(TIMER_CLOCK) = TMR_CTRL_INT_MASK | TMR_CTRL_EN;
	last = hrc_cycles();
	cycles = 0;
}

uint32_t hrc_micros(void)
{
	return (uint32_t)(cycles_now() / CYCLES_PER_US);
}

uint32_t hal_millis(void)
{
	return (uint32_t)(cycles_now() / (HRC_APB_HZ / 1000u));
}

uint32_t hal_micros(void)
{
	return (uint32_t)(cycles_now() / CYCLES_PER_US);
}

void hal_delay_us(uint32_t us)
{
	uint64_t end = cycles_now() + (uint64_t)us * CYCLES_PER_US;
	while (cycles_now() < end) {
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

static void (*volatile symbol_fn)(void);

static void symbol_isr(void)
{
	hrc_timer_ack(TIMER_AFSK);
	void (*fn)(void) = symbol_fn;
	if (fn != NULL) {
		fn();
	}
}

void hrc_symbol_timer_start(uint32_t us, void (*fn)(void))
{
	symbol_fn = fn;
	hrc_irq_register(IRQ_TIMER4, symbol_isr);
	hrc_irq_enable(IRQ_TIMER4, true);
	hrc_timer_periodic(TIMER_AFSK, us);
}

void hrc_symbol_timer_stop(void)
{
	hrc_timer_stop(TIMER_AFSK);
	hrc_irq_enable(IRQ_TIMER4, false);
}

void vApplicationTickHook(void)
{
	(void)cycles_now();
}
