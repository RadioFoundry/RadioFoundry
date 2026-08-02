// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// HR_C6000 DMR / FM baseband chip: driver over its configuration port (U_SPI), independent of the board
// (docs/spec/modem-hrc6000.md, docs/spec/hardware-dm1701.md 4 and 12).
// The board supplies one function that runs a whole chip-select-framed transaction and makes it atomic with respect
// to the chip's interrupt handlers. FM modes here; DMR (modem-hrc6000.md 3.1, 4-6) is in hrc6000_dmr.h.
#ifndef HRC6000_H
#define HRC6000_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
	// CS low, hdr_len header bytes, then len data bytes, CS high. tx NULL sends 0xFF; rx NULL discards what comes back
	bool (*transfer)(void *ctx, const uint8_t *hdr, uint8_t hdr_len, const uint8_t *tx, uint8_t *rx, uint16_t len);
	void (*delay_ms)(uint32_t ms);
	void (*delay_us)(uint32_t us);	// busy wait
	void *ctx;
	// Voice port (V_SPI, modem-hrc6000.md 1 and 1.1): one page 3 frame, write (buf sent) or read (buf filled).
	// Atomic like transfer, callable from the modem ISRs. NULL: page 3 over the config port (unverified on air)
	bool (*voice)(void *ctx, bool write, uint8_t *buf, uint8_t len);
} hrc6000_bus_t;

typedef struct {
	const hrc6000_bus_t *bus;
} hrc6000_t;

// Register pages (modem-hrc6000.md 1.1)
enum { HRC6000_PAGE_AUX = 1, HRC6000_PAGE_DATA = 2, HRC6000_PAGE_VOCODER = 3, HRC6000_PAGE_SYS = 4 };

// Which tone deviation register 0xA0 gets during FM TX
typedef enum { HRC6000_TONE_NONE, HRC6000_TONE_CTCSS, HRC6000_TONE_DCS } hrc6000_tone_t;

// FM TX deviation coefficients (hardware-dm1701.md 12.5, modem-hrc6000.md 8)
typedef struct {
	uint8_t voice;	// 4:0x35
	uint8_t ctcss;	// 4:0xA0 with CTCSS
	uint8_t dcs;	// 4:0xA0 with DCS
	uint8_t dtmf;	// 4:0xA0 for DTMF
} hrc6000_fm_dev_t;

typedef struct {
	bool uhf;		// above 300 MHz
	bool wide;		// 25 kHz channel
	uint8_t tone;		// hrc6000_tone_t
	uint8_t mod2;		// 4:0x45 (calibration "I", FM pair)
	uint8_t mod1;		// 4:0x46 (calibration "Q", FM pair)
	int8_t ref_offset;	// reference oscillator tune - 128 for the TX band
	uint8_t mic_gain;	// 0..31
} hrc6000_fm_tx_t;

void hrc6000_attach(hrc6000_t *dev, const hrc6000_bus_t *bus);

// --- Register access ---
bool hrc6000_write(hrc6000_t *dev, uint8_t page, uint8_t reg, uint8_t val);
bool hrc6000_read(hrc6000_t *dev, uint8_t page, uint8_t reg, uint8_t *val);
bool hrc6000_write_burst(hrc6000_t *dev, uint8_t page, uint8_t reg, const uint8_t *data, uint16_t len);
bool hrc6000_read_burst(hrc6000_t *dev, uint8_t page, uint8_t reg, uint8_t *data, uint16_t len);
// 11-bit address write (page 1 tone table)
bool hrc6000_write_ext(hrc6000_t *dev, uint8_t page, uint16_t addr, uint8_t val);

// --- Chip operations ---

// Power-on initialization, modem-hrc6000.md section 3 steps 1-14. The board has already taken PWD low and waited
// 10 ms. Leaves the chip in its DMR layer 2 receive set-up; call a hrc6000_set_mode_*() next
bool hrc6000_power_on_init(hrc6000_t *dev, uint8_t dmr_mic_gain);
// The radio's own DMR ID (4:0x14-0x16)
bool hrc6000_set_dmr_id(hrc6000_t *dev, uint32_t id);

// FM receive (modem-hrc6000.md 8). The audio feed-through is left as it was: see hrc6000_fm_audio()
bool hrc6000_set_mode_fm_rx(hrc6000_t *dev);
// FM transmit: two-point modulation of the microphone with the deviation for the band and bandwidth
bool hrc6000_set_mode_fm_tx(hrc6000_t *dev, const hrc6000_fm_tx_t *cfg);
// FM receive audio feed-through to the line out (4:0x36): on, or muted
bool hrc6000_fm_audio(hrc6000_t *dev, bool on);
// Reference oscillator offset (hardware-dm1701.md 12.4). fm_tx: the FM TX form (0x48 = 0)
bool hrc6000_set_ref_offset(hrc6000_t *dev, int8_t offset, bool fm_tx);
// Two-point modulation amplitudes from calibration (4:0x45 Mod2, 4:0x46 Mod1)
bool hrc6000_set_mod_gain(hrc6000_t *dev, uint8_t mod2, uint8_t mod1);
// Microphone gain 0..31 (4:0xE4)
// RX audio gain, -31..+31 dB (4:0x37 = 0x80 + 0x40 if positive + the low 5 bits of the gain, modem-hrc6000.md 2)
bool hrc6000_set_rx_gain(hrc6000_t *dev, int8_t db);
bool hrc6000_set_mic_gain(hrc6000_t *dev, uint8_t gain);

// Pure: the FM TX deviation coefficients for a band and bandwidth
hrc6000_fm_dev_t hrc6000_fm_deviation(bool uhf, bool wide);

#endif
