// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Quadrature decoding (quadrature.h)
#include "quadrature.h"

#define ILL	2	// both lines changed

// [previous AB << 2 | new AB]: 0 no change, +1 / -1 one step, ILL both lines changed
static const int8_t step_table[16] = {
	//  to: 00    01    10    11
	/* 00 */ 0,    +1,   -1,   ILL,
	/* 01 */ -1,   0,    ILL,  +1,
	/* 10 */ +1,   ILL,  0,    -1,
	/* 11 */ ILL,  -1,   +1,   0,
};

void quad_init(quad_t *q, uint8_t steps, uint8_t rest, uint8_t ab)
{
	q->ab = ab & 3u;
	q->steps = steps == 1 || steps == 4 ? steps : 2;
	q->rest = rest & 0x0Fu;
	q->acc = 0;
	q->last_step = 0;
	q->detents = 0;
	q->transitions = q->illegal = q->reversals = 0;
}

int8_t quad_sample(quad_t *q, uint8_t ab)
{
	ab &= 3u;
	int8_t s = step_table[(q->ab << 2) | ab];
	q->ab = ab;
	if (s == 0)
		return 0;
	if (s == ILL) {
		// A sample was missed: which way is unknown, so nothing is guessed. The steps already summed stay (they were
		// real); at worst the detent the gap fell in is lost. Never a count, so never one backward
		q->illegal++;
		return 0;
	}
	q->transitions++;
	if (q->last_step != 0 && s != q->last_step)
		q->reversals++;
	q->last_step = s;
	if (q->acc > -8 && q->acc < 8)
		q->acc = (int8_t)(q->acc + s);
	if (!(q->rest & (1u << ab)))
		return 0;
	// At a detent position: a whole detent's worth of steps one way counts once; anything less (bounce, a wobble
	// that came back) counts nothing. Either way the count starts over from here
	int8_t d = q->acc >= (int8_t)q->steps ? 1 : q->acc <= -(int8_t)q->steps ? -1 : 0;
	q->acc = 0;
	q->detents += d;
	return d;
}

int8_t quad_take(const quad_t *q, int32_t *taken)
{
	int32_t n = q->detents - *taken;
	n = n > 127 ? 127 : n < -127 ? -127 : n;
	*taken += n;
	return (int8_t)n;
}
