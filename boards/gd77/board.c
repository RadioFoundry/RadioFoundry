// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Radioddity GD-77 (TYT MD-760): board descriptor, clocks, start-up pin levels, key map (docs/spec/hardware-gd77.md).
// Bring-up state: display, keys, power, FM receive; transmit refused, DMR, USB and storage not wired yet (12).
#include "board.h"
#include "config.h"
#include "gd77.h"
#include "clock_gd77.h"
#include "pin_table.h"
#include "platform.h"
#include "FreeRTOS.h"

// The RTOS heap, with every task stack, in the upper SRAM; data and bss are in the lower half (gd77.ld)
uint8_t ucHeap[configTOTAL_HEAP_SIZE] ODR_CPU_RAM_NOINIT __attribute__((aligned(8)));

// Receive only until the PA path and calibration are confirmed (radio_gd77.c tx() refuses as well)
static const band_t bands[] = {
	{ 136000000, 174000000, false },
	{ 400000000, 470000000, false },
};

static const char *const power_names[] = { "L", "H" };

// docs/ux/information-architecture.md 7.6. Provisional: the six navigation keys' names and the direct keys' roles
// are not confirmed (hardware-gd77.md 9.1); the diagnostics page shows the raw bits to settle them
static const keymap_entry_t keymap[] = {
	{ KEY_NAV_A, ACT_SELECT, ACT_SELECT, false, false },
	{ KEY_NAV_B, ACT_BACK, ACT_BACK, false, false },
	{ KEY_NAV_C, ACT_NAV_UP, ACT_NAV_UP, false, true },
	{ KEY_NAV_D, ACT_NAV_DOWN, ACT_NAV_DOWN, false, true },
	{ KEY_NAV_E, ACT_NAV_LEFT, ACT_USER_1, false, true },
	{ KEY_NAV_F, ACT_NAV_RIGHT, ACT_USER_4, false, true },
	{ KEY_STAR, ACT_QUICK, ACT_QUICK, false, false },
	{ KEY_HASH, ACT_CONTEXT, ACT_CONTEXT, false, false },
	{ KEY_0, ACT_DIGIT_0, ACT_DIGIT_0, false, false },
	{ KEY_1, ACT_DIGIT_1, ACT_DIGIT_1, false, false },
	{ KEY_2, ACT_DIGIT_2, ACT_DIGIT_2, false, false },
	{ KEY_3, ACT_DIGIT_3, ACT_DIGIT_3, false, false },
	{ KEY_4, ACT_DIGIT_4, ACT_DIGIT_4, false, false },
	{ KEY_5, ACT_DIGIT_5, ACT_DIGIT_5, false, false },
	{ KEY_6, ACT_DIGIT_6, ACT_DIGIT_6, false, false },
	{ KEY_7, ACT_DIGIT_7, ACT_DIGIT_7, false, false },
	{ KEY_8, ACT_DIGIT_8, ACT_DIGIT_8, false, false },
	{ KEY_9, ACT_DIGIT_9, ACT_DIGIT_9, false, false },
	{ KEY_SIDE_B1, ACT_MONITOR, ACT_MONITOR, true, false },
	{ KEY_SIDE_B9, ACT_TALK_TARGET, ACT_USER_2, false, false },
	{ KEY_TOP, ACT_NONE, ACT_EMERGENCY, false, false },
	{ KEY_PTT, ACT_PTT, ACT_PTT, true, false },
};

// Reassignable in Settings > Keys and the codeplug (core/input/keyassign.h): side key 1, side key 2, top key
static const uint8_t side_keys[] = { KEY_SIDE_B1, KEY_SIDE_B9, KEY_TOP };

const board_t board = {
	.name = "GD-77",
	.caps = CAP_KEYPAD,
	.bands = bands,
	.band_count = sizeof(bands) / sizeof(bands[0]),
	.power_count = sizeof(power_names) / sizeof(power_names[0]),
	.power_names = power_names,
	.power_default = 0,	// low
	.display = &gd77_display,
	.input = &gd77_input,
	.keymap = keymap,
	.keymap_len = sizeof(keymap) / sizeof(keymap[0]),
	.side_keys = side_keys,
	.side_key_count = sizeof(side_keys),
	.radio = &gd77_radio,
	.power = &gd77_power,
	.flash = &gd77_flash,
#if CONFIG_DIAG
	.diag_line = gd77_diag_line,
#endif
	.system = &k22f_system,
	// No thresholds until the battery channel's scale is measured (power_gd77.c)
	.battery_low_mv = 0,
	.battery_critical_mv = 0,
};

// The power hold first, so the radio stays on once the knob is let go; then every other pin, then the clocks. A
// clock that doesn't come up leaves the radio on the internal FLL, which the diagnostics page shows
void board_early_init(void)
{
	SIM_SCGC5 |= SIM_SCGC5_PORTS;
	k22f_pin_config(PIN_POWER_HOLD, GPIO_OUT_HIGH);
	k22f_pins_apply(gd77_pin_table, GD77_PIN_COUNT);
	gd77_bus_init();
	(void)k22f_clock_init(&gd77_clock_bringup);	// the target plan (clock_gd77.h) once the radio runs
}

void board_init(void)
{
	gd77_power.init();
	(void)gd77_flash.init();
}
