// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// GD-77 factory calibration (docs/spec/hardware-gd77.md 6.4): two blocks in the EEPROM, one per band, read once at
// start-up and never written: pick the block by frequency, read a field by id. Every field comes back as raw bytes;
// what the fields mean and the values they give at a frequency is cal_gd77.h.
#ifndef CAL_BLOCKS_GD77_H
#define CAL_BLOCKS_GD77_H

#include <stdint.h>
#include <stdbool.h>
#include "cal/cal_block.h"

#define GD77_CAL_UHF_ADDR	0x7C00u		// EEPROM address of the block for 180 MHz and up
#define GD77_CAL_VHF_ADDR	0x7C70u		// below 180 MHz
#define GD77_CAL_BLOCK		0x70u		// bytes per block: 8 header, 103 field bytes, 1 spare
#define GD77_CAL_HEADER		8u
#define GD77_CAL_SPLIT_HZ	180000000u	// the band rule: the UHF block from here up

typedef enum {
	GD77_CAL_UHF = 0,
	GD77_CAL_VHF = 1,
	GD77_CAL_BANDS
} gd77_cal_band_t;

// Field ids in block order, with their sizes and contents (docs/spec/hardware-gd77.md 6.4, all unverified)
enum {
	GD77_CAL_ID_40 = 0x40,	// 3 bytes: two-point bias (16 bit), DC offset
	GD77_CAL_ID_41 = 0x41,	// 48 bytes: TX drive per 5 MHz node, low and high power byte each
	GD77_CAL_ID_42 = 0x42,	// 4 bytes: not used
	GD77_CAL_ID_43 = 0x43,	// 20 bytes: 8 squelch base nodes, then 6 threshold pairs (25, 12.5 kHz)
	GD77_CAL_ID_44 = 0x44,	// 2 bytes: not used
	GD77_CAL_ID_01 = 0x01,	// 8 bytes: modulation amplitude per node
	GD77_CAL_ID_30 = 0x30,	// 1 byte: codec DAC level, not applied
	GD77_CAL_ID_31 = 0x31,	// 1 byte: not used
	GD77_CAL_ID_32 = 0x32,	// 1 byte: tone transmit voice gain, not applied
	GD77_CAL_ID_33 = 0x33,	// 2 bytes: CTCSS deviation (25, 12.5 kHz)
	GD77_CAL_ID_34 = 0x34,	// 2 bytes: DCS deviation (25, 12.5 kHz)
	GD77_CAL_ID_50 = 0x50,	// 8 bytes: PA driver, mic PGA, voice gain, TX gain, FM deviation (16 bit, 25, 12.5 kHz);
				// the first three are range checked (below 0x20, 0x40, 0x10)
	GD77_CAL_ID_51 = 0x51,	// 3 bytes: RX DAC gain, RX volume, unused
};

extern const cal_layout_t gd77_cal_layout;

// Read both blocks through rd (EEPROM addresses). Call again to reload
void gd77_cal_load(cal_read_fn rd, void *ctx);
// The block's state: CAL_BLOCK_OK when it was read and passed the check (gd77_cal_check)
cal_block_state_t gd77_cal_state(gd77_cal_band_t band);
// The block for a frequency in Hz
gd77_cal_band_t gd77_cal_band(uint32_t hz);
// The block for hz is loaded and valid
bool gd77_cal_ok(uint32_t hz);
// Field id of the block for hz into out (at most max bytes): the bytes copied, 0 without a valid block or for an id
// the layout doesn't have. The caller keeps its uncalibrated default when it gets 0
uint8_t gd77_cal_field(uint32_t hz, uint8_t id, uint8_t *out, uint8_t max);
// The header's two ranges (type 0 at +0: RX, type 1 at +4: TX), 16-bit little endian in 100 kHz units: false
// without a valid block
bool gd77_cal_range(gd77_cal_band_t band, uint8_t type, uint16_t *lo, uint16_t *hi);
// The whole raw block (GD77_CAL_BLOCK bytes) for diagnostics, NULL unless it is valid
const uint8_t *gd77_cal_raw(gd77_cal_band_t band);

// The sanity check a block must pass: field 0x50's first three bytes below 0x20, 0x40, 0x10; both type 1 range values
// in 1360..5200 and outside 2601..3999; low below high
bool gd77_cal_check(const uint8_t *raw);

#endif
