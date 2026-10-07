// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Internal interfaces between the DB25-D board files
#ifndef DB25D_H
#define DB25D_H

#include <stdint.h>
#include <stdbool.h>
#include "board.h"

// Ops each board file provides
extern const hal_display_t db25d_display;
extern const hal_input_t db25d_input;
extern const hal_radio_t db25d_radio;
extern const hal_power_t db25d_power;

// Physical key bits returned by db25d_input.scan(). The front keys' matrix positions aren't traced
// (hardware-db25d.md 3.3): input_db25d.c places them provisionally, and the positions no key is known for report as
// KEY_SPARE0.. so the diagnostics page shows them
enum {
	KEY_P1, KEY_P2, KEY_P3, KEY_VOL, KEY_TONE, KEY_BE, KEY_MENU, KEY_ENC_PUSH,
	KEY_SPARE0, KEY_SPARE1, KEY_SPARE2, KEY_SPARE3, KEY_SPARE4, KEY_SPARE5, KEY_SPARE6,
	KEY_PTT,
	KEY_COUNT
};

// --- Radio (radio_db25d.c) ---
typedef struct {
	uint16_t main_id, lo_id;	// register 0 of each RF chip (0x4819 on the PRO)
	uint16_t rssi_raw, noise_raw;	// main chip 0x67 / 0x65 from the last status poll
	uint32_t rf_writes;
} db25d_radio_diag_t;
void db25d_radio_diag(db25d_radio_diag_t *d);

// --- Power (power_db25d.c) ---
uint16_t db25d_adc_raw(uint8_t ch);	// the last reading of ADC channel 0..7

// --- Diagnostics (diag_db25d.c) ---
uint32_t db25d_input_raw(void);		// the last key scan before debouncing
bool db25d_diag_line(uint8_t i, char *buf, uint8_t len);
void db25d_diag_reset(void);

#endif
