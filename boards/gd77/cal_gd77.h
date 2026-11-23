// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// GD-77 factory calibration as the radio code consumes it (docs/spec/hardware-gd77.md 6.4): the values of the block
// for a frequency, converted to the register values they set on the AT1846S, the HR_C6000 and DAC0. The storage side
// (cal_blocks_gd77.h) reads the blocks; gd77_cal_at() picks the block and converts. radio_gd77.c holds a weak stand-in
// that reports "no calibration" with the safe values, so the board builds and receives without it. Without a valid
// point tx() refuses.
//
// The conversion follows the block's field table; none of it has been checked on a radio yet (unverified).
#ifndef GD77_CAL_H
#define GD77_CAL_H

#include <stdint.h>
#include <stdbool.h>

#define GD77_CAL_POWER_LEVELS	2	// board.power_count: index 0 low, 1 high
#define GD77_CAL_BWS		2	// per-bandwidth values, indexed as at1846s_bw_t: 0 = 12.5 kHz, 1 = 25 kHz

// The safe drive when nothing is loaded: none at all
#define GD77_CAL_SAFE_DRIVE	0u
// The drive byte the block's own rule gives outside its range. RadioFoundry never transmits there (the point is invalid), but
// gd77_cal_drive() reports it so the rule stays visible and tested
#define GD77_CAL_DRIVE_OUTSIDE	0xA0u
// The largest two-point bias a block may hold; above it the TX point is invalid
#define GD77_CAL_BIAS_MAX	0x400u

typedef struct {
	// TX (valid): the block for hz is valid, its TX range is one the node table knows and hz is inside it. False:
	// every TX value is the safe one and tx() refuses
	bool valid;
	// RX (rx_valid): the block for hz is valid, so the receive-side AT1846S values below come from it. False: keep the
	// radio profile's own values
	bool rx_valid;
	// sq_valid: hz is inside the block's RX range, so sq_open comes from it
	bool sq_valid;

	// --- TX ---
	uint8_t power_drive[GD77_CAL_POWER_LEVELS];	// DAC0 / 16 per board power level, interpolated over 5 MHz nodes
	uint8_t c6k_mod1;	// HR_C6000 4:0x46, modulation amplitude (FM and DMR), interpolated over 8 nodes
	uint16_t c6k_bias;	// HR_C6000 two-point bias: 4:0x47 = bits 7:0, 4:0x48 = the bits above
	uint8_t c6k_offset;	// HR_C6000 4:0x04, modulation DC offset
	uint8_t pa_drv;		// AT1846S reg 0x0A bits 14:11, PA driver current (0..15)
	uint8_t mic_pga;	// AT1846S reg 0x0A bits 10:6, microphone PGA gain (0..31)
	uint8_t voice_gain;	// AT1846S reg 0x41 bits 6:0, digital TX voice gain (0..127)
	uint8_t gain_tx;	// AT1846S reg 0x44 bits 11:8, digital gain after the TX ADC (0..15)
	uint16_t fm_dev[GD77_CAL_BWS];	// AT1846S reg 0x59 bits 15:6, FM deviation (0..1023)
	uint8_t ctcss_dev[GD77_CAL_BWS];	// AT1846S reg 0x59 bits 5:0 when sending CTCSS (or no tone)
	uint8_t dcs_dev[GD77_CAL_BWS];		// ... when sending DCS

	// --- RX (rx_valid, sq_valid) ---
	uint8_t rx_dac_gain;	// AT1846S reg 0x44 bits 7:4, DAC analog gain
	uint8_t rx_volume;	// AT1846S reg 0x44 bits 3:0, digital volume
	uint16_t sq_reg[GD77_CAL_BWS][3];	// AT1846S regs 0x48, 0x60, 0x3F: (high << 7) | low threshold pairs
	uint8_t sq_open;	// RSSI squelch base for the RX frequency, chip units (about 137 + dBm); not applied
} gd77_cal_point_t;

// The calibration at hz. False when no valid block is loaded for hz or hz is outside its TX range: the TX values are
// then the safe ones (rx_valid and sq_valid say what of the receive side is still from the block)
bool gd77_cal_at(uint32_t hz, gd77_cal_point_t *out);

// The same from one raw block (GD77_CAL_BLOCK bytes, already checked): pure, for the host tests
bool gd77_cal_convert(const uint8_t *raw, uint32_t hz, gd77_cal_point_t *out);
// The block's TX drive rule alone for a power level: GD77_CAL_DRIVE_OUTSIDE outside the TX range
uint8_t gd77_cal_drive(const uint8_t *raw, uint32_t hz, uint8_t level);

// Fallbacks for a byte outside its field's range (and the safe values' AT1846S part)
#define GD77_CAL_DEF_PA_DRV	5u
#define GD77_CAL_DEF_MIC_PGA	0x10u
#define GD77_CAL_DEF_VOICE_GAIN	0x1Eu
#define GD77_CAL_DEF_GAIN_TX	2u
#define GD77_CAL_DEF_FM_DEV	0x3Au
#define GD77_CAL_DEF_TONE_DEV	0x0Bu
#define GD77_CAL_DEF_DAC_GAIN	0xDu
#define GD77_CAL_DEF_VOLUME	0xCu
#define GD77_CAL_DEF_SQ_48	0x213Fu
#define GD77_CAL_DEF_SQ_60	0x1930u
#define GD77_CAL_DEF_SQ_3F	0x29D0u

// The safe values (no calibration)
static inline void gd77_cal_safe(gd77_cal_point_t *out)
{
	*out = (gd77_cal_point_t){
		.valid = false,
		.rx_valid = false,
		.sq_valid = false,
		.power_drive = { GD77_CAL_SAFE_DRIVE, GD77_CAL_SAFE_DRIVE },
		.c6k_mod1 = 0,
		.c6k_bias = 0,
		.c6k_offset = 0,
		.pa_drv = GD77_CAL_DEF_PA_DRV,
		.mic_pga = GD77_CAL_DEF_MIC_PGA,
		.voice_gain = GD77_CAL_DEF_VOICE_GAIN,
		.gain_tx = GD77_CAL_DEF_GAIN_TX,
		.fm_dev = { GD77_CAL_DEF_FM_DEV, GD77_CAL_DEF_FM_DEV },
		.ctcss_dev = { GD77_CAL_DEF_TONE_DEV, GD77_CAL_DEF_TONE_DEV },
		.dcs_dev = { GD77_CAL_DEF_TONE_DEV, GD77_CAL_DEF_TONE_DEV },
		.rx_dac_gain = GD77_CAL_DEF_DAC_GAIN,
		.rx_volume = GD77_CAL_DEF_VOLUME,
		.sq_reg = {
			{ GD77_CAL_DEF_SQ_48, GD77_CAL_DEF_SQ_60, GD77_CAL_DEF_SQ_3F },
			{ GD77_CAL_DEF_SQ_48, GD77_CAL_DEF_SQ_60, GD77_CAL_DEF_SQ_3F },
		},
		.sq_open = 0,
	};
}

#endif
