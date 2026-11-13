// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// AT-D578UV DMR transmit: what the PA switch does at each event while the RF chips stay keyed for a call
// (docs/spec/hardware-d578uv.md 5.1). Pure, so the host tests check it; radio_d578uv.c applies it in its interrupts.
//
// The baseband's burst line (PD0) is high for 30 ms per burst, but the burst's modulation starts 7.0 ms after its
// rising edge and ends 4.5 ms after its falling edge (measured). A PA that followed PD0 cut off the burst's last 4 ms
// (16 to 25 bit errors in every LC header) and keyed 5.75 ms into the slot before. So the PA follows PD0 late, on one
// one-shot timer: PA_ON_DELAY_US after the rising edge the DAC level and PE6, PA_OFF_DELAY_US after the falling edge
// the DAC to 0, and PA_TAIL_US later PE6. The RF is then on from 1.25 ms before the burst to about 1 ms after it.
// Every decision first asks whether a DMR transmission is still keyed: once it has ended nothing turns the PA on
// again.
#ifndef D578UV_PA_GATE_H
#define D578UV_PA_GATE_H

#include <stdbool.h>
#include <stdint.h>

#define PA_ON_DELAY_US		5750u
#define PA_OFF_DELAY_US		5000u
#define PA_TAIL_US		800u	// PE6 after the DAC: TIMER1's 800 us one-shot

typedef enum {
	PA_GATE_NONE,	// leave the PA as it is
	PA_GATE_ON,	// DAC at the level, PE6 high
	PA_GATE_DOWN,	// DAC 0; PE6 stays high
	PA_GATE_OFF,	// DAC 0, PE6 low
} pa_gate_t;

typedef enum {
	PA_IDLE,	// off, waiting for a rising edge
	PA_RISING,	// the on delay running
	PA_UP,		// on, PD0 high
	PA_FALLING,	// on, the off delay running
	PA_TAIL,	// DAC 0, PE6's tail running
} pa_phase_t;

// What to switch, and the one-shot to start (0: stop it)
typedef struct {
	pa_gate_t act;
	uint16_t timer_us;
} pa_step_t;

static inline pa_step_t pa_step(pa_gate_t act, uint16_t timer_us)
{
	pa_step_t s = { act, timer_us };

	return s;
}

// An edge of the burst line (either: the handler reads the level, so edges that came together count as the last)
static inline pa_step_t pa_gate_edge(pa_phase_t *ph, bool keyed, bool burst)
{
	if (!keyed) {
		*ph = PA_IDLE;
		return pa_step(PA_GATE_OFF, 0);
	}
	if (burst) {
		if (*ph == PA_RISING || *ph == PA_UP) {
			return pa_step(PA_GATE_NONE, 0);	// a repeat: the delay runs on
		}
		// From the off delay or the tail the PA stays as it is until the on delay ends: no gap between bursts
		*ph = PA_RISING;
		return pa_step(PA_GATE_NONE, PA_ON_DELAY_US);
	}
	switch (*ph) {
	case PA_UP:
		*ph = PA_FALLING;
		return pa_step(PA_GATE_NONE, PA_OFF_DELAY_US);
	case PA_RISING:		// a pulse shorter than the on delay: no burst to carry
		*ph = PA_IDLE;
		return pa_step(PA_GATE_OFF, 0);
	default:
		return pa_step(PA_GATE_NONE, 0);
	}
}

// The one-shot timer
static inline pa_step_t pa_gate_timer(pa_phase_t *ph, bool keyed)
{
	if (!keyed) {
		*ph = PA_IDLE;
		return pa_step(PA_GATE_OFF, 0);
	}
	switch (*ph) {
	case PA_RISING:
		*ph = PA_UP;
		return pa_step(PA_GATE_ON, 0);
	case PA_FALLING:
		*ph = PA_TAIL;
		return pa_step(PA_GATE_DOWN, PA_TAIL_US);
	case PA_TAIL:
		*ph = PA_IDLE;
		return pa_step(PA_GATE_OFF, 0);
	default:
		return pa_step(PA_GATE_NONE, 0);	// late, after a stop
	}
}

// The burst line's edges the driver may count as done (the modem port's burst_edges): every one, but a fall whose
// burst is still on the air. The modulation runs 4.5 ms past PD0's fall and the PA with it (PA_FALLING, PA_TAIL);
// the driver ends a data transmission at the fall that completes its last burst, and ending at PD0's fall cut that
// burst's end off (bit errors in the last block)
static inline uint32_t pa_gate_edges_done(uint32_t edges, pa_phase_t ph, bool level)
{
	return !level && (ph == PA_FALLING || ph == PA_TAIL) && edges != 0 ? edges - 1u : edges;
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
