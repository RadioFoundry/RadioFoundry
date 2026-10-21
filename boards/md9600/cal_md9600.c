// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// MD-9600 factory calibration, the receiver front-end tuning only (docs/spec/hardware-md9600.md 11). The table is the
// 512 bytes of SPI flash security registers 1-2, read once and never written. The layout is unverified, so only the
// front-end tuning rows are used, and only when they don't look blank; otherwise the tuning sits at mid scale. The
// PA rows wait until the layout is checked on a radio: transmit is refused until then (radio_md9600.c).
#include <string.h>
#include "md9600.h"
#include "radio/cal_curve.h"

#define CAL_SIZE	512u
#define ROW_RX_TUNE_U	0x30u	// 9 points, UHF, 400 MHz + 10 MHz steps
#define ROW_RX_TUNE_V	0x39u	// 5 points, VHF, 136 MHz + 9.5 MHz steps
#define POINTS_U	9u
#define POINTS_V	5u
#define SAFE_RX_DAC	2048u

static uint8_t table[CAL_SIZE];
static bool valid;

static bool row_blank(const uint8_t *p, uint32_t n)
{
	bool all_ff = true, all_00 = true;
	for (uint32_t i = 0; i < n; i++) {
		all_ff &= p[i] == 0xFFu;
		all_00 &= p[i] == 0x00u;
	}
	return all_ff || all_00;
}

void md9600_cal_load(void)
{
	valid = md9600_flash_read_security(1, 0, table, 256) && md9600_flash_read_security(2, 0, table + 256, 256) &&
		!row_blank(table + ROW_RX_TUNE_U, POINTS_U) && !row_blank(table + ROW_RX_TUNE_V, POINTS_V);
}

const char *md9600_cal_source(void)
{
	return valid ? "factory" : "none";
}

uint16_t md9600_cal_rx_tune(uint32_t hz)
{
	if (!valid) {
		return SAFE_RX_DAC;
	}
	int32_t v[POINTS_U];
	uint32_t first, step;
	uint8_t n;
	const uint8_t *row;
	if (hz < MD9600_BAND_SPLIT_HZ) {
		row = table + ROW_RX_TUNE_V;
		n = POINTS_V;
		first = 136000000u;
		step = 9500000u;
	} else {
		row = table + ROW_RX_TUNE_U;
		n = POINTS_U;
		first = 400000000u;
		step = 10000000u;
	}
	for (uint8_t i = 0; i < n; i++) {
		v[i] = (int32_t)row[i] * 16;	// DAC value = byte x 16
	}
	int32_t dac = cal_curve_grid_at(first, step, v, n, hz, CAL_CURVE_ROUND | CAL_CURVE_EXTEND);
	return (uint16_t)(dac < 0 ? 0 : dac > 4095 ? 4095 : dac);
}
