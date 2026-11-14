// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The Cortex-M main stack (MSP) as the interrupt handlers' stack (hal_system_t.irq_stack_free). FreeRTOS' Cortex-M
// port runs the tasks on their own stacks and the handlers on the MSP, which it resets to the top of RAM when the
// scheduler starts. That stack spans from the end of .bss (the linker's `_end`) to _estack; below it lies .bss, so an
// overflow would overwrite variables and show up as anything but a crash. The reset handler fills it, the tick checks
// its lowest words, and the debug link reads how much of the fill was never touched. Start-up before the scheduler
// runs on it too, so the mark counts that as well.
#ifndef CORTEXM_MSP_STACK_H
#define CORTEXM_MSP_STACK_H

#include <stdbool.h>
#include <stdint.h>

#define MSP_STACK_FILL		0xA5A5A5A5u	// FreeRTOS' fill byte, so a memory dump reads the same as a task stack
#define MSP_STACK_CANARY_WORDS	4u
#define MSP_STACK_FILL_GAP	64u		// bytes below the stack pointer the fill leaves alone (the filler's frame)

extern uint32_t _end[], _estack[];

// From the reset handler, after .bss is cleared: every word from `_end` to a little below the stack pointer
static inline __attribute__((always_inline)) void msp_stack_fill(void)
{
	uint32_t sp;

	__asm__ volatile ("mov %0, sp" : "=r" (sp));
	for (volatile uint32_t *w = _end; (uint32_t)(uintptr_t)w + MSP_STACK_FILL_GAP < sp; w++) {
		*w = MSP_STACK_FILL;
	}
}

// The lowest words still hold the fill: false means a handler reached the end of the stack (or beyond)
static inline bool msp_stack_canary_ok(void)
{
	const volatile uint32_t *w = _end;

	for (uint32_t i = 0; i < MSP_STACK_CANARY_WORDS; i++) {
		if (w[i] != MSP_STACK_FILL) {
			return false;
		}
	}
	return true;
}

// Bytes never written since the fill, counted up from the bottom (the stack grows down)
static inline uint32_t msp_stack_free(void)
{
	const volatile uint32_t *w = _end;
	uint32_t words = (uint32_t)(_estack - _end);
	uint32_t n = 0;

	while (n < words && w[n] == MSP_STACK_FILL) {
		n++;
	}
	return n * 4u;
}

#endif
