// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Power key held to switch off (power_key.h)
#include "radio/power_key.h"

void pwrkey_init(pwrkey_t *k)
{
	k->released = false;
	k->held = 0;
}

bool pwrkey_on(pwrkey_t *k, bool down)
{
	if (!down) {
		k->released = true;
		k->held = 0;
	} else if (k->released && k->held < PWRKEY_OFF_POLLS) {
		k->held++;
	}
	return k->held < PWRKEY_OFF_POLLS;
}
