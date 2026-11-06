// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The interrupt stack's fill, canary and high-water count (platform/hrc7000/irq_stack.h).
#include <stdint.h>
#include <stdio.h>
#include "../../platform/hrc7000/irq_stack.h"

static int failures;

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

#define WORDS	512u

int main(void)
{
	static uint32_t stack[WORDS];

	irq_stack_fill(stack, WORDS);
	CHECK(irq_stack_canary_ok(stack));
	CHECK(irq_stack_free_words(stack, WORDS) == WORDS);

	// Handlers used the top 100 words: the rest is free, the canary intact
	for (uint32_t i = WORDS - 100u; i < WORDS; i++) {
		stack[i] = i;
	}
	CHECK(irq_stack_free_words(stack, WORDS) == WORDS - 100u);
	CHECK(irq_stack_canary_ok(stack));

	// A frame reached the canary's top word: caught, and the free count is 3 words
	stack[IRQ_STACK_CANARY_WORDS - 1u] = 0;
	CHECK(!irq_stack_canary_ok(stack));
	CHECK(irq_stack_free_words(stack, WORDS) == IRQ_STACK_CANARY_WORDS - 1u);

	// The bottom word alone (a store past a frame that skipped the rest): still caught
	irq_stack_fill(stack, WORDS);
	stack[0] = 0x12345678u;
	CHECK(!irq_stack_canary_ok(stack));
	CHECK(irq_stack_free_words(stack, WORDS) == 0);

	printf("hrc7000IrqStackTest: %s\n", failures ? "FAILED" : "passed");
	return failures != 0;
}
