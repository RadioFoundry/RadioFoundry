// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The lost-interrupt test of the GPIO interrupt watch (gpio_watch.c, hardware-dm32.md 6.3), no hardware access (host
// tested). With interrupts on, a pending GPIO interrupt is served within microseconds; one pending on two scans
// running, 10 ms apart, with no handler entry between them, has not reached the CPU: its PIC source is masked or its
// vector points elsewhere.
#ifndef HRC_IRQ_STALL_H
#define HRC_IRQ_STALL_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
	uint8_t last_irqs;	// the handler entry count at the last scan, low byte: 256 entries in one scan interval
				// would hide a stall for one scan, and a false stall only serves the bank once more
	bool was_pending;	// pending at the last scan
} hrc_irq_stall_t;

// One scan: pending = the watched bits are pending in the bank now, irqs = handler entries so far. True = stalled.
// The scan after a stall starts afresh (it takes two more to call another)
static inline bool hrc_irq_stalled(hrc_irq_stall_t *s, bool pending, uint32_t irqs)
{
	bool stall = pending && s->was_pending && (uint8_t)irqs == s->last_irqs;
	s->was_pending = pending && !stall;
	s->last_irqs = (uint8_t)irqs;
	return stall;
}

#endif
