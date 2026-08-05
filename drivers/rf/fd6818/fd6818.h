// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// FD6818 single-chip VHF/UHF transceiver (DM-32UV): chip driver, independent of the board
// (docs/spec/hardware-dm32.md 8). No datasheet exists; register values are the ones the working firmware uses.
//
// The chip has a three-wire serial port (chip select, clock, bidirectional data) that the board bit-bangs: the board
// supplies the pin operations and this driver builds the frames (8-bit address, bit 7 = read, then 16 data bits, MSB
// first, data changed while the clock is low and sampled on the rising edge). One register access is one frame,
// done inside the board's critical section so that the modem interrupts (DMR per-burst RX/TX switching) can use the
// same port: crit_enter / crit_exit may be NULL where nothing else touches it.
#ifndef FD6818_H
#define FD6818_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
	void (*cs)(bool level);		// chip select pin level (low = selected)
	void (*clk)(bool level);
	void (*data_out)(bool level);	// drive the data pin (makes it an output)
	void (*data_release)(void);	// data pin to input
	bool (*data_in)(void);
	void (*delay_us)(uint32_t us);
	uint32_t (*crit_enter)(void);	// optional
	void (*crit_exit)(uint32_t saved);
	void (*delay_ms)(uint32_t ms);	// init only
} fd6818_bus_t;

typedef struct {
	const fd6818_bus_t *bus;
	uint32_t freq_10hz;	// last frequency written, 10 Hz units (0 = none)
	uint16_t bw_reg;	// last 0x43 value
	uint16_t state;		// last 0x30 value
	uint32_t writes;	// frames sent (diagnostics)
} fd6818_t;

// Registers with a known meaning
#define FD6818_REG_CTCSS_FREQ	0x07
#define FD6818_REG_CSS_STATUS	0x0C	// high byte: b2 CTCSS, b7 DCS normal, b6 DCS inverted detected
#define FD6818_REG_BAND_A	0x1A
#define FD6818_REG_STATE	0x30
#define FD6818_REG_FREQ_LO	0x38	// 10 Hz units, low 16 bits
#define FD6818_REG_FREQ_HI	0x39
#define FD6818_REG_BAND_B	0x40
#define FD6818_REG_BANDWIDTH	0x43
#define FD6818_REG_AF		0x47	// 0x6042 = AF muted in FM
#define FD6818_REG_VOLUME	0x48
#define FD6818_REG_SUBAUDIO	0x51
#define FD6818_REG_NOISE	0x65
#define FD6818_REG_RSSI		0x67
#define FD6818_REG_TONE1	0x71

// 0x30 states: always through idle between RX and TX (8.3)
#define FD6818_STATE_IDLE	0x0000u
#define FD6818_STATE_RX		0xBFF1u
#define FD6818_STATE_TX		0xC1FEu
// 0x43 bandwidths
#define FD6818_BW_FM_25K	0x3028u
#define FD6818_BW_FM_12K5	0x4008u
#define FD6818_BW_DMR		0x4048u

typedef enum { FD6818_MODE_FM, FD6818_MODE_DMR } fd6818_mode_t;

// --- Pure helpers ---

// Band select words (0x1A, 0x40) for a frequency in 10 Hz units; false below 16 MHz (nothing is written then)
bool fd6818_band_words(uint32_t freq_10hz, uint16_t *r1a, uint16_t *r40);
// CTCSS frequency register for a tone in Hz x 10: Hz x 20.64888, rounded (the old port truncated the tone to whole
// tens of Hz first, hardware-dm32.md 16 #3)
uint16_t fd6818_ctcss_word(uint16_t hz_x10);
// RSSI raw value from reg 0x67 (bits 8:0, halved, uncalibrated)
static inline uint16_t fd6818_rssi_raw(uint16_t r67) { return (uint16_t)((r67 & 0x1FFu) >> 1); }

// --- Register access (one frame each, inside the critical section) ---

void fd6818_attach(fd6818_t *dev, const fd6818_bus_t *bus);
void fd6818_write(fd6818_t *dev, uint8_t reg, uint16_t val);
uint16_t fd6818_read(fd6818_t *dev, uint8_t reg);

// --- Sequences ---

// Power-on table, then the post-init writes (receiver at 144.000 MHz, AF muted). Task level
void fd6818_init(fd6818_t *dev);
// The FM or DMR register set (8.2 "Enter FM" / "Enter DMR"). Leaves the chip in RX
void fd6818_set_mode(fd6818_t *dev, fd6818_mode_t mode);
// Mute (0x30 = idle), band select, frequency (8.3 order). The state stays idle: set_state next
void fd6818_set_freq(fd6818_t *dev, uint32_t hz);
void fd6818_set_bandwidth(fd6818_t *dev, uint16_t bw_reg);
void fd6818_set_state(fd6818_t *dev, uint16_t state);
// Analog TX preparation writes (0x3F, 0x40, 0x37) before switching to TX (8.3, FM only)
void fd6818_fm_tx_prepare(fd6818_t *dev);
// TX CTCSS from the FD6818 (hz_x10 = 0: sub-audio off)
void fd6818_set_tx_ctcss(fd6818_t *dev, uint16_t hz_x10);
// RX CTCSS detect (0x51 = 0x1000 with the tone in 0x07), or off
void fd6818_set_rx_ctcss(fd6818_t *dev, uint16_t hz_x10);
// From an interrupt: idle then TX, or idle then RX with the bandwidth (DMR per-burst switching, 8.5)
void fd6818_dmr_burst(fd6818_t *dev, bool tx, uint16_t rx_bw);

#endif
