// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// GD-77 RF facts as data and pure rules (docs/spec/hardware-gd77.md 5): the AT1846S register profile, the band split of
// the RF path lines, the order of the transmit and receive switch sequences and the DAC0 ramp. No hardware access:
// radio_gd77.c drives the pins and the chips from these, and tests/host/gd77RfTest.c checks them. Every value is
// unverified until a radio is on the bench.
#ifndef GD77_RF_H
#define GD77_RF_H

#include <stdint.h>
#include <stdbool.h>
#include "rf/at1846s/at1846s.h"

// --- AT1846S ---

extern const at1846s_profile_t gd77_at1846s;
// Reg 0x05 for a frequency: 0x8543 or 0x8643 on a few exact frequencies, 0x8763 everywhere else
uint16_t gd77_at1846s_reg05(uint32_t hz);

// --- RF path lines ---

// The frequency at and above which the radio uses its UHF paths (the split between the two bands)
#define GD77_UHF_SPLIT_HZ	188743680u	// 0x01200000 x 10 Hz

// One step of a switch sequence
typedef enum {
	GD77_RF_RX_PATH_OFF,	// the band's receive path line low
	GD77_RF_RX_PATH_ON,	// the band's receive path line high, the other band's low
	GD77_RF_TX_PATH_OFF,	// the band's transmit path line low
	GD77_RF_TX_PATH_ON,	// the band's transmit path line high
	GD77_RF_TUNE,		// AT1846S: idle, reg 0x05, 0x29, 0x2A for the frequency
	GD77_RF_CHIP_TX,	// AT1846S reg 0x30 to transmit
	GD77_RF_CHIP_RX,	// AT1846S reg 0x30 to receive
	GD77_RF_TX_EN_ON,	// PTA17 high
	GD77_RF_TX_EN_OFF,	// PTA17 low
	GD77_RF_RAMP_UP,	// DAC0 from 0 to the drive in GD77_RAMP_STEPS
	GD77_RF_RAMP_DOWN,	// DAC0 from the drive to 0
	GD77_RF_RX_DRIVE,	// DAC0 to the receive value, when calibration has one
} gd77_rf_step_t;

// The switch sequences, in order. Transmit: receiver path off, tune, chip to TX, transmit path on, TX enable, ramp
// up. Receive: TX enable off, ramp down, transmit path off, tune, chip to RX, receive path on, receive DAC value
extern const uint8_t gd77_rf_tx_seq[6];
extern const uint8_t gd77_rf_rx_seq[7];

// The pins of one band (indices into the board's pin list, see radio_gd77.c)
typedef struct {
	bool uhf;
	uint8_t rx_on, rx_other;	// the receive path line for this band, and the other band's
	uint8_t tx_on;			// the transmit path line for this band
} gd77_rf_band_t;

enum { GD77_LINE_TX_UHF, GD77_LINE_TX_VHF, GD77_LINE_RX_UHF, GD77_LINE_RX_VHF, GD77_LINE_COUNT };

gd77_rf_band_t gd77_rf_band(uint32_t hz);

// --- DAC0 ---

#define GD77_RAMP_STEPS		19u
#define GD77_DAC_SHIFT		4u	// DAC0 = the 8-bit drive x 16

// DAC0 value at ramp step k (1..GD77_RAMP_STEPS) going up to drive (8-bit unit), or down from it
uint16_t gd77_ramp_value(uint8_t drive, uint8_t k, bool up);
// Microseconds to wait after step k: short steps far from full drive, longer ones near it
uint16_t gd77_ramp_wait_us(uint8_t k, bool up);

#endif
