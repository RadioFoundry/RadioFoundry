// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The AT-D578UV's per-burst PA switching in DMR transmit (pa_gate.h, hardware-d578uv.md 5.1): a model of PE6, the DAC
// and the one-shot timer run on a microsecond clock through bursts, a burst under way at the key-up, and the end of the
// transmission. The PA must cover each burst's modulation, which lags the burst line (7.0 ms after its rise to 4.5 ms
// after its fall, measured), and stay off for most of the other slot
#include <stdio.h>
#include "pa_gate.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

#define LEVEL		0x5A0
#define MOD_START_US	7000u	// the burst's modulation after PD0's rising edge
#define MOD_END_US	4500u	// and after its falling edge
#define PD0_HIGH_US	30000u

typedef struct {
	bool keyed, burst;	// the guard flag, PD0
	bool pe6;
	unsigned dac;
	pa_phase_t ph;
	unsigned long now, fire;	// us; fire 0 = the timer stopped
	unsigned long on_at, off_at;	// the last DAC rise and fall
} pa_t;

static void apply(pa_t *p, pa_gate_t a)
{
	switch (a) {
	case PA_GATE_ON:
		if (p->dac == 0)
			p->on_at = p->now;
		p->dac = LEVEL;
		p->pe6 = true;
		break;
	case PA_GATE_DOWN:
		if (p->dac != 0)
			p->off_at = p->now;
		p->dac = 0;
		break;
	case PA_GATE_OFF:
		if (p->dac != 0)
			p->off_at = p->now;
		p->fire = 0;
		p->dac = 0;
		p->pe6 = false;
		break;
	case PA_GATE_NONE:
		break;
	}
}

// As the board's burst_step: a timer to start, else a stop (but for an edge that changes nothing)
static void step(pa_t *p, pa_step_t st, bool from_edge)
{
	if (from_edge && st.act == PA_GATE_NONE && st.timer_us == 0)
		return;
	if (st.timer_us != 0)
		p->fire = p->now + st.timer_us;
	else
		p->fire = 0;
	apply(p, st.act);
}

static void run_to(pa_t *p, unsigned long t)
{
	while (p->fire != 0 && p->fire <= t) {
		p->now = p->fire;
		p->fire = 0;
		step(p, pa_gate_timer(&p->ph, p->keyed), false);
	}
	p->now = t;
}

static void edge(pa_t *p, bool level)
{
	p->burst = level;
	step(p, pa_gate_edge(&p->ph, p->keyed, level), true);
}

// The radio glue's disarm: the flag first, then the PA off
static void disarm(pa_t *p)
{
	p->keyed = false;
	apply(p, PA_GATE_OFF);
}

static bool off(const pa_t *p)
{
	return !p->pe6 && p->dac == 0 && p->fire == 0;
}

// One burst from PD0's rise at t; checks the PA covers the modulation with a guard, and is off soon after
static void burst(pa_t *p, unsigned long t)
{
	run_to(p, t);
	edge(p, true);
	run_to(p, t + PD0_HIGH_US);
	edge(p, false);
	run_to(p, t + PD0_HIGH_US + 10000u);
	unsigned long mod_on = t + MOD_START_US, mod_off = t + PD0_HIGH_US + MOD_END_US;
	CHECK(p->on_at <= mod_on - 1000u && p->on_at >= mod_on - 1500u);	// on 1 to 1.5 ms before the burst
	CHECK(p->off_at >= mod_off && p->off_at <= mod_off + 1000u);		// off within 1 ms after it
	CHECK(off(p));
}

