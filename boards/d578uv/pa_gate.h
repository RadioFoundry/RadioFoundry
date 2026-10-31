// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// AT-D578UV DMR transmit: what the PA switch does at each event while the RF chips stay keyed for a call
// (docs/spec/hardware-d578uv.md 5.1). Pure, so the host tests check it; radio_d578uv.c applies it in its interrupts.
//
// The baseband's burst line (PD0) is high while a burst goes out. Its rising edge turns the PA on (the DAC level, then
// PE6), its falling edge drops the DAC at once and PE6 after a short one-shot timer, as the stock firmware does. Every
// decision first asks whether a DMR transmission is still keyed: once it has ended nothing turns the PA on again.
#ifndef D578UV_PA_GATE_H
#define D578UV_PA_GATE_H

#include <stdbool.h>

typedef enum {
	PA_GATE_NONE,	// leave it as it is
	PA_GATE_ON,	// timer stopped, DAC at the level, PE6 high
	PA_GATE_DOWN,	// DAC 0 now, the timer started; PE6 stays high until it fires
	PA_GATE_OFF,	// timer stopped, DAC 0, PE6 low
} pa_gate_t;

// An edge of the burst line (either: the handler reads the level, so edges that came together count as the last)
static inline pa_gate_t pa_gate_edge(bool keyed, bool burst)
{
	if (!keyed) {
		return PA_GATE_OFF;
	}
	return burst ? PA_GATE_ON : PA_GATE_DOWN;
}

// The one-shot timer after a falling edge: PE6 off, unless a new burst has started since
static inline pa_gate_t pa_gate_timer(bool keyed, bool burst)
{
	return keyed && burst ? PA_GATE_NONE : PA_GATE_OFF;
}

// Keying: whether the key-up may go on. The core keys the radio before it tells the baseband to transmit, so the burst
// line is low then; high, nobody drives it (it floats high on its pull-up with the baseband in reset or crashed) or
// its polarity isn't the inferred one, and the PA would carry an unmodulated carrier. The key-up is refused. Arming
// never turns the PA on: only a rising edge does
static inline bool pa_gate_can_key(bool burst)
{
	return !burst;
}

#endif
