// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Tests radio/volume_pot.h: the position of a volume potentiometer below and above mid-rotation, at both ends, and when
// it counts as silent
#include <stdio.h>
#include "radio/volume_pot.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void)
{
	const volume_pot_cal_t cal = { 20, 320, 2020, 5 };
	volume_pot_t s = { 0 };
	// A knob that starts silent stays silent up to min + mute + hysteresis
	CHECK(volume_pot_percent(&s, &cal, 20) == 0);
	CHECK(volume_pot_percent(&s, &cal, 30) == 0);
	CHECK(volume_pot_percent(&s, &cal, 31) == 1);	// one count further: turned up
	// Once turned up, it is silent again only at min + mute
	CHECK(volume_pot_percent(&s, &cal, 26) == 1);
	CHECK(volume_pot_percent(&s, &cal, 25) == 0);
	// Below and above mid-rotation, and the ends
	CHECK(volume_pot_percent(&s, &cal, 170) == 25);
	CHECK(volume_pot_percent(&s, &cal, 320) == 50);
	CHECK(volume_pot_percent(&s, &cal, 1170) == 75);
	CHECK(volume_pot_percent(&s, &cal, 2020) == 100);
	CHECK(volume_pot_percent(&s, &cal, 4095) == 100);
	// A knob that is never silent: no reading gives 0
	const volume_pot_cal_t open = { 2, 310, 2012, VOLUME_POT_NO_MUTE };
	volume_pot_t o = { 0 };
	CHECK(volume_pot_percent(&o, &open, 0) == 1);
	CHECK(volume_pot_percent(&o, &open, 2) == 1);
	CHECK(volume_pot_percent(&o, &open, 2012) == 100);
	printf(fails ? "volumePotTest: %d failed\n" : "volumePotTest: ok\n", fails);
	return fails != 0;
}
