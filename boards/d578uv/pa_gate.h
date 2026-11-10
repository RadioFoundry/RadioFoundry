// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// AT-D578UV DMR transmit: what the PA switch does at each event while the RF chips stay keyed for a call
// (docs/spec/hardware-d578uv.md 5.1). Pure, so the host tests check it; radio_d578uv.c applies it in its interrupts.
//
// The baseband's burst line (PD0) is high for 30 ms per burst, but the burst's modulation starts 7.0 ms after its
// rising edge and ends 4.5 ms after its falling edge (measured). A PA that followed PD0 cut off the burst's last 4 ms
// (16 to 25 bit errors in every LC header) and keyed 5.75 ms into the slot before. So the PA follows PD0 late, on one
// one-shot timer. After the rising edge it runs the key-up's steps (core/radio/pa_keyup.h, docs/spec/pa-keyup.md),
// timed from the burst's first symbol PA_FIRST_SYMBOL_US after the edge: the board's constants key the PA at the step's
// drive 1.25 ms before the burst, as before the shared rule (PA_ON_DELAY_US), until a boost is tuned. PA_OFF_DELAY_US
// after the falling edge the DAC goes to 0, and PA_TAIL_US later PE6. Every decision first asks whether a DMR
// transmission is still keyed: once it has ended nothing turns the PA on again.
#ifndef D578UV_PA_GATE_H
#define D578UV_PA_GATE_H

#include <stdbool.h>
#include <stdint.h>
#include "radio/pa_keyup.h"

#define PA_FIRST_SYMBOL_US	7000u	// the burst's modulation after PD0's rise (measured)
#define PA_ON_US		1250u	// the PA keyed this long before it without a boost (pa_keyup_cfg_t.pa_us)
#define PA_ON_DELAY_US		(PA_FIRST_SYMBOL_US - PA_ON_US)
#define PA_OFF_DELAY_US		5000u
#define PA_TAIL_US		800u	// PE6 after the DAC: TIMER1's 800 us one-shot

typedef enum {
	PA_GATE_NONE,	// leave the PA as it is
	PA_GATE_STEP,	// a key-up step (pa_keyup_step_t): the DAC to its drive, PE6 high, either or both
	PA_GATE_DOWN,	// DAC 0; PE6 stays high
	PA_GATE_OFF,	// DAC 0, PE6 low
} pa_gate_t;

typedef enum {
	PA_IDLE,	// off, waiting for a rising edge
	PA_RISING,	// the key-up's steps running
	PA_UP,		// on, PD0 high
	PA_FALLING,	// on, the off delay running
	PA_TAIL,	// DAC 0, PE6's tail running
} pa_phase_t;

// What to switch, and the one-shot to start (0: stop it)
typedef struct {
	pa_gate_t act;
	uint16_t timer_us;
	pa_keyup_step_t step;	// PA_GATE_STEP's
} pa_step_t;

// The gate's state, and a burst's steps: full from off, plain (no boost) for a burst that rises while the PA is still
// on from the one before, whose drive a boost would push over (back to back, a continuous transmission)
typedef struct {
	pa_phase_t ph;
	uint8_t next;		// the next step while rising
	bool plain;		// this burst runs the plain steps
} pa_gate_state_t;

typedef struct {
	pa_keyup_steps_t full, plain;
} pa_gate_plan_t;

// The key-up's steps for a transmission, from the constants it took and its plan (radio task, before arming)
static inline void pa_gate_plan(pa_gate_plan_t *p, const pa_keyup_cfg_t *c, const pa_keyup_t *k)
{
	pa_keyup_steps(c, k, true, PA_FIRST_SYMBOL_US, &p->full);
	pa_keyup_steps(c, k, false, PA_FIRST_SYMBOL_US, &p->plain);
}

static inline pa_step_t pa_step(pa_gate_t act, uint16_t timer_us)
{
	pa_step_t s = { act, timer_us, { 0 } };

	return s;
}

static inline const pa_keyup_steps_t *pa_gate_steps(const pa_gate_state_t *g, const pa_gate_plan_t *p)
{
	return g->plain ? &p->plain : &p->full;
}

// An edge of the burst line (either: the handler reads the level, so edges that came together count as the last)
static inline pa_step_t pa_gate_edge(pa_gate_state_t *g, const pa_gate_plan_t *p, bool keyed, bool burst)
{
	if (!keyed) {
		g->ph = PA_IDLE;
		return pa_step(PA_GATE_OFF, 0);
	}
	if (burst) {
		if (g->ph == PA_RISING || g->ph == PA_UP) {
			return pa_step(PA_GATE_NONE, 0);	// a repeat: the steps run on
		}
		// From the off delay or the tail the PA stays as it is until the plain step: no gap between bursts
		g->plain = g->ph == PA_FALLING || g->ph == PA_TAIL;
		g->next = 0;
		g->ph = PA_RISING;
		return pa_step(PA_GATE_NONE, pa_gate_steps(g, p)->s[0].at_us);
	}
	switch (g->ph) {
	case PA_UP:
		g->ph = PA_FALLING;
		return pa_step(PA_GATE_NONE, PA_OFF_DELAY_US);
	case PA_RISING:		// a pulse shorter than the key-up: no burst to carry, and a boost may be on
		g->ph = PA_IDLE;
		return pa_step(PA_GATE_OFF, 0);
	default:
		return pa_step(PA_GATE_NONE, 0);
	}
}

// The one-shot timer
static inline pa_step_t pa_gate_timer(pa_gate_state_t *g, const pa_gate_plan_t *p, bool keyed)
{
	if (!keyed) {
		g->ph = PA_IDLE;
		return pa_step(PA_GATE_OFF, 0);
	}
	switch (g->ph) {
	case PA_RISING: {
		const pa_keyup_steps_t *ss = pa_gate_steps(g, p);
		pa_step_t st = pa_step(PA_GATE_STEP, 0);
		st.step = ss->s[g->next++];
		if (g->next < ss->n) {
			st.timer_us = (uint16_t)(ss->s[g->next].at_us - st.step.at_us);
		} else {
			g->ph = PA_UP;
		}
		return st;
	}
	case PA_FALLING:
		g->ph = PA_TAIL;
		return pa_step(PA_GATE_DOWN, PA_TAIL_US);
	case PA_TAIL:
		g->ph = PA_IDLE;
		return pa_step(PA_GATE_OFF, 0);
	default:
		return pa_step(PA_GATE_NONE, 0);	// late, after a stop
	}
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
