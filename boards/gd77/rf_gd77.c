// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// GD-77 RF data and pure rules (rf_gd77.h, docs/spec/hardware-gd77.md 5.1, 5.4). Unverified on a radio.
#include "rf_gd77.h"

// --- AT1846S (hardware-gd77.md 5.1) ---

// After the soft reset: power on, the 26 MHz clock mode, the radio's register values, then the chip's own calibration
// (reg 0x30 0x40A4, 0x40A6, then 0x4006 with xtal_mode, pdn_reg and chip_cal_en)
static const at1846s_step_t init_steps[] = {
	{ 0x30, 0x0004, 0 },
	{ 0x04, 0x0FD0, 0 },
	{ 0x1F, 0x1000, 0 },	// GPIO6 = squelch output, the others Hi-Z
	{ 0x09, 0x03AC, 0 },
	{ 0x0F, 0x8C24, 0 },
	{ 0x13, 0xB100, 0 },
	{ 0x24, 0x0001, 0 },	// PLL lock detection on
	{ 0x31, 0x0031, 0 },
	{ 0x33, 0x45F5, 0 },
	{ 0x34, 0x2B89, 0 },
	{ 0x3F, 0x3263, 0 },
	{ 0x40, 0x0031, 0 },
	{ 0x41, 0x470F, 0 },
	{ 0x42, 0x1036, 0 },
	{ 0x43, 0x00BB, 0 },
	{ 0x44, 0x06FF, 0 },
	{ 0x47, 0x7F2F, 0 },
	{ 0x4E, 0x0082, 0 },
	{ 0x4F, 0x2C62, 0 },
	{ 0x53, 0x0094, 0 },
	{ 0x54, 0x2A3C, 0 },
	{ 0x55, 0x0081, 0 },
	{ 0x56, 0x0B02, 0 },
	{ 0x57, 0x1C00, 0 },
	{ 0x5A, 0x4935, 0 },
	{ 0x58, 0xBCCD, 0 },
	{ 0x62, 0x3263, 0 },
	{ 0x4E, 0x2082, 0 },
	{ 0x63, 0x16AD, 0 },
	{ 0x30, 0x40A4, 50 },
	{ 0x30, 0x40A6, 100 },
	{ 0x30, 0x4006, 10 },
};

// 12.5 / 25 kHz. Page 1 rows are registers 0x86-0x8F and 0x92 (reg 0x7F = 1 selects them)
static const at1846s_bw_row_t bw_rows[] = {
	{ 0, 0x15, { 0x1100, 0x1F00 } },
	{ 0, 0x32, { 0x4495, 0x7564 } },
	{ 0, 0x3A, { 0x40C3, 0x44C3 } },
	{ 0, 0x3F, { 0x28D0, 0x29D2 } },
	{ 0, 0x3C, { 0x0F1E, 0x0E1C } },
	{ 0, 0x48, { 0x1DB6, 0x1E38 } },
	{ 0, 0x62, { 0x1425, 0x3767 } },
	{ 0, 0x65, { 0x2494, 0x248A } },
	{ 0, 0x66, { 0xEB2E, 0xFF2E } },
	{ 1, 0x06, { 0x0014, 0x0024 } },
	{ 1, 0x07, { 0x020C, 0x0214 } },
	{ 1, 0x08, { 0x0214, 0x0224 } },
	{ 1, 0x09, { 0x030C, 0x0314 } },
	{ 1, 0x0A, { 0x0314, 0x0324 } },
	{ 1, 0x0B, { 0x0324, 0x0344 } },
	{ 1, 0x0C, { 0x0344, 0x0384 } },
	{ 1, 0x0D, { 0x1344, 0x1384 } },
	{ 1, 0x0E, { 0x1B44, 0x1B84 } },
	{ 1, 0x0F, { 0x3F44, 0x3F84 } },
	{ 1, 0x12, { 0xE0EB, 0xE0EB } },
};

