// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The main stack's fill and canary (platform.h). Once the scheduler runs, the main stack (MSP) holds only the interrupt
// handlers' frames: the tasks run on their own stacks, and starting the scheduler sets MSP back to the vector table's
// initial value (_estack), dropping main()'s frames. The stack has no end of its own: it may grow down to the end of
// .bss (common.ld __main_stack_bottom), and past that it overwrites data. FreeRTOS checks the task stacks only, so
// main() fills the free part first, the tick checks the lowest words, and the debug link reads how much of the fill was
// never touched (the same scheme as the HR_C7000's interrupt stack).
#include "config.h"
#include "platform.h"
#include "FreeRTOS.h"
#include "task.h"

#define FILL		0xA5A5A5A5u	// FreeRTOS' fill byte, so a memory dump reads the same as a task stack
#define CANARY_WORDS	4u
// Left unfilled below the stack pointer at the fill. Nothing below it is live (an interrupt taken meanwhile pushes its
// frame there and is gone again before the loop goes on), so this is only slack
#define FILL_MARGIN	64u

extern uint32_t __main_stack_bottom[];	// common.ld
extern uint32_t _estack[];

// From main(), on the main stack itself: up to the margin below where it runs
void stm32_irq_stack_fill(void)
{
	uintptr_t sp;
	__asm volatile ("mov %0, sp" : "=r"(sp));
	volatile uint32_t *end = (volatile uint32_t *)((sp - FILL_MARGIN) & ~(uintptr_t)3u);
	for (volatile uint32_t *p = __main_stack_bottom; p < end; p++) {
		*p = FILL;
	}
}

// From the tick hook: the tick interrupt on this stack, or a task catching up on ticks. Either way the capture has the
// room the overflowing handler didn't
void stm32_irq_stack_check(void)
{
	const volatile uint32_t *b = __main_stack_bottom;
	for (uint32_t i = 0; i < CANARY_WORDS; i++) {
		if (b[i] != FILL) {
			vApplicationStackOverflowHook(NULL, (char *)"irq");
			return;
		}
	}
}

// Bytes never written since the fill, counted up from the bottom (the stack grows down)
uint32_t stm32_irq_stack_free(void)
{
	const volatile uint32_t *p = __main_stack_bottom;
	while (p < _estack && *p == FILL) {
		p++;
	}
	return (uint32_t)((uintptr_t)p - (uintptr_t)__main_stack_bottom);
}
