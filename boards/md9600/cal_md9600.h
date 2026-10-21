// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// MD-9600 factory calibration and hardware option block: what the radio code may ask (docs/spec/hardware-md9600.md
// 11). Both are read once at start-up from the SPI flash security registers (calibration: registers 1-2, 512 bytes;
// options: register 3, 64 bytes) and never written. The layout is unverified, so every accessor says when it has no
// value, and the caller keeps its safe default then. Pure apart from md9600_cal_load's flash read: the host test
// (tests/host/md9600CalTest.c) runs it on synthetic blocks.
#ifndef CAL_MD9600_H
#define CAL_MD9600_H

#include <stdint.h>
#include <stdbool.h>

#define MD9600_CAL_SIZE		512u
#define MD9600_OPT_SIZE		64u

// Read both blocks (after the flash's init). Without them every accessor below says "no value"
void md9600_cal_load(void);
// Feed the blocks directly (host tests; NULL = unreadable)
void md9600_cal_set(const uint8_t *cal, const uint8_t *opt);

// "factory", "blank", "unreadable" or "none"
const char *md9600_cal_source(void);
const char *md9600_opt_source(void);
bool md9600_cal_valid(void);

// The calibration rows (11.2). UHF rows have 9 points, 400 MHz + 10 MHz steps; VHF rows 5 points, 136 MHz + 9.5 MHz
// steps; the band follows MD9600_BAND_SPLIT_HZ. Values are the stored bytes, interpolated (rounded, the last slope
// continued past the top point); the DACs take byte x 16
typedef enum {
	MD9600_CAL_RX_TUNE,		// receiver front-end tuning (DAC1 UHF / DAC2 VHF)
	MD9600_CAL_POWER_LOW,		// PA drive per power level (5, 10, 25, 40 W labels)
	MD9600_CAL_POWER_MID_LOW,
	MD9600_CAL_POWER_MID,
	MD9600_CAL_POWER_HIGH,
	MD9600_CAL_SQ9_OPEN,		// squelch level 9 and 1, opening and closing thresholds
	MD9600_CAL_SQ9_CLOSE,
	MD9600_CAL_SQ1_OPEN,
	MD9600_CAL_SQ1_CLOSE,
	MD9600_CAL_CTCSS_67,		// CTCSS deviation at 67.0, 151.4 and 254.1 Hz
	MD9600_CAL_CTCSS_151,
	MD9600_CAL_CTCSS_254,
	MD9600_CAL_DCS,			// DCS deviation
	MD9600_CAL_ROW_COUNT
} md9600_cal_row_t;

// The row's value at hz; false when there is no calibration or the row is blank
bool md9600_cal_row(md9600_cal_row_t row, uint32_t hz, int32_t *value);
// Front-end tuning as a 12-bit DAC value: the row x 16, clamped; mid scale (2048) without calibration
uint16_t md9600_cal_rx_tune(uint32_t hz);
// The reference oscillator tune of a band (the HR_C6000's MOD2 offset), the stored byte - 128
bool md9600_cal_ref_tune(bool uhf, int8_t *out);
// The RSSI readings stored for -120 dBm and -70 dBm
bool md9600_cal_rssi(uint8_t *at_m120, uint8_t *at_m70);
// A raw byte of the 512 (0xFF without calibration): the parts not decoded yet, for diagnostics and the debug link
uint8_t md9600_cal_byte(uint16_t offset);

// The hardware option block (register 3). Band edges: four halfwords at +0x14..+0x1A, each a BCD frequency in
// 100 kHz steps (0x1450 = 145.0 MHz). Which halfword is which edge is not settled, so they are sorted by value: the two
// below MD9600_BAND_SPLIT_HZ are the VHF band, the two above it UHF. False unless all four decode and two fall in each
// band
typedef struct {
	uint32_t vhf_lo_hz, vhf_hi_hz, uhf_lo_hz, uhf_hi_hz;
} md9600_band_edges_t;
bool md9600_opt_band_edges(md9600_band_edges_t *e);
// Bit 4 of +0x1D: the backlight is only switched on and off (no dimming). False without the block
bool md9600_opt_backlight_switched(void);
uint8_t md9600_opt_byte(uint16_t offset);

#endif