// Exact frequencies (10 Hz units) that take another reg 0x05 value: multiples of 11.375 MHz
static const uint32_t reg05_8543[] = {
	13650000, 14787500, 15925000, 40950000, 42087500, 43225000, 44362500, 46637500, 47775000, 48912500, 50050000,
};
static const uint32_t reg05_8643[] = { 17062500, 50463417, 51187500 };

static bool in_list(uint32_t f10, const uint32_t *list, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++) {
		if (list[i] == f10) {
			return true;
		}
	}
	return false;
}

uint16_t gd77_at1846s_reg05(uint32_t hz)
{
	uint32_t f10 = hz / 10u;
	if (in_list(f10, reg05_8543, sizeof(reg05_8543) / sizeof(reg05_8543[0]))) {
		return 0x8543;
	}
	if (in_list(f10, reg05_8643, sizeof(reg05_8643) / sizeof(reg05_8643[0]))) {
		return 0x8643;
	}
	return 0x8763;
}

const at1846s_profile_t gd77_at1846s = {
	.init = init_steps,
	.init_len = sizeof(init_steps) / sizeof(init_steps[0]),
	.bw = bw_rows,
	.bw_len = sizeof(bw_rows) / sizeof(bw_rows[0]),
	// Reg 0x30: bits 13:12 the bandwidth (00 12.5 kHz, 11 25 kHz), bit 14 xtal_mode, bit 2 pdn_reg, bit 1
	// chip_cal_en; bit 5 rx_on; transmit sets bits 7:6 (tx_on with bit 7) in both modes
	.ctrl = {
		[AT1846S_STATE_OFF] = { 0x4006, 0x7006 },
		[AT1846S_STATE_IDLE] = { 0x4006, 0x7006 },
		[AT1846S_STATE_RX] = { 0x4026, 0x7026 },
		[AT1846S_STATE_FM_TX] = { 0x40C6, 0x70C6 },
		[AT1846S_STATE_DMR_TX] = { 0x40C6, 0x70C6 },
	},
	.no_mode_tables = true,
	.reg05 = gd77_at1846s_reg05,
};

// --- RF path lines (hardware-gd77.md 5.4) ---

const uint8_t gd77_rf_tx_seq[6] = {
	GD77_RF_RX_PATH_OFF, GD77_RF_TUNE, GD77_RF_CHIP_TX, GD77_RF_TX_PATH_ON, GD77_RF_TX_EN_ON, GD77_RF_RAMP_UP,
};
const uint8_t gd77_rf_rx_seq[6] = {
	GD77_RF_TX_EN_OFF, GD77_RF_RAMP_DOWN, GD77_RF_TX_PATH_OFF, GD77_RF_TUNE, GD77_RF_CHIP_RX, GD77_RF_RX_PATH_ON,
};

gd77_rf_band_t gd77_rf_band(uint32_t hz)
{
	bool uhf = hz >= GD77_UHF_SPLIT_HZ;
	return (gd77_rf_band_t){
		.uhf = uhf,
		.rx_on = uhf ? GD77_LINE_RX_UHF : GD77_LINE_RX_VHF,
		.rx_other = uhf ? GD77_LINE_RX_VHF : GD77_LINE_RX_UHF,
		.tx_on = uhf ? GD77_LINE_TX_UHF : GD77_LINE_TX_VHF,
	};
}

// --- DAC0 ramp ---

uint16_t gd77_ramp_value(uint8_t drive, uint8_t k, bool up)
{
	if (k > GD77_RAMP_STEPS) {
		k = GD77_RAMP_STEPS;
	}
	uint32_t full = (uint32_t)drive << GD77_DAC_SHIFT;
	uint32_t num = up ? k : GD77_RAMP_STEPS - k;
	return (uint16_t)(full * num / GD77_RAMP_STEPS);
}

// The drive changes slowly where the PA output is large: the last five steps up and the first five down
uint16_t gd77_ramp_wait_us(uint8_t k, bool up)
{
	bool near_full = up ? k + 5u > GD77_RAMP_STEPS : k <= 5u;
	return near_full ? 40u : 10u;
}
