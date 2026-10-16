// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Factory calibration of the Baofeng HR_C7000 radios (DM-32UV, DM-1801 Pro): where it is, what it holds and the
// values derived from it (docs/spec/hardware-dm32.md 11, hardware-dm1801pro.md 6.4). Never writes.
#ifndef BAOFENG_CAL_H
#define BAOFENG_CAL_H

#include <stdint.h>
#include <stdbool.h>
#include "hal_flash.h"

typedef struct {
	uint16_t very_low, low, mid, high;	// PA drive DAC values (0..4095) at a frequency
} bf_cal_power_points_t;

// Find and read the factory calibration from the data flash (page whose last byte is 2). copy_addr: where a decoded
// copy of the image may be (0x710000 on the DM-32UV), 0 for none. Needs the flash initialized
bool bf_cal_load(const hal_flash_t *flash, uint32_t copy_addr);
bool bf_cal_valid(void);
const char *bf_cal_source(void);	// "factory", "copy" or "none"
uint8_t bf_cal_tx_center(void);		// 128 without calibration
// Modulation balance (RF_MOD_BIAS_CTRL low half) for a TX frequency in Hz; 0x4D4D without calibration
uint16_t bf_cal_modcal(uint32_t hz);
// PA drive points for a TX frequency in Hz; false without calibration
bool bf_cal_power(uint32_t hz, bf_cal_power_points_t *p);
// DAC value for a power level 0..8 (hardware-dm32.md 11.3), clamped to 4095; the uncalibrated fallback when false
uint16_t bf_cal_power_level(uint32_t hz, uint8_t level);
// A calibration row read the way the DM-1801 Pro's own firmware reads it (hardware-dm1801pro.md 6.4): the band by the
// TX frequency (UHF above 200 MHz), the frequency clamped to the band limits (0x00 / 0xCC), the five TX points (0x314 /
// 0x33C) and linear interpolation truncated toward the lower point's byte. REG40 has three bytes, at TX points 0, 2
// and 4. 0 without calibration
typedef enum {
	BF_ROW_PA_LOW,		// 0x40 UHF / 0xD0 VHF
	BF_ROW_PA_MID,		// 0x45 / 0xD5
	BF_ROW_PA_HIGH,		// 0x4A / 0xDA
	BF_ROW_MOD,		// 0x17 / 0xA6: modulation balance, RF_MOD_BIAS_CTRL low half = byte x 0x0101
	BF_ROW_REG40,		// 0x1C / 0xAB: FD6818 register 0x40 = 0x3480 + byte on FM transmit
} bf_cal_row_t;
uint8_t bf_cal_tx_byte(uint32_t hz, bf_cal_row_t row);
// Raw byte of the 0x175-byte working image (diagnostics)
uint8_t bf_cal_byte(uint16_t offset);
// Diagnostics text of the L / M / H drive (levels 1, 4, 8) for a TX frequency: "L/M/H 192/832/1632", or
// "L/M/H none" without calibration. Always NUL terminated when len > 0; returns the length written
uint8_t bf_cal_drive_text(uint32_t hz, char *buf, uint8_t len);

// Pure part for host tests: decode the three pieces of a factory page into the working image and derive K values
#define BF_CAL_LEN		0x175u
bool bf_cal_parse(const uint8_t *sector, uint32_t sector_len, uint8_t image[BF_CAL_LEN]);
void bf_cal_set_image(const uint8_t image[BF_CAL_LEN], bool valid);

#endif
