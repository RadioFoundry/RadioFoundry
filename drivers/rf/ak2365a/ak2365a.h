// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// AKM AK2365A FM IF receiver (mixer, band-pass filter, AGC, limiter, discriminator, RSSI) on a three-wire bus
// (docs/spec/hardware-md9600.md 5.4). Register map and frame from the AK2365 data sheet (MS1453-E-02); the A part is
// taken to be the same (unverified).
//
// Frame: 16 clocks, MSB first, latched on the rising edge of SCLK while CSN is low: the R/W bit (0 = write), a 6-bit
// address, one low bit, then 8 data bits. As a word: reg << 9 | data. SCLK high and low times are at least 500 ns.
// After power-up the chip needs a hardware reset (RSTN low for at least 1 us); writing 0xAA to register 4 is a
// software reset that leaves it in standby 2.
#ifndef AK2365A_H
#define AK2365A_H

#include <stdint.h>
#include <stdbool.h>
#include "bus/wire3.h"

#define AK2365A_WORD_BITS	16u

enum {
	AK2365A_REG_CTRL1	= 0x01,	// BS[2:0] operation mode, BAND, BPF_BW[1:0], LOFREQ[1:0]
	AK2365A_REG_CTRL2	= 0x02,	// AGC_TIME[1:0], AGC1_STEP, CAL
	AK2365A_REG_CTRL3	= 0x03,	// IFOG[1:0]: IF buffer gain 6 / 9 / 12 / 15 dB
	AK2365A_REG_SRST	= 0x04,	// write AK2365A_SRST_VALUE: software reset
	AK2365A_REG_CTRL4	= 0x0B,	// BPF_BW[2], AGC_OFF, AGC1_G[5:0]
	AK2365A_REG_CTRL5	= 0x0C,	// AGC2_G[4:0]
};

#define AK2365A_SRST_VALUE	0xAAu
#define AK2365A_CTRL2_CAL	0x01u	// start the discriminator calibration; it takes 1.3 ms and clears itself

// Control register 1: operation mode bs (7 = everything on), wide (demodulated level for 3 kHz deviation instead of
// 1.5 kHz), band-pass filter bpf (0 = F1 +-6 kHz .. 3 = F4 +-2 kHz), LO lofreq (0 = 45.9, 1 = 50.4, 2 = 57.6 MHz)
#define AK2365A_CTRL1(bs, wide, bpf, lofreq) \
	((uint8_t)((((bs) & 7u) << 5) | ((wide) ? 0x10u : 0u) | (((bpf) & 3u) << 2) | ((lofreq) & 3u)))

static inline uint16_t ak2365a_word(uint8_t reg, uint8_t data)
{
	return (uint16_t)(((uint16_t)(reg & 0x3Fu) << 9) | data);
}

// The board's register values for receive
typedef struct {
	uint8_t ctrl1_wide, ctrl1_narrow;	// 25 kHz and 12.5 kHz channels
	uint8_t ctrl2, ctrl3, ctrl4, ctrl5;
} ak2365a_cfg_t;

#define AK2365A_SETUP_WORDS	6u

// Pure: software reset, then control registers 1, 2, 3, 4, 5 for the channel width, in write order
uint8_t ak2365a_setup_words(const ak2365a_cfg_t *c, bool wide, uint16_t out[AK2365A_SETUP_WORDS]);
// Write them on chip select cs
bool ak2365a_setup(const wire3_bus_t *bus, uint8_t cs, const ak2365a_cfg_t *c, bool wide);

#endif
