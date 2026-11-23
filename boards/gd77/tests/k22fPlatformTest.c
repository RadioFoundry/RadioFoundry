// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The K22F platform's pure parts: the clock arithmetic and limit check (platform/k22f/clock_math.h) with the GD-77's
// two clock plans (boards/gd77/clock_gd77.h), the DSPI and PIT dividers, and the pin encoding and PORT control
// register values (platform/k22f/k22f_pin.h)
#include <stdio.h>
#include <string.h>
#include "clock_math.h"
#include "k22f_pin.h"
#include "clock_gd77.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static void clock_plans(void)
{
	// The bring-up plan: 12.288 MHz / 6 x 24, everything within the RUN limits
	CHECK(k22f_clock_check(&gd77_clock_bringup) == 0);
	CHECK(k22f_pll_ref_hz(&gd77_clock_bringup) == 2048000u);
	CHECK(k22f_core_hz(&gd77_clock_bringup) == 49152000u);
	CHECK(k22f_bus_hz(&gd77_clock_bringup) == 49152000u);
	CHECK(k22f_flash_hz(&gd77_clock_bringup) == 24576000u);

	// The target plan: 95.232 MHz needs HSRUN
	CHECK(k22f_clock_check(&gd77_clock_target) == 0);
	CHECK(k22f_core_hz(&gd77_clock_target) == 95232000u);
	CHECK(k22f_bus_hz(&gd77_clock_target) == 47616000u);
	CHECK(k22f_pit_ldval(k22f_bus_hz(&gd77_clock_target), 1000u) == 47615u);	// 1 ms exactly
	k22f_clock_t run = gd77_clock_target;
	run.hsrun = false;
	CHECK(k22f_clock_check(&run) != 0);

	// A 24.576 MHz reference with the target's divider would put the PLL reference above 4 MHz
	k22f_clock_t fast_ref = gd77_clock_target;
	fast_ref.ref_hz = 24576000u;
	CHECK(k22f_clock_check(&fast_ref) != 0);

	// Each limit on its own
	k22f_clock_t c = gd77_clock_bringup;
	c.div_flash = 1;		// 49 MHz flash
	CHECK(k22f_clock_check(&c) != 0);
	c = gd77_clock_bringup;
	c.prdiv = 24;			// out of the field
	CHECK(k22f_clock_check(&c) != 0);
	c = gd77_clock_bringup;
	c.div_bus = 0;
	CHECK(k22f_clock_check(&c) != 0);
	c = gd77_clock_bringup;
	c.prdiv = 2;			// 4.096 MHz reference
	CHECK(k22f_clock_check(&c) != 0);
	c = gd77_clock_bringup;
	c.vdiv = 31;			// 2.048 x 55 = 112.6 MHz core in RUN
	CHECK(k22f_clock_check(&c) != 0);
	c.hsrun = true;
	c.div_bus = 2;
	c.div_flexbus = 3;
	c.div_flash = 5;
	CHECK(k22f_clock_check(&c) == 0);
	c.div_bus = 1;			// 112.6 MHz bus
	CHECK(k22f_clock_check(&c) != 0);
}

static void dividers(void)
{
	// DSPI: bus / prescaler / scaler, doubled by DBR
	CHECK(k22f_dspi_hz(48000000u, 0, 0, false) == 12000000u);	// / 2 / 2
	CHECK(k22f_dspi_hz(48000000u, 0, 2, false) == 4000000u);	// / 2 / 6
	CHECK(k22f_dspi_hz(48000000u, 1, 3, false) == 2000000u);	// / 3 / 8
	CHECK(k22f_dspi_hz(48000000u, 2, 4, false) == 600000u);		// / 5 / 16
	CHECK(k22f_dspi_hz(49152000u, 0, 6, true) == 768000u);		// the GD-77's HR_C6000 port
	CHECK(k22f_dspi_hz(32768000u, 0, 15, false) == 500u);		// / 2 / 32768
}

static void pins(void)
{
	CHECK(K22F_PIN(K22F_PORT_A, 0) == 0);
	CHECK(K22F_PIN(K22F_PORT_E, 26) == 154);
	CHECK(k22f_pin_port(K22F_PIN(K22F_PORT_C, 17)) == K22F_PORT_C);
	CHECK(k22f_pin_num(K22F_PIN(K22F_PORT_C, 17)) == 17);
	CHECK(k22f_pin_bit(K22F_PIN(K22F_PORT_B, 31)) == 0x80000000u);
	CHECK(k22f_pin_valid(K22F_PIN(K22F_PORT_E, 31)));
	CHECK(!k22f_pin_valid(K22F_PIN_NONE));

	// PCR: mux in bits 10:8, pull enable / select, open drain, drive strength, filter, interrupt edge
	CHECK(k22f_pcr_value(K22F_MUX_GPIO) == 0x100u);
	CHECK(k22f_pcr_value(K22F_MUX_GPIO | K22F_PULLUP) == 0x103u);
	CHECK(k22f_pcr_value(K22F_MUX_GPIO | K22F_PULLDOWN) == 0x102u);
	CHECK(k22f_pcr_value(K22F_MUX_GPIO | K22F_PULLUP | K22F_PULLDOWN) == 0x103u);	// up wins
	CHECK(k22f_pcr_value(K22F_MUX(2) | K22F_DRIVE_HIGH) == 0x240u);
	CHECK(k22f_pcr_value(K22F_MUX(6)) == 0x600u);
	CHECK(k22f_pcr_value(K22F_MUX_GPIO | K22F_OPEN_DRAIN | K22F_FILTER) == 0x130u);
	CHECK(k22f_pcr_value(K22F_MUX_GPIO | K22F_IRQ(PORT_IRQC_FALLING)) == 0xA0100u);
	CHECK(k22f_pcr_value(K22F_MUX_GPIO | K22F_IRQ(PORT_IRQC_EITHER)) == 0xB0100u);
	// Output and level flags are GPIO data, not PCR bits
	CHECK(k22f_pcr_value(K22F_MUX_GPIO | K22F_OUT | K22F_HIGH) == 0x100u);
	CHECK(k22f_pin_is_output(K22F_MUX_GPIO | K22F_OUT));
	CHECK(!k22f_pin_is_output(K22F_MUX(2) | K22F_OUT));	// an alternative function isn't a GPIO output
	CHECK(!k22f_pin_is_output(K22F_MUX_GPIO));
}

int main(void)
{
	clock_plans();
	dividers();
	pins();
	if (failures) {
		printf("k22fPlatformTest: %d failures\n", failures);
		return 1;
	}
	printf("k22fPlatformTest: all passed\n");
	return 0;
}
