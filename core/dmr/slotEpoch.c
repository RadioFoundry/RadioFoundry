// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The split duplex RX half's slot labels (see slotEpoch.h; docs/spec/split-duplex.md 7)
#include <string.h>
#include "dmr/slotEpoch.h"

void slotEpochInit(slotEpoch_t *e)
{
	memset(e, 0, sizeof(*e));
	e->period_ns = SLOT_EPOCH_PERIOD_NS;
}

void slotEpochClear(slotEpoch_t *e)
{
	uint32_t sets = e->sets;

	slotEpochInit(e);
	e->sets = sets;
}

bool slotEpochSet(slotEpoch_t *e, uint32_t t_us, uint32_t period_ns, uint32_t now_ms)
{
	if (period_ns == 0) {
		period_ns = SLOT_EPOCH_PERIOD_NS;
	}
	if ((period_ns < SLOT_EPOCH_PERIOD_MIN_NS) || (period_ns > SLOT_EPOCH_PERIOD_MAX_NS)) {
		return false;
	}
	e->valid = true;
	e->epoch_us = t_us;
	e->epoch_ns = 0;
	e->period_ns = period_ns;
	e->set_ms = now_ms;
	e->track_us = 0;
	e->avg_us = 0;
	e->sets++;
	return true;
}

bool slotEpochValid(const slotEpoch_t *e, uint32_t now_ms)
{
	return e->valid && ((uint32_t)(now_ms - e->set_ms) < SLOT_EPOCH_MAX_AGE_MS);
}

// Round to nearest, halves away from zero
static int64_t div_round(int64_t a, int64_t b)
{
	return (a >= 0) ? ((a + b / 2) / b) : -((-a + b / 2) / b);
}

int8_t slotEpochLabel(slotEpoch_t *e, uint32_t t_us, uint32_t now_ms, int32_t *residual_us)
{
	if (!slotEpochValid(e, now_ms)) {
		return 0;
	}
	int32_t dt_us = (int32_t)(t_us - e->epoch_us) - e->track_us;
	int64_t two = 2 * (int64_t)e->period_ns;

	if ((dt_us > SLOT_EPOCH_ADVANCE_US) || (dt_us < -SLOT_EPOCH_ADVANCE_US)) {
		// Move E by whole frames (an even number of slots keeps it a TS1 start) so the arithmetic stays small
		int64_t frames = ((int64_t)dt_us * 1000) / two;
		int64_t ns = (int64_t)e->epoch_ns + frames * two;
		int64_t us = (ns >= 0) ? (ns / 1000) : -((-ns + 999) / 1000);

		e->epoch_us += (uint32_t)us;
		e->epoch_ns = (uint32_t)(ns - us * 1000);
		dt_us = (int32_t)(t_us - e->epoch_us) - e->track_us;
	}
	int64_t dt_ns = (int64_t)dt_us * 1000 - (int64_t)e->epoch_ns;
	int64_t n = div_round(dt_ns, (int64_t)e->period_ns);
	int64_t res_ns = dt_ns - n * (int64_t)e->period_ns;

	*residual_us = (int32_t)(res_ns / 1000);
	e->last_residual_us = *residual_us;
	return ((n & 1) == 0) ? 1 : 2;
}

void slotEpochTrack(slotEpoch_t *e, int32_t residual_us)
{
	// avg_us holds 8 x the average: avg += residual - avg / 8
	e->avg_us += residual_us - e->avg_us / 8;
	int32_t step = e->avg_us / 64;		// an eighth of the average per burst
	int32_t t = e->track_us + step;

	if (t > SLOT_EPOCH_TRACK_MAX_US) {
		t = SLOT_EPOCH_TRACK_MAX_US;
	} else if (t < -SLOT_EPOCH_TRACK_MAX_US) {
		t = -SLOT_EPOCH_TRACK_MAX_US;
	}
	step = t - e->track_us;
	e->track_us = t;
	e->avg_us -= step * 8;			// the residuals to come are measured from the moved anchor
}
