// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The GD-77 board's pure data (docs/spec/hardware-gd77.md 3.2, 9.1): the start-up pin table (boards/gd77/pin_table.h)
// and the key matrix decode with its map (boards/gd77/keys.h, drivers/input/keymatrix)
#include <stdio.h>
#include <string.h>
#include "pin_table.h"
#include "keys.h"
#include "bus_cfg.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

// -1 = not in the table, else the entry's flags
static long flags_of(uint8_t pin)
{
	long f = -1;
	for (unsigned i = 0; i < GD77_PIN_COUNT; i++) {
		if (gd77_pin_table[i].pin == pin) {
			f = gd77_pin_table[i].flags;
		}
	}
	return f;
}

static bool out_level(uint8_t pin, bool level)
{
	long f = flags_of(pin);
	return f >= 0 && k22f_pin_is_output((uint32_t)f) && (((uint32_t)f & K22F_HIGH) != 0) == level;
}

static bool input_pullup(uint8_t pin)
{
	long f = flags_of(pin);
	return f >= 0 && !k22f_pin_is_output((uint32_t)f) && ((uint32_t)f & K22F_PULLUP) != 0 &&
	       k22f_pin_mux_of((uint32_t)f) == 1u;
}

static void pin_table(void)
{
	// Every pin once, every pin a real one
	for (unsigned i = 0; i < GD77_PIN_COUNT; i++) {
		CHECK(k22f_pin_valid(gd77_pin_table[i].pin));
		for (unsigned j = i + 1; j < GD77_PIN_COUNT; j++) {
			CHECK(gd77_pin_table[i].pin != gd77_pin_table[j].pin);
		}
	}
	// The power hold is set on its own, before the table
	CHECK(flags_of(PIN_POWER_HOLD) == -1);

	// Nothing transmits or switches the RF path at start-up
	CHECK(out_level(PIN_TX_EN, false));
	CHECK(out_level(PIN_RF_SW1, false));
	CHECK(out_level(PIN_RF_SW2, false));
	CHECK(out_level(PIN_RF_SW3, false));
	CHECK(out_level(PIN_RF_SW4, false));
	CHECK(out_level(PIN_AUDIO_EN, false));

	// Chip selects idle inactive; the data flash's is released (an input), never driven high (it is also XTAL0)
	CHECK(out_level(PIN_LCD_CS, true));
	CHECK(out_level(PIN_C6K_CS, true));
	CHECK(flags_of(PIN_NOR_CS) == GPIO_IN);

	// The HR_C6000 stays powered down until the radio code starts it; the LCD stays in reset
	CHECK(out_level(PIN_C6K_PWD, true));
	CHECK(out_level(PIN_C6K_E0, true));
	CHECK(out_level(PIN_LCD_RESET, false));

	// Keys: every matrix line an input with its pull-up (columns are driven only while scanned)
	for (uint8_t c = 0; c < KEY_COLS; c++) {
		CHECK(input_pullup((uint8_t)(PIN_KEY_COL0 + c)));
	}
	for (uint8_t r = 0; r < KEY_ROWS; r++) {
		CHECK(input_pullup((uint8_t)(PIN_KEY_ROW0 + r)));
	}
	CHECK(input_pullup(PIN_KEY_B9));
	CHECK(input_pullup(PIN_KEY_B1));
	CHECK(input_pullup(PIN_KEY_A2));
	CHECK(flags_of(PIN_KEY_A1) == GPIO_IN);
	CHECK(flags_of(PIN_POWER_SWITCH) == GPIO_IN);

	// Peripheral functions: SPI0 on ALT2 with high drive, I2S0 on ALT6
	CHECK(k22f_pcr_value((uint32_t)flags_of(PIN_C6K_SCK)) == 0x240u);
	CHECK(k22f_pcr_value((uint32_t)flags_of(PIN_C6K_SIN)) == 0x240u);
	CHECK(k22f_pin_mux_of((uint32_t)flags_of(PIN_I2S_RX_BCLK)) == 6u);
	CHECK(k22f_pin_mux_of((uint32_t)flags_of(PIN_I2S_RX_FS)) == 6u);
	CHECK(k22f_pin_mux_of((uint32_t)flags_of(PIN_I2S_TXD0)) == 6u);
	CHECK(k22f_pin_mux_of((uint32_t)flags_of(PIN_I2S_RXD0)) == 6u);

	// I2C buses idle: clock high, data released
	CHECK(out_level(PIN_I2CA_SCL, true));
	CHECK(input_pullup(PIN_I2CA_SDA));
	CHECK(out_level(PIN_I2CB_SCL, true));
	CHECK(input_pullup(PIN_I2CB_SDA));
}

// col_low[c]: row bits low while column c is driven
static uint32_t press(int col, int row)
{
	uint8_t col_low[4] = { 0 };
	if (col >= 0) {
		col_low[col] = (uint8_t)(1u << row);
	}
	return keymatrix_decode(&gd77_keymatrix, 0, col_low);
}

