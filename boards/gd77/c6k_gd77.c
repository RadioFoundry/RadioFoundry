// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// GD-77 HR_C6000 board values (c6k_gd77.h). The shared power-on sequence leaves the chip in DMR layer 2 receive with
// the values the HR_C6000 radios have in common; this radio then sets its own RF timing advances, signal levels,
// auxiliary tables and codec defaults. Registers the mode functions own (0x01, 0x10, 0x26, 0x34-0x37, 0x40, 0x41,
// 0x5F, 0x81, 0xE0, 0xE4) are left to them. Unverified on a radio.
#include "c6k_gd77.h"

// Page 1 auxiliary tables
static const uint8_t aux_10[32] = {
	0x69, 0x69, 0x96, 0x96, 0x96, 0x99, 0x99, 0x99, 0xA5, 0xA5, 0xAA, 0xAA, 0xCC, 0xCC, 0x00, 0xF0,
	0x01, 0xFF, 0x01, 0x0F, 0x00, 0x00, 0x00, 0x00, 0x10, 0x70, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
static const uint8_t aux_30[16] = {
	0x00, 0x00, 0x14, 0x1E, 0x1A, 0xFF, 0x3D, 0x50, 0x07, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
static const uint8_t aux_40[7] = { 0x00, 0x03, 0x01, 0x02, 0x05, 0x1E, 0xF0 };
static const uint8_t aux_51[5] = { 0x00, 0x00, 0xEB, 0x78, 0x67 };

const gd77_c6k_row_t gd77_c6k_rows[] = {
	{ 4, 0x0D, 0x10 },	// voice superframe
	{ 4, 0x0E, 0x8E },
	{ 4, 0x0F, 0xB8 },
	{ 4, 0x12, 0x17 },	// rf_pre_on: the RF TX interrupt about 2.3 ms before a burst
	{ 4, 0xC0, 0x0A },	// rf_pre_on_rx: the RF RX interrupt's advance
	{ 4, 0x27, 0x40 },
	{ 4, 0x28, 0x7D },
	{ 4, 0x29, 0x40 },
	{ 4, 0x2A, 0x0B },
	{ 4, 0x2B, 0x0B },	// Sql_l
	{ 4, 0x2C, 0x17 },	// Sql_h
	{ 4, 0x2D, 0x05 },
	{ 4, 0x2E, 0x04 },	// tx_pre_on
	{ 4, 0x2F, 0x0B },	// I2S bit clock: 12.288 MHz / (2 x (0x0B + 1)) = 512 kHz
	{ 4, 0x32, 0x02 },	// I2S frame sync: 12.288 MHz / (2 x (0x2FF + 1)) = 8 kHz
	{ 4, 0x33, 0xFF },
	{ 4, 0x4B, 0x1B },
	{ 4, 0x4C, 0x00 },
	{ 4, 0xC2, 0x00 },	// codec AGC off
	{ 4, 0xD1, 0xC4 },
	{ 4, 0xE1, 0x0F },
	{ 4, 0xE2, 0x06 },
	{ 4, 0xE3, 0x52 },
	{ 4, 0xE5, 0x1A },
};
const uint8_t gd77_c6k_row_count = sizeof(gd77_c6k_rows) / sizeof(gd77_c6k_rows[0]);

bool gd77_c6k_board_init(hrc_t *dev)
{
	dev->mic_input = GD77_MIC_INPUT;
	dev->mic_gain_base = GD77_MIC_GAIN_BASE;
	bool ok = hrc_write_burst(dev, HRC_PAGE_AUX, 0x10, aux_10, sizeof aux_10);
	ok &= hrc_write_burst(dev, HRC_PAGE_AUX, 0x30, aux_30, sizeof aux_30);
	ok &= hrc_write_burst(dev, HRC_PAGE_AUX, 0x40, aux_40, sizeof aux_40);
	ok &= hrc_write_burst(dev, HRC_PAGE_AUX, 0x51, aux_51, sizeof aux_51);
	for (uint8_t i = 0; i < gd77_c6k_row_count; i++) {
		ok &= hrc_write(dev, gd77_c6k_rows[i].page, gd77_c6k_rows[i].reg, gd77_c6k_rows[i].val);
	}
	ok &= hrc_set_mic_gain(dev, GD77_MIC_GAIN);
	return ok;
}

bool gd77_c6k_pcm_out(hrc_t *dev, bool dmr)
{
	uint8_t v;
	bool ok = hrc_read(dev, 4, 0xE2, &v);
	if (ok && (v & 0x02u) == 0) {
		ok = hrc_write(dev, 4, 0xE2, (uint8_t)(v | 0x02u));
	}
	if (ok && hrc_read(dev, 4, 0xE0, &v) && (v & 0x08u) == 0) {
		ok = hrc_write(dev, 4, 0xE0, (uint8_t)(v | 0x08u));
	}
	if (ok && dmr && hrc_read(dev, 4, 0x22, &v) && (v & 0x06u) != 0x06u) {
		ok = hrc_write(dev, 4, 0x22, (uint8_t)(v | 0x06u));
	}
	return ok;
}
