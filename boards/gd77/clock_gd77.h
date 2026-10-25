// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// GD-77 clock plans (docs/spec/hardware-gd77.md 2.2). One 12.288 MHz reference feeds the K22F's EXTAL0 and the
// HR_C6000 (unverified; a clock, not a crystal: XTAL0 is the data flash's chip select). Pure data: tests/host checks
// both plans against the K22F's limits (platform/k22f/clock_math.h).
#ifndef GD77_CLOCK_H
#define GD77_CLOCK_H

#include "clock_math.h"

#define GD77_REF_HZ	12288000u

// Bring-up, in RUN mode: PLL reference 12.288 / 6 = 2.048 MHz, x 24 = 49.152 MHz for the core, bus and FlexBus;
// flash / 2 = 24.576 MHz
__attribute__((unused)) static const k22f_clock_t gd77_clock_bringup = {
	.ref_hz = GD77_REF_HZ, .crystal = false, .prdiv = 5, .vdiv = 0,
	.div_core = 1, .div_bus = 1, .div_flexbus = 1, .div_flash = 2, .hsrun = false,
};

// The target once the radio runs: HSRUN, PLL reference 12.288 / 4 = 3.072 MHz, x 31 = 95.232 MHz core; bus and
// FlexBus / 2 = 47.616 MHz, flash / 4 = 23.808 MHz. A 1 ms PIT period is a whole number of bus clocks (47 616)
__attribute__((unused)) static const k22f_clock_t gd77_clock_target = {
	.ref_hz = GD77_REF_HZ, .crystal = false, .prdiv = 3, .vdiv = 7,
	.div_core = 1, .div_bus = 2, .div_flexbus = 2, .div_flash = 4, .hsrun = true,
};

// Which plans start-up tries, in order (board.c). The default is the bring-up plan alone until a radio has run the
// target; a build can choose the target first (MCU_DEFS += -DGD77_CLOCK_TARGET=1), and then falls back to the
// bring-up plan, then to the internal FLL, if the HSRUN switch or the PLL fails
#ifndef GD77_CLOCK_TARGET
#define GD77_CLOCK_TARGET	0
#endif

#endif
