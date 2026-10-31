// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// What the HR_C6000 and HR_C5000 DMR / FM baseband chips have in common: the configuration port, the register pages
// and the chip description the shared DMR slot engine (hrc6000_dmr.h) works through. Each chip's own set-up and FM
// modes are in its driver (drivers/modem/hrc6000, drivers/modem/hrc5000). Nothing here knows which chip it is: the
// differences are the fields and functions of hrc_chip_t, filled in by the chip's driver.
// The board supplies one function that runs a whole chip-select-framed transaction and makes it atomic with respect
// to the chip's interrupt handlers.
#ifndef HRC_H
#define HRC_H

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
} hrc_bus_t;

// Register pages. HRC_PAGE_SYS is the configuration page, which the chip's description maps to the page it really
// sits on (the HR_C6000: 4, the HR_C5000: 0)
enum { HRC_PAGE_AUX = 1, HRC_PAGE_DATA = 2, HRC_PAGE_VOCODER = 3, HRC_PAGE_SYS = 4 };

typedef struct hrc hrc_t;

// What differs between the chips, as far as the shared code is concerned
typedef struct {
	uint8_t cfg_page;		// the page HRC_PAGE_SYS is on
	bool id_low_first;		// 4:0x14-0x16 take the DMR ID least significant byte first
	bool mcu_crc;			// CRC_MCU_Control: a burst's CRC can be sent as given (else the chip makes its own)
	bool sync_reg;			// the sync type register 0x5F exists
	bool mode_per_channel;		// 4:0x10 depends on the channel (repeater or not) and is rewritten when it changes
	uint8_t (*dmr_mode)(bool repeater);		// 4:0x10 for DMR
	// 4:0x52 bit 2 is the TC of the last burst received, 1 = TS2 starting, and changes only in slots opened for
	// receive: the repeater slot lock keeps both slots open
	bool tc_last_rx;
	// Bytes of a data / control burst's page 2 payload the MCU writes when the chip adds the CRC itself (kind is a
	// DMR_BURST_*); 0: the burst goes as given
	uint8_t (*crc_len)(uint8_t kind);
	// The low bits of reg 0x50 for voice burst index 0..5 (A..F): the embedded LC's LCSS where the MCU supplies it
	uint8_t (*voice_lcss)(uint8_t voice_index);
	// What the chip needs at the start and end of a DMR transmission, beyond the burst registers: access_reg is the
	// channel access policy byte. Either may be NULL
	void (*tx_begin)(hrc_t *dev, uint8_t access_reg);
	void (*tx_end)(hrc_t *dev);
	bool (*set_mic_gain)(hrc_t *dev, uint8_t gain);	// the DMR microphone gain, in the chip's own steps
	// Enter DMR from FM or from power-on: the chip's DMR set-up for layer 2 receive, with the DMR modulation pair
	bool (*dmr_enter)(hrc_t *dev, uint8_t mod2, uint8_t mod1, uint8_t mic_gain);
	// Received voice that the chip doesn't announce per burst (the HR_C5000): the engine starts the chip's decoder
	// when a call begins, reads the voice port at the timeslot ticks and stops the decoder when the call ends.
	// rx_voice_read gives one burst's three AMBE+2 air frames (27 bytes) and returns how many of them have a C0 code
	// word with at most one bit error (0-3), -1 if the read failed. Both NULL: voice comes with the per-burst reports
	// (the HR_C6000)
	int (*rx_voice_read)(hrc_t *dev, uint8_t out[27]);
	void (*rx_decode)(hrc_t *dev, bool on);
} hrc_chip_t;

// The codec input the microphone is wired to (0xE0 bits, hrc6000-registers.md 3.20): a board choice
#define HRC_MIC_LINEIN1		0x40u	// the default
#define HRC_MIC_MIC_P		0x02u

struct hrc {
	const hrc_bus_t *bus;
	const hrc_chip_t *chip;
	uint8_t mic_input;	// HRC_MIC_*: set in 0xE0 whenever the microphone is on
	uint8_t mic_gain_base;	// HR_C6000 0xE4 = this + the mic gain: the LINEOUT gain and mic stages a board wants (0x20)
};

// Bind a chip driver's description to a bus. Called by hrc6000_attach and hrc5000_attach, not by the boards
void hrc_attach(hrc_t *dev, const hrc_bus_t *bus, const hrc_chip_t *chip);

// --- Register access (the page is the caller's: HRC_PAGE_SYS is mapped to the chip's configuration page) ---
bool hrc_write(hrc_t *dev, uint8_t page, uint8_t reg, uint8_t val);
bool hrc_read(hrc_t *dev, uint8_t page, uint8_t reg, uint8_t *val);
bool hrc_write_burst(hrc_t *dev, uint8_t page, uint8_t reg, const uint8_t *data, uint16_t len);
bool hrc_read_burst(hrc_t *dev, uint8_t page, uint8_t reg, uint8_t *data, uint16_t len);
// 11-bit address write (page 1 tone table)
bool hrc_write_ext(hrc_t *dev, uint8_t page, uint16_t addr, uint8_t val);
// 11-bit address burst, read or write (the 1.2K RAMs: page 2 writes the TX RAM, reads the RX RAM)
bool hrc_ram(hrc_t *dev, uint8_t page, uint16_t addr, uint8_t *buf, uint16_t len, bool write);

// --- Shared operations ---

// The radio's own DMR ID (4:0x14-0x16), in the chip's byte order
bool hrc_set_dmr_id(hrc_t *dev, uint32_t id);
// Two-point modulation amplitudes from calibration (4:0x45 Mod2, 4:0x46 Mod1)
bool hrc_set_mod_gain(hrc_t *dev, uint8_t mod2, uint8_t mod1);
// The DMR microphone gain, through the chip's own register
bool hrc_set_mic_gain(hrc_t *dev, uint8_t gain);
// The vocoder buffer flush (modem-hrc6000.md 3, step 5): 128 bytes of 0x7F to page 3
bool hrc_flush_vocoder(hrc_t *dev);
// The MS-sourced data sync pattern for page 1 0x04 (ETSI TS 102 361-1; the chip sends the voice sync as its inverse)
extern const uint8_t hrc_ms_data_sync[6];

#endif
