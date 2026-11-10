// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The AT-D578UV's per-burst PA switching in DMR transmit (pa_gate.h, hardware-d578uv.md 5.1): a model of PE6, the DAC
// and the one-shot timer run on a microsecond clock through bursts, a burst under way at the key-up, and the end of the
// transmission, with the board's key-up (the PA at the step's drive) and with a boost (radio/pa_keyup.h). The PA must cover each burst's modulation, which lags the burst line (7.0 ms after its rise to 4.5 ms
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
	unsigned peak;		// the highest DAC word since the last rise from 0
	pa_gate_state_t g;
	pa_gate_plan_t plan;
	unsigned long now, fire;	// us; fire 0 = the timer stopped
	unsigned long on_at, off_at;	// the last DAC rise and fall
} pa_t;

static void apply(pa_t *p, pa_gate_t a, const pa_keyup_step_t *k)
{
	switch (a) {
	case PA_GATE_STEP:
		if (k->set_drive) {
			if (p->dac == 0 && k->drive != 0) {
				p->on_at = p->now;
				p->peak = 0;
			}
			p->dac = k->drive;
			p->peak = p->dac > p->peak ? p->dac : p->peak;
		}
		if (k->pa_on)
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
	apply(p, st.act, &st.step);
}

static void run_to(pa_t *p, unsigned long t)
{
	while (p->fire != 0 && p->fire <= t) {
		p->now = p->fire;
		p->fire = 0;
		step(p, pa_gate_timer(&p->g, &p->plan, p->keyed), false);
	}
	p->now = t;
}

static void edge(pa_t *p, bool level)
{
	p->burst = level;
	step(p, pa_gate_edge(&p->g, &p->plan, p->keyed, level), true);
}

// The radio glue's disarm: the flag first, then the PA off
static void disarm(pa_t *p)
{
	p->keyed = false;
	apply(p, PA_GATE_OFF, NULL);
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

// The board's constants (radio_d578uv.c): no boost, the PA at the step's drive PA_ON_US ahead
static const pa_keyup_cfg_t board_cfg = {
	.dead = 190, .knee = 1450, .slope_us = 600, .boost_pct = 120, .hold_pct = 80,
	.boost_us = 0, .pa_us = PA_ON_US, .drive_us = 0,
};
// A tuned form: the boost from 1.5 ms ahead, the PA with it, the step's drive 0.5 ms ahead
static const pa_keyup_cfg_t boost_cfg = {
	.dead = 190, .knee = 1450, .slope_us = 600, .boost_pct = 120, .hold_pct = 80,
	.boost_us = 1500, .pa_us = 1500, .drive_us = 500,
};
#define TOP		2960

static pa_t fresh(const pa_keyup_cfg_t *c, bool keyed)
{
	pa_t p = { .keyed = keyed };
	pa_keyup_t k = pa_keyup_drives(c, LEVEL, TOP, 0xFFF);
	pa_gate_plan(&p.plan, c, &k);
	return p;
}

int main(void)
{
	pa_t p = fresh(&board_cfg, false);
	CHECK(pa_keyup_valid(&board_cfg, 3000) && pa_keyup_valid(&boost_cfg, 3000));
	CHECK(p.plan.full.n == 1 && p.plan.full.s[0].at_us == PA_ON_DELAY_US);	// one step, as before the shared rule

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
	p.g.ph = PA_IDLE;
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
	CHECK(pa_gate_timer(&p.g, &p.plan, false).act == PA_GATE_OFF);
	CHECK(off(&p));

	// Ended during the on delay, and during the off delay
	p = fresh(&board_cfg, true);
	edge(&p, true);
	disarm(&p);
	CHECK(pa_gate_timer(&p.g, &p.plan, p.keyed).act == PA_GATE_OFF);
	p = fresh(&board_cfg, true);
	edge(&p, true);
	run_to(&p, PA_ON_DELAY_US);
	edge(&p, false);
	disarm(&p);
	CHECK(pa_gate_timer(&p.g, &p.plan, p.keyed).act == PA_GATE_OFF);
	CHECK(off(&p));

	// The burst line high at key-up (undriven on its pull-up, or inverted): the key-up is refused, the PA stays off
	CHECK(!pa_gate_can_key(true));

	// With a boost: the boost and PE6 1.5 ms before the burst, the hold, then the step's drive 0.5 ms before; each
	// burst's DAC rises to the boost and is at the level by the modulation
	p = fresh(&boost_cfg, true);
	pa_keyup_t k = pa_keyup_drives(&boost_cfg, LEVEL, TOP, 0xFFF);
	CHECK(k.boost == TOP * 120 / 100 && k.hold == LEVEL * 80 / 100 && k.hold_us < 1500 && k.hold_us > 500);
	CHECK(p.plan.full.n == 3 && p.plan.plain.n == 1);
	p.now = 5000000u;
	edge(&p, true);
	run_to(&p, p.now + MOD_START_US - 1500u - 1u);
	CHECK(p.dac == 0 && !p.pe6);
	run_to(&p, p.now + 1u);
	CHECK(p.dac == k.boost && p.pe6);
	run_to(&p, 5000000u + MOD_START_US - k.hold_us);
	CHECK(p.dac == k.hold && p.pe6);
	run_to(&p, 5000000u + MOD_START_US - 500u);
	CHECK(p.dac == LEVEL && p.fire == 0);
	run_to(&p, 5000000u + PD0_HIGH_US);
	edge(&p, false);
	run_to(&p, p.now + PA_OFF_DELAY_US + PA_TAIL_US);
	CHECK(off(&p));
	for (int i = 0; i < 3; i++) {
		unsigned long t = 6000000u + 60000u * (unsigned long)i;
		run_to(&p, t);
		edge(&p, true);
		run_to(&p, t + MOD_START_US);
		CHECK(p.dac == LEVEL && p.peak == k.boost && p.pe6);
		CHECK(p.on_at == t + MOD_START_US - 1500u);
		run_to(&p, t + PD0_HIGH_US);
		edge(&p, false);
		run_to(&p, t + PD0_HIGH_US + 10000u);
		CHECK(off(&p));
	}

	// Back to back with a boost: the PA still on from the burst before, so no boost over it; the plain step keys it
	p.now = 7000000u;
	edge(&p, true);
	run_to(&p, p.now + PD0_HIGH_US);
	edge(&p, false);
	run_to(&p, p.now + 1000u);
	edge(&p, true);
	CHECK(p.g.plain && p.dac == LEVEL);
	run_to(&p, p.now + MOD_START_US);
	CHECK(p.dac == LEVEL && p.pe6 && p.fire == 0);

	// Ended in the boost, or the burst line falling in it: the PA off at once, not at the boost's drive
	run_to(&p, p.now + PD0_HIGH_US);
	edge(&p, false);
	run_to(&p, p.now + PA_OFF_DELAY_US + PA_TAIL_US);
	edge(&p, true);
	run_to(&p, p.now + MOD_START_US - 1400u);
	CHECK(p.dac == k.boost);
	edge(&p, false);
	CHECK(off(&p));
	edge(&p, true);
	run_to(&p, p.now + MOD_START_US - 1400u);
	CHECK(p.dac == k.boost);
	disarm(&p);
	CHECK(off(&p));
	run_to(&p, p.now + 2000u);
	CHECK(off(&p));

	if (failures) {
		printf("d578uvPaGateTest: %d failure(s)\n", failures);
		return 1;
	}
	printf("d578uvPaGateTest: all passed\n");
	return 0;
}
