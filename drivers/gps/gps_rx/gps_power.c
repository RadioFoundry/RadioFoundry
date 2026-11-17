// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// When a GPS module is powered (gps_power.h)
#include "gps_power.h"

void gps_power_init(gps_power_t *p)
{
	p->present = false;
	p->on = true;
	p->gone = false;
}

void gps_power_set(gps_power_t *p, bool on)
{
	p->off_wanted = !on;
}

void gps_power_found(gps_power_t *p)
{
	p->present = true;
}

void gps_power_absent(gps_power_t *p)
{
	p->on = false;
	p->gone = true;
}

bool gps_power_step(gps_power_t *p)
{
	// The probe runs to its end whatever the setting says: a module is known only once it has been heard
	if (!p->present || p->gone) {
		return false;
	}
	bool want = !p->off_wanted;
	if (want == p->on) {
		return false;
	}
	p->on = want;
	return true;
}
