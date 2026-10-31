// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The K22F platform's system and peripheral arithmetic: the flash self-programming window and command object
// (platform/k22f/ftfa_math.h), the reset cause names, the HSRUN plan's RUN step and the time base across clock
// changes (clock_math.h), the I2C divider, the DSPI chip select delays and the FTM PWM (periph_math.h), and the pin
// alternate table (k22f_mux.h) against the GD-77's pin table and bus choices (boards/gd77)
#include <stdio.h>
#include <string.h>
#include "clock_math.h"
#include "ftfa_math.h"
#include "periph_math.h"
#include "k22f_mux.h"
#include "clock_gd77.h"
#include "pin_table.h"
#include "bus_cfg.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static void flash_window(void)
{
	// The bootloader below 0x4000 and the last sector are never touched
	CHECK(!k22f_ftfa_erase_ok(0x0000u));
	CHECK(!k22f_ftfa_erase_ok(0x3800u));
	CHECK(k22f_ftfa_erase_ok(0x4000u));
	CHECK(k22f_ftfa_erase_ok(0x7F000u));
	CHECK(!k22f_ftfa_erase_ok(0x7F800u));
	CHECK(!k22f_ftfa_erase_ok(0x80000u));
	CHECK(!k22f_ftfa_erase_ok(0x4400u));		// not sector aligned
	CHECK(!k22f_ftfa_range_ok(0x4000u, 0u));
	CHECK(!k22f_ftfa_range_ok(0x3FFCu, 8u));	// starts in the bootloader
	CHECK(!k22f_ftfa_range_ok(0x7F7FCu, 8u));	// runs into the last sector
	CHECK(k22f_ftfa_range_ok(0x7F7FCu, 4u));
	CHECK(!k22f_ftfa_range_ok(0x4000u, 0xFFFFFFFFu));	// no wrap-around
	CHECK(k22f_ftfa_program_ok(0x4000u, 8u));
	CHECK(!k22f_ftfa_program_ok(0x4002u, 4u));
	CHECK(!k22f_ftfa_program_ok(0x4000u, 6u));

	uint8_t o[8];
	k22f_ftfa_fccob(K22F_FTFA_CMD_PROGRAM_LONGWORD, 0x07A10Cu, 0x44332211u, o);
	static const uint8_t want[8] = { 0x06, 0x07, 0xA1, 0x0C, 0x44, 0x33, 0x22, 0x11 };
	CHECK(memcmp(o, want, 8) == 0);
	k22f_ftfa_fccob(K22F_FTFA_CMD_ERASE_SECTOR, 0x7F000u, 0, o);
	CHECK(o[0] == 0x09 && o[1] == 0x07 && o[2] == 0xF0 && o[3] == 0x00);
}

static void reset_names(void)
{
	CHECK(strcmp(k22f_reset_name(0x0082u), "por") == 0);	// power-on sets the low voltage bit too
	CHECK(strcmp(k22f_reset_name(0x0002u), "lvd") == 0);
	CHECK(strcmp(k22f_reset_name(0x0040u), "pin") == 0);
	CHECK(strcmp(k22f_reset_name(0x0400u), "sw") == 0);
	CHECK(strcmp(k22f_reset_name(0x0200u), "lockup") == 0);
	CHECK(strcmp(k22f_reset_name(0x0020u), "wdog") == 0);
	CHECK(strcmp(k22f_reset_name(0x0008u), "lol") == 0);
	CHECK(strcmp(k22f_reset_name(0x0000u), "none") == 0);
}

