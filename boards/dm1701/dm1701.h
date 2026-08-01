// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Internal interfaces between the DM-1701 board files
#ifndef DM1701_H
#define DM1701_H

#include <stdint.h>
#include <stdbool.h>
#include "board.h"

// The LCD data bus doubles as the keypad columns: the display and the keypad scan take turns
void dm1701_lcdbus_lock(void);
void dm1701_lcdbus_unlock(void);
// Reconnect LCD D0-D7 (the keypad columns) to the FSMC; the display calls it at the start of every burst, lock held
void dm1701_lcd_pins_fsmc(void);

// Extra analog readings (power_dm1701.c)
uint16_t dm1701_mic_level_raw(void);
int16_t dm1701_temperature_c10(void);

// Ops each board file provides (display_dm1701.c, input_dm1701.c, radio_dm1701.c, power_dm1701.c, flash_dm1701.c)
extern const hal_display_t dm1701_display;
extern const hal_input_t dm1701_input;
extern const hal_radio_t dm1701_radio;
extern const hal_power_t dm1701_power;
extern hal_flash_t dm1701_flash;

// Physical key bits returned by dm1701_input.scan() (matrix positions in docs/spec/hardware-dm1701.md 10.1)
enum {
	KEY_0, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9,
	KEY_STAR, KEY_HASH, KEY_UP, KEY_DOWN, KEY_GREEN, KEY_RED,
	KEY_FN_LEFT, KEY_FN_RIGHT, KEY_TOP, KEY_SK1, KEY_SK2, KEY_PTT,
	KEY_COUNT
};

// SPI flash security registers (1..3, 256 bytes each): factory calibration in 1-2, panel type and model flags in 3
bool dm1701_flash_read_security(uint8_t reg, uint32_t offset, void *buf, uint32_t len);

// Factory calibration (calibration_dm1701.c, docs/spec/hardware-dm1701.md section 12)
typedef struct {
	bool valid;
	uint16_t power_dac[4];	// very low, low, mid, high at the requested frequency (12-bit DAC values)
	uint8_t fm_i, fm_q;	// HR_C6000 4:0x45 / 4:0x46 for FM TX
	uint8_t dmr_i, dmr_q;	// for DMR
	int8_t ref_offset;	// reference oscillator tune - 128
} dm1701_cal_point_t;

bool dm1701_cal_load(void);
// Calibration values interpolated at a frequency (Hz); returns false if no calibration is loaded (safe defaults filled)
bool dm1701_cal_at(uint32_t hz, dm1701_cal_point_t *out);

#endif
