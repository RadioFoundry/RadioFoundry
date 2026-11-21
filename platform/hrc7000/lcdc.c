// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The i8080 LCD controller (docs/spec/hardware-dm32.md 3.10): one byte a write, RS from the register written. Shared by
// every board with a panel on it; the board hands the bus pins to the controller and takes any lock its pins need.
//
// A full 320x240 frame is 153,600 bytes, so the per-byte loop decides how long the UI holds the CPU: a delay loop
// after each byte and a frame converted into a line buffer first took 135 ms a frame on a 168 MHz core, these loops
// 27 ms with the same timing word.
#include "platform.h"

#define LCDC_CMD		0x00u	// write: command byte (RS low)
#define LCDC_DATA		0x04u	// write: data byte (RS high)
#define LCDC_CFG		REG32(LCDC_BASE + 0x10)
#define LCDC_EN			REG32(LCDC_BASE + 0x28)	// b0 enable, b1 ready
#define LCDC_CFG_VALUE		0x303u
#define LCDC_READY_SPINS	100000u
// Cycles to let the last byte's strobe finish before a board gives the pins back to another user (a keypad on the same pins)
#define LCDC_DRAIN_SPINS	4u

// One byte to the panel (the host test, tests/host/hrc7000LcdcTest.c, records them instead)
#ifndef LCDC_WRITE
#define LCDC_WRITE(reg, v)	(REG32(LCDC_BASE + (reg)) = (v))
#endif

void hrc_lcdc_start(void)
{
	LCDC_CFG = LCDC_CFG_VALUE;
	LCDC_EN |= 1u;
	for (uint32_t n = LCDC_READY_SPINS; n != 0 && (LCDC_EN & 2u) == 0; n--) {
	}
}

void hrc_lcdc_drain(void)
{
	for (volatile uint32_t i = 0; i < LCDC_DRAIN_SPINS; i++) {
	}
}

void hrc_lcdc_cmd(uint8_t c)
{
	LCDC_WRITE(LCDC_CMD, c);
}

void hrc_lcdc_data(const uint8_t *d, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++) {
		LCDC_WRITE(LCDC_DATA, d[i]);
	}
}

void hrc_lcdc_pixels(const uint16_t *px, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++) {
		uint32_t c = px[i];
		LCDC_WRITE(LCDC_DATA, c >> 8);
		LCDC_WRITE(LCDC_DATA, c & 0xFFu);
	}
}

void hrc_lcdc_pixels4(const uint8_t *row, const uint16_t *palette, uint32_t n)
{
	for (uint32_t i = 0; i + 1u < n; i += 2u) {
		uint32_t b = row[i >> 1];
		uint32_t c0 = palette[b >> 4], c1 = palette[b & 0x0Fu];
		LCDC_WRITE(LCDC_DATA, c0 >> 8);
		LCDC_WRITE(LCDC_DATA, c0 & 0xFFu);
		LCDC_WRITE(LCDC_DATA, c1 >> 8);
		LCDC_WRITE(LCDC_DATA, c1 & 0xFFu);
	}
	if (n & 1u) {	// an odd width ends on a high nibble
		uint32_t c = palette[row[n >> 1] >> 4];
		LCDC_WRITE(LCDC_DATA, c >> 8);
		LCDC_WRITE(LCDC_DATA, c & 0xFFu);
	}
}