static void clock_steps(void)
{
	// The target's RUN step for flash programming and standby: core = bus = 47.616 MHz, flash unchanged
	k22f_clock_t run;
	CHECK(k22f_clock_run_variant(&gd77_clock_target, &run));
	CHECK(!run.hsrun);
	CHECK(k22f_core_hz(&run) == 47616000u && k22f_bus_hz(&run) == k22f_bus_hz(&gd77_clock_target));
	CHECK(k22f_flash_hz(&run) == k22f_flash_hz(&gd77_clock_target));
	// A plan whose bus clock is above the RUN limit has no RUN step
	k22f_clock_t fast = gd77_clock_target;
	fast.vdiv = 15;		// 3.072 x 39 = 119.808 MHz, bus / 2 = 59.904 MHz, FlexBus / 3, flash / 5
	fast.div_flexbus = 3;
	fast.div_flash = 5;
	CHECK(k22f_clock_check(&fast) == 0);
	CHECK(!k22f_clock_run_variant(&fast, &run));
	// Every GD-77 clock, and the step, has a whole number of cycles per millisecond (time.c)
	CHECK(k22f_core_hz(&gd77_clock_bringup) % 1000u == 0 && k22f_core_hz(&gd77_clock_target) % 1000u == 0);
	CHECK(47616000u % 1000u == 0 && K22F_FIRC_HZ % 1000u == 0);
}

static void time_base(void)
{
	uint64_t ms = 0;
	uint32_t rem = 0;
	const uint32_t per = 95232u;	// 95.232 MHz
	k22f_time_fold(&ms, &rem, per * 3u + 5u, per);
	CHECK(ms == 3u && rem == 5u);
	// A fold that wraps the 32-bit sum
	rem = per - 1u;
	k22f_time_fold(&ms, &rem, 0xFFFFFFF0u, per);
	uint64_t total = 3ull * per + (per - 1u) + 0xFFFFFFF0ull;	// what was folded so far, in cycles
	CHECK(ms == total / per && rem == total % per);
	// Down to the RUN step and back: half a millisecond stays half a millisecond
	rem = per / 2u;
	rem = k22f_time_rescale(rem, per, 47616u);
	CHECK(rem == 23808u);
	rem = k22f_time_rescale(rem, 47616u, per);
	CHECK(rem == per / 2u);
}

static void i2c_divider(void)
{
	uint32_t hz;
	uint8_t f = k22f_i2c_f(49152000u, 100000u, &hz);
	CHECK(f != 0xFFu && hz <= 100000u && hz > 90000u);
	CHECK(hz == 49152000u / ((1u << (f >> 6)) * k22f_i2c_scl_div[f & 63u]));
	f = k22f_i2c_f(47616000u, 400000u, &hz);
	CHECK(f != 0xFFu && hz <= 400000u && hz > 350000u && (f >> 6) == 0u);
	// Slower than the table alone: a multiplier
	f = k22f_i2c_f(47616000u, 10000u, &hz);
	CHECK(f != 0xFFu && (f >> 6) != 0u && hz <= 10000u);
	CHECK(k22f_i2c_f(47616000u, 1000u, &hz) == 0xFFu);
	// The divider table only grows within each group of eight
	for (unsigned i = 0; i < 64u; i++) {
		CHECK(i % 8u == 0 || k22f_i2c_scl_div[i] > k22f_i2c_scl_div[i - 1u]);
	}
}

static void dspi_delays(void)
{
	uint8_t pre = 9, scale = 9;
	CHECK(k22f_dspi_delay(49152000u, 1000u, &pre, &scale));	// 1 us = 49.2 cycles: 7 x 8 = 56 is the shortest
	CHECK(pre == 3 && scale == 2);
	CHECK(k22f_dspi_delay(47616000u, 0u, &pre, &scale) && pre == 0 && scale == 0);
	CHECK(!k22f_dspi_delay(47616000u, 4000000000u, &pre, &scale));	// longer than 7 x 65536 cycles
}

