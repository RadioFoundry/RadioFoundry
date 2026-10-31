// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The GPIO interrupt watch (hardware-dm32.md 6.3): a board whose input depends on one GPIO bank interrupt (the rotary
// encoder) registers its handler here. A wrapper counts the entries, and the board's 10 ms input scan checks that a
// pending interrupt was taken. One left pending on two scans running with no entry between them has been lost (seen on
// a radio: the bank pending, its PIC source quiet for good). The first time, the PIC and bank state is kept for the
// debug link; every time, the scan serves the bank as the handler would and enables the source again.
#include "platform.h"
#include "hal_time.h"
#include "irq_stall.h"

static struct {
	const hrc_gpio_watch_t *w;
	uint32_t irqs, stalls, stall_ms;
	uint32_t pic[3];	// PIC_MASK, PIC_MASK_1, PIC_INT_ST at the first stall
	uint32_t gpio[3];	// the bank's INTSTATUS, INTEN, INTMASK at the first stall
	hrc_irq_stall_t check;
	bool handler_ok;
} watch;

static void watched_isr(void)
{
	watch.irqs++;
	watch.w->isr();
}

void hrc_gpio_watch_register(const hrc_gpio_watch_t *w)
{
	watch.w = w;
	hrc_irq_register(w->source, watched_isr);
}

void hrc_gpio_watch_scan(void)
{
	const hrc_gpio_watch_t *w = watch.w;
	if (w == NULL) {
		return;
	}
	// Interrupts off: the pending bits and the entry count are read together, and the service below must not race the
	// handler if the source comes back on its own
	uint32_t psr = hrc_irq_save();
	bool pending = (REG32(w->bank + GPIO_INTSTATUS) & w->bits) != 0;
	if (hrc_irq_stalled(&watch.check, pending, watch.irqs)) {
		if (watch.stalls == 0) {
			watch.stall_ms = hal_millis();
			watch.pic[0] = PIC_MASK;
			watch.pic[1] = PIC_MASK_1;
			watch.pic[2] = PIC_INT_ST;
			watch.gpio[0] = REG32(w->bank + GPIO_INTSTATUS);
			watch.gpio[1] = REG32(w->bank + GPIO_INTEN);
			watch.gpio[2] = REG32(w->bank + GPIO_INTMASK);
			watch.handler_ok = hrc_irq_table[w->source] == watched_isr;
		}
		watch.stalls++;
		w->isr();					// sample and ack, as the interrupt would have
		hrc_irq_table[w->source] = watched_isr;		// in case it was pointed elsewhere
		hrc_irq_enable(w->source, true);
	}
	hrc_irq_restore(psr);
}

void hrc_gpio_watch_stats(hal_encoder_stats_t *st)
{
	if (watch.w == NULL) {
		return;
	}
	st->irq_watch = true;
	st->irqs = watch.irqs;
	st->stalls = watch.stalls;
	st->stall_ms = watch.stall_ms;
	st->stall_irq_mask[0] = watch.pic[0];
	st->stall_irq_mask[1] = watch.pic[1];
	st->stall_irq_pending = watch.pic[2];
	st->stall_gpio_pending = watch.gpio[0];
	st->stall_gpio_enabled = watch.gpio[1];
	st->stall_gpio_masked = watch.gpio[2];
	st->stall_handler_ok = watch.handler_ok;
}
