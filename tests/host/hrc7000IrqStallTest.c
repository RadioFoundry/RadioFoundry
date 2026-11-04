// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The GPIO interrupt watch's lost-interrupt test (platform/hrc7000/irq_stall.h): a stall is the bank pending on two
// scans running with no handler entry between them, never a pending edge the handler is still on its way to.
#include <stdint.h>
#include <stdio.h>
#include "../../platform/hrc7000/irq_stall.h"

static int failures;

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

int main(void)
{
	hrc_irq_stall_t s = { 0 };

	// Quiet: nothing pending, no entries
	for (int i = 0; i < 5; i++) {
		CHECK(!hrc_irq_stalled(&s, false, 0));
	}
	// Pending once, then served: no stall
	CHECK(!hrc_irq_stalled(&s, true, 0));
	CHECK(!hrc_irq_stalled(&s, false, 1));
	// Pending on two scans with an entry between them (a knob turning fast): no stall
	CHECK(!hrc_irq_stalled(&s, true, 5));
	CHECK(!hrc_irq_stalled(&s, true, 9));
	CHECK(!hrc_irq_stalled(&s, true, 12));
	// The handler stops: the second scan without an entry is the stall
	CHECK(!hrc_irq_stalled(&s, true, 20));
	CHECK(hrc_irq_stalled(&s, true, 20));
	// After a stall the next scan starts afresh, even if the service didn't clear it
	CHECK(!hrc_irq_stalled(&s, true, 20));
	CHECK(hrc_irq_stalled(&s, true, 20));
	// A gap clears the history
	CHECK(!hrc_irq_stalled(&s, true, 20));
	CHECK(!hrc_irq_stalled(&s, false, 20));
	CHECK(!hrc_irq_stalled(&s, true, 20));
	CHECK(hrc_irq_stalled(&s, true, 20));
	// The entry count wraps through its low byte and the 32-bit total
	hrc_irq_stall_t w = { 0 };
	CHECK(!hrc_irq_stalled(&w, true, 0xFFFFFFFFu));
	CHECK(!hrc_irq_stalled(&w, true, 0));
	CHECK(hrc_irq_stalled(&w, true, 0));
	// Exactly 256 entries between scans look like none: one extra service, never a missed stall later
	hrc_irq_stall_t x = { 0 };
	CHECK(!hrc_irq_stalled(&x, true, 0));
	CHECK(hrc_irq_stalled(&x, true, 256));

	if (failures == 0) {
		printf("hrc7000IrqStallTest: all passed\n");
	}
	return failures != 0;
}
