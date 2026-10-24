// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Radioddity GD-77 (TYT MD-760): board descriptor, clocks, start-up pin levels, key map (docs/spec/hardware-gd77.md).
// Bring-up state: display, keys, power, FM receive; transmit refused, DMR, USB and storage not wired yet (12).
#include "board.h"
#include "config.h"
#include "gd77.h"
#include "clock_gd77.h"
#include "pin_table.h"
#include "keymap_gd77.h"
#include "platform.h"
#include "FreeRTOS.h"
#if CONFIG_USB
#include "usb/usb_device.h"
#endif

// The RTOS heap, with every task stack, in the upper SRAM; data and bss are in the lower half (platform/k22f/common.ld)
uint8_t ucHeap[configTOTAL_HEAP_SIZE] ODR_CPU_RAM_NOINIT __attribute__((aligned(8)));

// Receive only unless built with CONFIG_GD77_TX (boards/gd77/config.mk); radio_gd77.c tx() also wants calibration
static const band_t bands[] = {
	{ 136000000, 174000000, CONFIG_GD77_TX != 0 },
	{ 400000000, 470000000, CONFIG_GD77_TX != 0 },
};

static const char *const power_names[] = { "L", "H" };

const board_t board = {
	.name = "GD-77",
	.caps = CAP_KEYPAD | (CONFIG_DMR ? CAP_DMR : 0),
	.bands = bands,
	.band_count = sizeof(bands) / sizeof(bands[0]),
	.power_count = sizeof(power_names) / sizeof(power_names[0]),
	.power_names = power_names,
	.power_default = 0,	// low
	.display = &gd77_display,
	.input = &gd77_input,
	.keymap = gd77_keymap,
	.keymap_len = GD77_KEYMAP_LEN,
	.side_keys = gd77_side_keys,
	.side_key_count = sizeof(gd77_side_keys),
	.radio = &gd77_radio,
	.power = &gd77_power,
	.flash = &gd77_flash,
#if CONFIG_BEEP || CONFIG_DMR
	.audio = &gd77_audio,
#endif
#if CONFIG_DMR
	.dmr = &gd77_dmr,
	.vocoder = &gd77_vocoder,
#endif
#if CONFIG_DIAG
	.diag_line = gd77_diag_line,
#endif
#if CONFIG_DEBUGLINK
	.view_line = gd77_view_line,
#endif
	.system = &k22f_system,
#if CONFIG_USB
	.host_port = &usb_cdc_stream,
#endif
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
	static const k22f_clock_t *const plans[] = {
#if GD77_CLOCK_TARGET
		&gd77_clock_target,
#endif
		&gd77_clock_bringup,
	};
	(void)k22f_clock_init_first(plans, sizeof(plans) / sizeof(plans[0]));
}

void board_init(void)
{
	gd77_power.init();
	(void)gd77_flash.init();	// before storage_init: the core's flash map comes from the probed size
	gd77_i2c_init();
	gd77_eeprom_init();		// the factory calibration, before the radio is set up
}