int main(void)
{
	pa_t p = { 0 };

	// Not keyed: an edge never turns anything on
	edge(&p, true);
	CHECK(off(&p));
	edge(&p, false);
	CHECK(off(&p));

	// Keyed between bursts: nothing until the first rising edge, and nothing for the on delay after it
	CHECK(pa_gate_can_key(p.burst));
	p.keyed = true;
	p.now = 1000000u;
	CHECK(off(&p));
	edge(&p, true);
	CHECK(p.dac == 0 && !p.pe6);
	run_to(&p, p.now + PA_ON_DELAY_US - 1u);
	CHECK(p.dac == 0 && !p.pe6);
	run_to(&p, p.now + 1u);
	CHECK(p.dac == LEVEL && p.pe6);
	run_to(&p, 1000000u + PD0_HIGH_US);
	edge(&p, false);
	CHECK(p.dac == LEVEL && p.pe6);		// the burst's tail still going out
	run_to(&p, p.now + PA_OFF_DELAY_US);
	CHECK(p.dac == 0 && p.pe6);		// the DAC first, PE6 after its tail
	run_to(&p, p.now + PA_TAIL_US);
	CHECK(off(&p));

	// Bursts one slot in two (a call), and headers one slot in four
	for (int i = 0; i < 4; i++)
		burst(&p, 2000000u + 60000u * (unsigned long)i);
	for (int i = 0; i < 3; i++)
		burst(&p, 3000000u + 120000u * (unsigned long)i);

	// Off within the other slot: the PA is off before the next slot's burst line rises
	CHECK(PA_OFF_DELAY_US + PA_TAIL_US < 30000u);

	// Back to back (continuous transmission): the PA stays on across the gap
	p.now = 4000000u;
	edge(&p, true);
	run_to(&p, p.now + PD0_HIGH_US);
	edge(&p, false);
	run_to(&p, p.now + 1000u);
	edge(&p, true);
	CHECK(p.dac == LEVEL && p.pe6);
	run_to(&p, p.now + PA_ON_DELAY_US + 1u);
	CHECK(p.dac == LEVEL && p.pe6 && p.fire == 0);

	// A repeated rising edge leaves the running delay alone
	run_to(&p, p.now + PD0_HIGH_US);
	edge(&p, false);
	run_to(&p, p.now + PA_OFF_DELAY_US + PA_TAIL_US);
	CHECK(off(&p));
	edge(&p, true);
	unsigned long due = p.fire;
	run_to(&p, p.now + 100u);
	edge(&p, true);
	CHECK(p.fire == due);

	// A pulse shorter than the on delay carries no burst: the PA stays off
	run_to(&p, p.now + 2000000u);
	p.ph = PA_IDLE;
	edge(&p, true);
	run_to(&p, p.now + 1000u);
	edge(&p, false);
	run_to(&p, p.now + 10000u);
	CHECK(off(&p));

	// The end of the transmission in the middle of a burst: off, and a late edge or timer can't bring it back
	edge(&p, true);
	run_to(&p, p.now + PA_ON_DELAY_US + 1000u);
	CHECK(p.pe6);
	disarm(&p);
	CHECK(off(&p));
	edge(&p, true);
	CHECK(off(&p));
	CHECK(pa_gate_timer(&p.ph, false).act == PA_GATE_OFF);
	CHECK(off(&p));

	// The edges done (the modem's early end): a fall counts once the PA's off delay and tail have run, the burst's
	// modulation with them; a rise and a fall with no burst after it count at once
	p = (pa_t){ .keyed = true };
	uint32_t n = 0;
	edge(&p, true);
	n++;
	CHECK(pa_gate_edges_done(n, p.ph, p.burst) == 1);
	run_to(&p, PD0_HIGH_US);
	edge(&p, false);
	n++;
	CHECK(pa_gate_edges_done(n, p.ph, p.burst) == 1);	// PA_FALLING: the burst still going out
	run_to(&p, PD0_HIGH_US + PA_OFF_DELAY_US);
	CHECK(p.ph == PA_TAIL && pa_gate_edges_done(n, p.ph, p.burst) == 1);
	run_to(&p, PD0_HIGH_US + PA_OFF_DELAY_US + PA_TAIL_US);
	CHECK(off(&p) && pa_gate_edges_done(n, p.ph, p.burst) == 2);
	CHECK(p.off_at >= PD0_HIGH_US + MOD_END_US);		// done only after the modulation ended
	edge(&p, true);					// the next burst in the off delay: its rise counts
	CHECK(pa_gate_edges_done(3, p.ph, p.burst) == 3);
	CHECK(pa_gate_edges_done(0, PA_FALLING, false) == 0);
	p = (pa_t){ .keyed = true };
	edge(&p, true);
	run_to(&p, 1000u);
	edge(&p, false);					// a pulse with no burst: PA_IDLE, done at once
	CHECK(pa_gate_edges_done(2, p.ph, p.burst) == 2);

	// Ended during the on delay, and during the off delay
	p = (pa_t){ .keyed = true };
	edge(&p, true);
	disarm(&p);
	CHECK(pa_gate_timer(&p.ph, p.keyed).act == PA_GATE_OFF);
	p = (pa_t){ .keyed = true };
	edge(&p, true);
	run_to(&p, PA_ON_DELAY_US);
	edge(&p, false);
	disarm(&p);
	CHECK(pa_gate_timer(&p.ph, p.keyed).act == PA_GATE_OFF);
	CHECK(off(&p));

	// The burst line high at key-up (undriven on its pull-up, or inverted): the key-up is refused, the PA stays off
	CHECK(!pa_gate_can_key(true));

	if (failures) {
		printf("d578uvPaGateTest: %d failure(s)\n", failures);
		return 1;
	}
	printf("d578uvPaGateTest: all passed\n");
	return 0;
}