static void keys(void)
{
	CHECK(KEY_COUNT <= 32);
	CHECK(press(-1, 0) == 0);
	// The keypad: columns PTC0..PTC3 are keypad rows, rows PTB19..PTB21 keypad columns
	CHECK(press(0, 0) == 1u << KEY_1);
	CHECK(press(0, 1) == 1u << KEY_2);
	CHECK(press(0, 2) == 1u << KEY_3);
	CHECK(press(1, 0) == 1u << KEY_4);
	CHECK(press(1, 2) == 1u << KEY_6);
	CHECK(press(2, 1) == 1u << KEY_8);
	CHECK(press(3, 0) == 1u << KEY_STAR);
	CHECK(press(3, 1) == 1u << KEY_0);
	CHECK(press(3, 2) == 1u << KEY_HASH);
	// The navigation rows
	CHECK(press(0, 3) == 1u << KEY_NAV_A);
	CHECK(press(3, 3) == 1u << KEY_NAV_D);
	CHECK(press(0, 4) == 1u << KEY_NAV_E);
	CHECK(press(1, 4) == 1u << KEY_NAV_F);
	CHECK(press(2, 4) == 0);	// no key there
	CHECK(press(3, 4) == 0);

	// Two keys at once in different columns
	uint8_t col_low[4] = { 0x01, 0, 0x04, 0 };
	CHECK(keymatrix_decode(&gd77_keymatrix, 0, col_low) == ((1u << KEY_1) | (1u << KEY_9)));

	// Every key bit appears once in the map
	unsigned seen[KEY_COUNT] = { 0 };
	for (unsigned i = 0; i < sizeof gd77_key_map; i++) {
		if (gd77_key_map[i] != KEYMATRIX_NONE) {
			CHECK(gd77_key_map[i] < KEY_COUNT);
			seen[gd77_key_map[i]]++;
		}
	}
	for (unsigned k = 0; k < KEY_SIDE_B1; k++) {
		CHECK(seen[k] == 1);
	}
	for (unsigned k = KEY_SIDE_B1; k < KEY_COUNT; k++) {
		CHECK(seen[k] == 0);	// the direct keys aren't in the matrix
	}
}

// The bus choices (bus_cfg.h): the shipped table is valid, and the check refuses what the pins can't carry
static void buses(void)
{
	CHECK(gd77_bus_cfg_check(gd77_bus_cfg, GD77_BUS_COUNT) == 0);
	CHECK(gd77_bus_cfg[GD77_BUS_C6K].hw && !gd77_bus_cfg[GD77_BUS_C6K].hw_cs);
	CHECK(!gd77_bus_cfg[GD77_BUS_NOR].hw && !gd77_bus_cfg[GD77_BUS_LCD].hw);

	gd77_bus_cfg_t c[GD77_BUS_COUNT];
	memcpy(c, gd77_bus_cfg, sizeof c);
	c[GD77_BUS_I2C_A].hw = true;				// one two-wire bus on I2C0: fine
	CHECK(gd77_bus_cfg_check(c, GD77_BUS_COUNT) == 0);
	c[GD77_BUS_I2C_B].hw = true;				// both: I2C0 twice
	CHECK(gd77_bus_cfg_check(c, GD77_BUS_COUNT) != 0);

	memcpy(c, gd77_bus_cfg, sizeof c);
	c[GD77_BUS_NOR].hw = true;				// no SPI clock and data on PTE4..PTE6
	CHECK(gd77_bus_cfg_check(c, GD77_BUS_COUNT) != 0);
	memcpy(c, gd77_bus_cfg, sizeof c);
	c[GD77_BUS_LCD].hw = true;				// no SPI on PTC8..PTC12
	CHECK(gd77_bus_cfg_check(c, GD77_BUS_COUNT) != 0);

	memcpy(c, gd77_bus_cfg, sizeof c);
	c[GD77_BUS_C6K].hw_cs = true;				// PTD0 is SPI0_PCS0
	CHECK(gd77_bus_cfg_check(c, GD77_BUS_COUNT) == 0);
	c[GD77_BUS_C6K].hw = false;				// a hardware chip select needs the peripheral
	CHECK(gd77_bus_cfg_check(c, GD77_BUS_COUNT) != 0);
	memcpy(c, gd77_bus_cfg, sizeof c);
	c[GD77_BUS_I2C_A].hw_cs = true;				// and a pin that carries it
	c[GD77_BUS_I2C_A].hw = true;
	CHECK(gd77_bus_cfg_check(c, GD77_BUS_COUNT) != 0);
}

int main(void)
{
	pin_table();
	keys();
	buses();
	if (failures) {
		printf("gd77BoardTest: %d failures\n", failures);
		return 1;
	}
	printf("gd77BoardTest: all passed\n");
	return 0;
}
