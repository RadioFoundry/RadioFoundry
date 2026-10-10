// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Beken BK4819 VHF/UHF transceiver: chip driver, independent of the board (docs/spec/hardware-db25d.md 4).
//
// One instance per chip. The DB25-D PRO has two on one 3-wire bus with separate enables: the "main" chip receives at
// the 51.55 MHz IF and transmits, the "LO" chip runs as the superhet's local oscillator. Register meanings are the
// BK4819's public ones; the init values are the ones the stock firmware writes.
//
// The bus is bit-banged by the board: 8-bit address (bit 7 = read), then 16 data bits, MSB first, data changed while
// the clock is low and sampled on the rising edge (the FD6818's framing). Each frame runs inside the board's critical
// section so an interrupt can use the bus too; crit_enter / crit_exit may be NULL where nothing else touches it.
#ifndef BK4819_H
#define BK4819_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
	void (*cs)(bool level);		// this chip's enable (low = selected)
	void (*clk)(bool level);
	void (*data_out)(bool level);	// drive the data pin (makes it an output)
	void (*data_release)(void);	// data pin to input
	bool (*data_in)(void);
	void (*delay_us)(uint32_t us);
	uint32_t (*crit_enter)(void);	// optional
	void (*crit_exit)(uint32_t saved);
	void (*delay_ms)(uint32_t ms);	// init only
} bk4819_bus_t;

typedef struct {
	const bk4819_bus_t *bus;
	uint32_t freq_10hz;	// last frequency written, 10 Hz units (0 = none)
	uint16_t state;		// last 0x30 value
	uint16_t af_gain;	// last 0x48 value
	uint32_t writes;	// frames sent (diagnostics)
} bk4819_t;

// Registers with a known meaning
#define BK4819_REG_ID		0x00	// reads 0x4819; bit 15 written = soft reset
#define BK4819_REG_CTCSS_FREQ	0x07
#define BK4819_REG_CSS_STATUS	0x0C	// high byte: b2 CTCSS found (as the FD6818)
#define BK4819_REG_ENABLES	0x30
#define BK4819_REG_PA		0x36	// PA bias and gain (the LO's output level)
#define BK4819_REG_POWER	0x37
#define BK4819_REG_FREQ_LO	0x38	// 10 Hz units, low 16 bits
#define BK4819_REG_FREQ_HI	0x39
#define BK4819_REG_BANDWIDTH	0x43
#define BK4819_REG_AF		0x47
#define BK4819_REG_AF_GAIN	0x48	// bits 3:0 AF DAC gain
#define BK4819_REG_SUBAUDIO	0x51
#define BK4819_REG_NOISE	0x65
#define BK4819_REG_RSSI		0x67
#define BK4819_REG_AFC		0x73	// bit 4 = AFC off
#define BK4819_REG_SQ_RSSI	0x78	// RSSI squelch: open threshold << 8 | close threshold, half dB + 160 dB

#define BK4819_ID		0x4819u

// 0x30 values: always through idle between RX and TX
#define BK4819_STATE_IDLE	0x0000u
#define BK4819_STATE_RX		0xBFF1u
#define BK4819_STATE_TX		0xC1FEu
// 0x37 values
#define BK4819_POWER_ON		0x1D0Fu
#define BK4819_POWER_OFF	0x1D00u
// 0x43 receive bandwidths (both among the values the stock firmware writes; which is which is INF)
#define BK4819_BW_25K		0x3028u
#define BK4819_BW_12K5		0x4048u
// 0x47 audio output: muted, or the FM demodulator (the AF select field, bits 11:8 = 1)
#define BK4819_AF_MUTE		0x6042u
#define BK4819_AF_FM		0x6140u
#define BK4819_AFC_OFF		0x0010u
// 0x36 for the LO: output level 15 of 15 (level x 63 / 15, bit 7 set), as the stock firmware
#define BK4819_LO_PA_MAX	0x00BFu

// --- Pure helpers ---

// RSSI in dBm from reg 0x67 (bits 8:0 in half dB, 160 dB offset), at the chip's input
static inline int16_t bk4819_rssi_dbm(uint16_t r67) { return (int16_t)((int16_t)((r67 & 0x1FFu) >> 1) - 160); }
// 0x78 for squelch opening at open_dbm and closing hyst_db lower, in reg 0x67's scale. Clamped to what the bytes hold
uint16_t bk4819_squelch_rssi(int16_t open_dbm, uint8_t hyst_db);
// CTCSS frequency word for a tone in Hz x 10 (Hz x 20.64888, as the FD6818)
uint16_t bk4819_ctcss_word(uint16_t hz_x10);

// --- Register access (one frame each, inside the critical section) ---

void bk4819_attach(bk4819_t *dev, const bk4819_bus_t *bus);
void bk4819_write(bk4819_t *dev, uint8_t reg, uint16_t val);
uint16_t bk4819_read(bk4819_t *dev, uint8_t reg);

// --- Sequences (hardware-db25d.md 4.3) ---

// Main chip: soft reset and the power-on table; ends idle with the audio muted. Task level
void bk4819_init_main(bk4819_t *dev);
// LO chip: soft reset, the power-on table and synthesizer set-up with output level pa (0x36); ends off
void bk4819_init_lo(bk4819_t *dev, uint16_t pa);
// Idle, then the frequency. The state stays idle: set_state next
void bk4819_set_freq(bk4819_t *dev, uint32_t hz);
// Never RX <-> TX directly: through idle
void bk4819_set_state(bk4819_t *dev, uint16_t state);
void bk4819_set_bandwidth(bk4819_t *dev, uint16_t bw_reg);
void bk4819_set_af(bk4819_t *dev, uint16_t af);
// AF DAC gain 0..15 (the received audio level)
void bk4819_set_af_gain(bk4819_t *dev, uint8_t gain);
// LO: run the synthesizer on hz (its transmitter stage drives the mixer), or stop it
void bk4819_lo_tune(bk4819_t *dev, uint32_t hz);
void bk4819_lo_off(bk4819_t *dev);
// RX CTCSS detect (hz_x10 = 0: off) and the detector's answer
void bk4819_set_rx_ctcss(bk4819_t *dev, uint16_t hz_x10);
bool bk4819_ctcss_found(bk4819_t *dev);

#endif
