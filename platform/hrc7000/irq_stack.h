// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The interrupt stack's fill and canary (irq.c). FreeRTOS checks the task stacks only; the handlers run on a stack of
// their own (freertos/portasm.S), and the .ramfunc code (the DMR slot engine) lies right below it, so an overflow
// would overwrite code and show up as anything but a crash. The stack is filled at start-up, the tick checks its
// lowest words, and the debug link reads how much of the fill was never touched.
#ifndef HRC_IRQ_STACK_H
#define HRC_IRQ_STACK_H

#include <stdbool.h>
#include <stdint.h>

#define IRQ_STACK_FILL		0xA5A5A5A5u	// FreeRTOS' fill byte, so a memory dump reads the same as a task stack
#define IRQ_STACK_CANARY_WORDS	4u

static inline void irq_stack_fill(uint32_t *bottom, uint32_t words)
{
	for (uint32_t i = 0; i < words; i++) {
		bottom[i] = IRQ_STACK_FILL;
	}
}

// The lowest words still hold the fill: false means a handler reached the end of the stack (or beyond)
static inline bool irq_stack_canary_ok(const volatile uint32_t *bottom)
{
	for (uint32_t i = 0; i < IRQ_STACK_CANARY_WORDS; i++) {
		if (bottom[i] != IRQ_STACK_FILL) {
			return false;
		}
	}
	return true;
}

// Words never written since the fill, counted up from the bottom (the stack grows down)
static inline uint32_t irq_stack_free_words(const volatile uint32_t *bottom, uint32_t words)
{
	uint32_t n = 0;

	while (n < words && bottom[n] == IRQ_STACK_FILL) {
		n++;
	}
	return n;
}

#endif
