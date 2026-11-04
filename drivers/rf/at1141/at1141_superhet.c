// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The AT1141 as a superhet's LO (rf/superhet/superhet.h): chip = an at1141_t. Off and on are the working firmware's
// live TX and RX steps: standby, then standby and on again, with no wait and no retune
#include "at1141.h"
#include "at1141_superhet.h"

static void tune(void *c, uint32_t hz)
{
	at1141_t *a = c;
	if (a->lo_hz != hz) {
		at1141_tune(a, hz);	// enables too
	} else {
		at1141_enable(a);
	}
}

static void off(void *c) { at1141_disable(c); }
static void resume(void *c) { at1141_enable(c); }
static void gpio(void *c, uint8_t pin, bool high) { at1141_gpio(c, pin, high); }

const superhet_lo_ops_t at1141_superhet_lo = {
	.tune = tune, .off = off, .resume = resume, .gpio = gpio,
};
