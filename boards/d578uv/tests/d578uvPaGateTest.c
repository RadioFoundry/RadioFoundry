// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The AT-D578UV's per-burst PA switching in DMR transmit (pa_gate.h, hardware-d578uv.md 5.1): a model of PE6, the DAC
// and the one-shot timer driven through bursts, a burst under way at the key-up, and the end of the transmission
#include <stdio.h>
#include "pa_gate.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

#define LEVEL	0x5A0

typedef struct {
	bool keyed, burst;	// the guard flag, PD0
	bool pe6, timer;
	unsigned dac;
} pa_t;

static void apply(pa_t *p, pa_gate_t a)
{
	switch (a) {
	case PA_GATE_ON:
		p->timer = false;
		p->dac = LEVEL;
		p->pe6 = true;
		break;
	case PA_GATE_DOWN:
		p->dac = 0;
		p->timer = true;
		break;
	case PA_GATE_OFF:
		p->timer = false;
		p->dac = 0;
		p->pe6 = false;
		break;
	case PA_GATE_NONE:
		break;
	}
}

static void edge(pa_t *p, bool level)
{
	p->burst = level;
	apply(p, pa_gate_edge(p->keyed, level));
}

static void timer_fires(pa_t *p)
{
	if (p->timer) {
		p->timer = false;
		apply(p, pa_gate_timer(p->keyed, p->burst));
	}
}

// The radio glue's disarm: the flag first, then the PA off
static void disarm(pa_t *p)
{
	p->keyed = false;
	apply(p, PA_GATE_OFF);
}

static bool off(const pa_t *p)
{
	return !p->pe6 && p->dac == 0 && !p->timer;
}

int main(void)
{
	pa_t p = { 0 };

	// Not keyed: an edge never turns anything on
	edge(&p, true);
	CHECK(off(&p));
	edge(&p, false);
	CHECK(off(&p));

	// Keyed between bursts: nothing until the first rising edge
	CHECK(pa_gate_can_key(p.burst));
	p.keyed = true;
	CHECK(off(&p));
	for (int i = 0; i < 3; i++) {
		edge(&p, true);
		CHECK(p.pe6 && p.dac == LEVEL && !p.timer);
		edge(&p, false);
		CHECK(p.pe6 && p.dac == 0 && p.timer);	// the DAC drops at once, PE6 with the timer
		timer_fires(&p);
		CHECK(off(&p));
	}

	// A new burst before the timer: PE6 stays, the timer is cancelled (and would leave PE6 if it fired late)
	edge(&p, true);
	edge(&p, false);
	edge(&p, true);
	CHECK(p.pe6 && p.dac == LEVEL && !p.timer);
	CHECK(pa_gate_timer(true, true) == PA_GATE_NONE);

	// The end of the transmission in the middle of a burst: off, and a late edge or timer can't bring it back
	disarm(&p);
	CHECK(off(&p));
	edge(&p, true);
	CHECK(off(&p));
	p.timer = true;
	timer_fires(&p);
	CHECK(off(&p));

	// Ended between a falling edge and its timer
	p = (pa_t){ .keyed = true };
	edge(&p, true);
	edge(&p, false);
	disarm(&p);
	CHECK(off(&p));
	CHECK(pa_gate_timer(false, true) == PA_GATE_OFF);

	// The burst line high at key-up (undriven on its pull-up, or inverted): the key-up is refused, the PA stays off
	CHECK(!pa_gate_can_key(true));

	if (failures) {
		printf("d578uvPaGateTest: %d failure(s)\n", failures);
		return 1;
	}
	printf("d578uvPaGateTest: all passed\n");
	return 0;
}
