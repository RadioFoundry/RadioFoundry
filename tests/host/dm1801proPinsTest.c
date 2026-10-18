// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// DM-1801 Pro start-up pin levels (boards/dm1801pro/boot_pins.h) against the stock firmware's boot levels
// (docs/spec/hardware-dm1801pro.md 3.1)
#include <stdio.h>
#include "boot_pins.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

// -1 = not in the table, else the level it is driven to
static int level_of(uint8_t pin)
{
	int level = -1;
	for (unsigned i = 0; i < DM1801PRO_BOOT_PIN_COUNT; i++)
		if (dm1801pro_boot_pins[i].pin == pin)
			level = dm1801pro_boot_pins[i].level;
	return level;
}

int main(void)
{
	// Every pin once
	for (unsigned i = 0; i < DM1801PRO_BOOT_PIN_COUNT; i++)
		for (unsigned j = i + 1; j < DM1801PRO_BOOT_PIN_COUNT; j++)
			CHECK(dm1801pro_boot_pins[i].pin != dm1801pro_boot_pins[j].pin);

	// PTB16: the stock firmware drives it low at start-up and never writes it again
	CHECK(PIN_PTB16 == 48);
	CHECK(level_of(PIN_PTB16) == 0);

	// Nothing transmits or listens until the radio code picks a band
	CHECK(level_of(PIN_TX_VHF) == 0);
	CHECK(level_of(PIN_TX_UHF) == 0);
	CHECK(level_of(PIN_RX_VHF) == 0);
	CHECK(level_of(PIN_RX_UHF) == 0);
	CHECK(level_of(PIN_AMP_EN) == 0);
	CHECK(level_of(PIN_VIBRATION) == 0);

	// Chip selects idle high; the broadcast tuner is off (its enable is low = on)
	CHECK(level_of(PIN_FLASH_CS) == 1);
	CHECK(level_of(PIN_RF_CS) == 1);
	CHECK(level_of(PIN_LCD_CS) == 1);
	CHECK(level_of(PIN_BCAST_EN) == 1);

	// Inputs are not driven: the RF data line, the power knob and the keys
	CHECK(level_of(PIN_RF_DATA) == -1);
	CHECK(level_of(PIN_POWER_KEY) == -1);
	CHECK(level_of(PIN_KEY_D0) == -1);
	CHECK(level_of(PIN_KEY_D2) == -1);

	// The power latch is set before the table, on its own
	CHECK(level_of(PIN_POWER_LATCH) == -1);

	if (failures) {
		printf("dm1801proPinsTest: %d failures\n", failures);
		return 1;
	}
	printf("dm1801proPinsTest: all passed\n");
	return 0;
}
