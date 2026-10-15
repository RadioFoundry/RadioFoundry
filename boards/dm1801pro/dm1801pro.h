// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Internal interfaces between the DM-1801 Pro board files
#ifndef DM1801PRO_H
#define DM1801PRO_H

#include <stdint.h>
#include <stdbool.h>
#include "board.h"

// Ops each board file provides
extern const hal_display_t dm1801pro_display;
extern const hal_input_t dm1801pro_input;
extern const hal_radio_t dm1801pro_radio;
extern const hal_power_t dm1801pro_power;

// Physical key bits returned by dm1801pro_input.scan(). The matrix positions and the four direct pins are provisional
// (input_dm1801pro.c, hardware-dm1801pro.md 9.1); the diagnostics page shows the raw scan to fix them
enum {
	KEY_0, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9,
	KEY_STAR, KEY_HASH, KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_MENU, KEY_BACK, KEY_VFO_MR, KEY_AB,
	KEY_SK1, KEY_SK2, KEY_SOS, KEY_PTT,
	KEY_COUNT
};

// Raw key state for the diagnostics page: matrix keys as a bit per strobe * 4 + input, the direct pins in bits 20-23
uint32_t dm1801pro_key_raw(void);

uint16_t dm1801pro_battery_raw(void);	// the last ADC channel C reading

// --- Radio (radio_dm1801pro.c) ---
typedef struct {
	uint16_t rssi_raw, noise_raw;	// FD6818 0x67 / 0x65 from the last status poll
	uint16_t fm_noise;		// HR_C7000 FM_SQL
	uint32_t rf_writes;
} dm1801pro_radio_diag_t;
void dm1801pro_radio_diag(dm1801pro_radio_diag_t *d);

// --- Diagnostics (diag_dm1801pro.c) ---
bool dm1801pro_diag_line(uint8_t i, char *buf, uint8_t len);
void dm1801pro_diag_reset(void);

#endif