static void pwm(void)
{
	uint8_t ps;
	uint16_t mod;
	CHECK(k22f_pwm_period(47616000u, 1000u, &ps, &mod));
	CHECK(ps == 0 && mod == 47615u);
	CHECK(k22f_pwm_period(47616000u, 100u, &ps, &mod));	// 476 160 counts: a prescaler
	CHECK(ps == 3 && mod == 59519u);
	CHECK(!k22f_pwm_period(47616000u, 0u, &ps, &mod));
	CHECK(k22f_pwm_cv(47615u, 0) == 0u);
	CHECK(k22f_pwm_cv(47615u, 50) == 23808u);
	CHECK(k22f_pwm_cv(47615u, 100) == 47616u);	// above MOD: always high
}

static void mux_table(void)
{
	// No pin carries one function at two ALT numbers, and no pin and ALT pair is listed twice
	for (unsigned i = 0; i < K22F_MUX_COUNT; i++) {
		CHECK(k22f_pin_valid(k22f_mux_table[i].pin) && k22f_mux_table[i].alt >= 2 && k22f_mux_table[i].alt <= 7);
		for (unsigned j = i + 1; j < K22F_MUX_COUNT; j++) {
			CHECK(!(k22f_mux_table[i].pin == k22f_mux_table[j].pin && k22f_mux_table[i].alt == k22f_mux_table[j].alt));
			CHECK(!(k22f_mux_table[i].pin == k22f_mux_table[j].pin && k22f_mux_table[i].fn == k22f_mux_table[j].fn));
		}
	}
	// Every alternate the GD-77's start-up table selects is a verified row at that ALT number
	for (unsigned i = 0; i < GD77_PIN_COUNT; i++) {
		uint32_t mux = k22f_pin_mux_of(gd77_pin_table[i].flags);
		if (mux < 2u) {
			continue;
		}
		bool found = false;
		for (unsigned j = 0; j < K22F_MUX_COUNT; j++) {
			if (k22f_mux_table[j].pin == gd77_pin_table[i].pin && k22f_mux_table[j].alt == mux) {
				found = k22f_mux_table[j].verified;
			}
		}
		CHECK(found);
	}
	CHECK(k22f_mux_allowed(K22F_PIN(K22F_PORT_E, 24), 5, K22F_FN_I2C0_SCL));
	CHECK(!k22f_mux_allowed(K22F_PIN(K22F_PORT_E, 24), 2, K22F_FN_I2C0_SCL));
	CHECK(!k22f_mux_allowed(K22F_PIN(K22F_PORT_E, 4), 2, K22F_FN_SPI1_SCK));	// PTE4..6: chip selects only
}

static void board_choices(void)
{
	gd77_bus_cfg_t c[GD77_BUS_COUNT];
	memcpy(c, gd77_bus_cfg, sizeof c);
	c[GD77_BUS_I2C_A].hw = true;
	c[GD77_BUS_C6K].hw_cs = true;
	CHECK(gd77_bus_cfg_check(c, GD77_BUS_COUNT) == 0);
	c[GD77_BUS_I2C_A].mux = 2;		// I2C0 isn't ALT2 on PTE24 / PTE25
	CHECK(gd77_bus_cfg_check(c, GD77_BUS_COUNT) != 0);
	memcpy(c, gd77_bus_cfg, sizeof c);
	c[GD77_BUS_C6K].pins[3].pin = K22F_PIN(K22F_PORT_D, 4);	// a chip select pin that isn't PCS0
	CHECK(gd77_bus_cfg_check(c, GD77_BUS_COUNT) == 0);	// fine while it stays a GPIO
	c[GD77_BUS_C6K].hw_cs = true;
	CHECK(gd77_bus_cfg_check(c, GD77_BUS_COUNT) != 0);
	CHECK(gd77_backlight_cfg_check(false) == 0 && gd77_backlight_cfg_check(true) == 0);
}

int main(void)
{
	flash_window();
	reset_names();
	clock_steps();
	time_base();
	i2c_divider();
	dspi_delays();
	pwm();
	mux_table();
	board_choices();
	if (failures) {
		printf("k22fSystemTest: %d failures\n", failures);
		return 1;
	}
	printf("k22fSystemTest: all passed\n");
	return 0;
}
