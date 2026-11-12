// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// MD-9600 hardware options (hal_hwopt.h): the board revision (docs/spec/hardware-md9600.md 16, 19.3). Start-up tells
// V4A / V5 from the older boards by the AK2365A read test, and the option is hidden then; on V1-V4 a person or the
// installer tells V1-V3 from V4, stored in the board's record sector
#include <stddef.h>
#include "md9600.h"
#include "config.h"

// Choice i is revision i + 1 (md9600_rev_t)
static const char *const rev_choices[] = { "V1-V3", "V4", "V4A/V5" };

// The record as stored now, read once: the home screen asks for the state at every redraw
static md9600_rev_t stored;
static bool stored_known;

static md9600_rev_t stored_rev(void)
{
	if (!stored_known) {
		stored = md9600_flash_rev_load();
		stored_known = true;
	}
	return stored;
}

static int8_t rev_get(void)
{
	md9600_rev_t r = stored_rev();
	return r == MD9600_REV_UNKNOWN ? HWOPT_NONE : (int8_t)(r - 1);
}

static bool rev_set(uint8_t choice)
{
	md9600_rev_t r = (md9600_rev_t)(choice + 1u);
	md9600_rev_state_t s = md9600_rev_state();
	// An AK2365A answered: the board is V1-V3 or V4, never V4A / V5
	if (md9600_rev_desc(r) == NULL || (s.if_chip == MD9600_IF_PRESENT && r == MD9600_REV_V5) ||
	    !md9600_flash_rev_store(r)) {
		return false;
	}
	stored = r;
	stored_known = true;
	if (r == s.rev) {
		md9600_radio_rev_confirm();	// the revision running now: transmit without a restart
	}
	return true;
}

// Hidden when the read test found no AK2365A (V4A / V5, nothing to state); a restart pending when the record names
// another revision than the one running; a question while nothing confirms the running one
static uint8_t rev_state(void)
{
	md9600_rev_state_t s = md9600_rev_state();
	if (s.source == MD9600_REV_SRC_DETECTED) {
		return HWOPT_ST_HIDDEN;
	}
	md9600_rev_t r = stored_rev();
	if (r != MD9600_REV_UNKNOWN && r != s.rev && (s.if_chip != MD9600_IF_PRESENT || r != MD9600_REV_V5)) {
		return HWOPT_ST_RESTART;
	}
	return s.confirmed ? HWOPT_ST_OK : HWOPT_ST_ASK;
}

const hal_hwopt_t md9600_hwopts[MD9600_HWOPT_COUNT] = {
	{ "Board revision", sizeof(rev_choices) / sizeof(rev_choices[0]), rev_choices, rev_get, rev_set, true,
	  rev_state },
};
