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

// --- Factory calibration (calibration_db25d.c, hardware-db25d.md 6) ---
#define DB25D_CAL_LEN		0x800u
#define DB25D_CAL_POINTS	8u
#define DB25D_CAL_FLASH_ADDR	0x031FE000u	// the program-flash copy, memory-mapped
#define DB25D_CAL_NAND_ADDR	0x280000u	// the NAND copy (block 20)
typedef enum { DB25D_CAL_NONE, DB25D_CAL_FLASH, DB25D_CAL_NAND } db25d_cal_source_t;
// Which copy the stock rule picks: "TUNE" and the higher save counter. Either pointer may be NULL; only the first 8
// bytes are read
db25d_cal_source_t db25d_cal_choose(const uint8_t *flash_copy, const uint8_t *nand_copy);
// Reads NAND main-area bytes (db25d_nand_read)
typedef bool (*db25d_cal_read_fn)(uint32_t addr, void *buf, uint32_t len);
// Copy the chosen page (DB25D_CAL_LEN bytes) into RAM. Either argument may be NULL. A NAND copy that wins on its header
// but can't be read whole gives way to the flash copy. False if neither copy is usable
bool db25d_cal_load(const uint8_t *flash_copy, db25d_cal_read_fn nand_read);
bool db25d_cal_valid(void);
db25d_cal_source_t db25d_cal_source(void);
uint32_t db25d_cal_counter(void);
// Per-frequency values, interpolated between the 8 points of the band. 0 when there's no page or the value isn't one
uint16_t db25d_cal_pa(uint32_t hz, bool high);		// DAC B at the top of the ramp
uint16_t db25d_cal_rx_tune(uint32_t hz);		// DAC A while receiving
uint8_t db25d_cal_mod(uint32_t hz);			// modcal for hrc7000_mod_bias_dc
void db25d_cal_tx_delays(uint32_t hz, uint16_t *on_us, uint16_t *off_us);
uint8_t db25d_cal_tx_center(void);			// 128 without a page
// Squelch open threshold for level 0..9, in dB below 0 dBm at the main chip (bk4819_squelch_rssi(-v, 2))
uint8_t db25d_cal_squelch_db(uint32_t hz, uint8_t level);

// --- SPI NAND (nand_db25d.c, hardware-db25d.md 6.5): read only so far ---
bool db25d_nand_init(void);
bool db25d_nand_ready(void);
uint16_t db25d_nand_id(void);		// manufacturer << 8 | device, 0 if nothing answered
uint32_t db25d_nand_corrected(void);	// pages read with corrected bit errors
bool db25d_nand_read(uint32_t addr, void *buf, uint32_t len);

// --- Power (power_db25d.c) ---
uint16_t db25d_adc_raw(uint8_t ch);	// the last reading of ADC channel 0..7

// --- Diagnostics (diag_db25d.c) ---
uint32_t db25d_input_raw(void);		// the last key scan before debouncing
bool db25d_diag_line(uint8_t i, char *buf, uint8_t len);
void db25d_diag_reset(void);

#endif
