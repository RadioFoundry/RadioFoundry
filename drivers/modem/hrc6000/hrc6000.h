// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// HR_C6000 DMR / FM baseband chip: driver over its configuration port (U_SPI), independent of the board
// (docs/spec/modem-hrc6000.md, docs/spec/hardware-dm1701.md 4 and 12). Also drives its predecessor, the HR_C5000
// (MD-380), as a variant: same register interface and pages, different configuration page and a few registers that
// mean other things (docs/spec/hardware-md380.md 4). The board says which chip it has (hrc6000_attach_chip).
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

// Which chip of the family (hardware-md380.md 4.2)
typedef enum {
	HRC6000_CHIP_C6000 = 0,	// HR_C6000 (DM-1701, MD-UV380, MD-9600, GD-77)
	HRC6000_CHIP_C5000 = 1,	// HR_C5000 (MD-380 / MD-390 / RT3)
} hrc6000_chip_t;

typedef struct {
	const hrc6000_bus_t *bus;
	uint8_t chip;		// hrc6000_chip_t; 0 (the HR_C6000) when the board only calls hrc6000_attach
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
void hrc6000_attach_chip(hrc6000_t *dev, const hrc6000_bus_t *bus, hrc6000_chip_t chip);

// --- Chip differences (pure; hardware-md380.md 4.2). Callers name the HR_C6000's page 4 (HRC6000_PAGE_SYS) and the
// driver moves it to the chip's own configuration page ---

static inline bool hrc6000_is_c5000(const hrc6000_t *dev)
{
	return dev->chip == HRC6000_CHIP_C5000;
}
// The page actually sent for a page the caller names: the HR_C5000's configuration registers are on page 0
uint8_t hrc6000_page(const hrc6000_t *dev, uint8_t page);
// Reg 0x10 for DMR: HR_C6000 0x6E; HR_C5000 0x6B in direct mode, 0x4F on a repeater channel
uint8_t hrc6000_dmr_mode_value(const hrc6000_t *dev, bool repeater);
// CRC_MCU_Control (0x40 bit 3): the HR_C6000 can send a burst's CRC as given; the HR_C5000 always makes its own
bool hrc6000_has_mcu_crc(const hrc6000_t *dev);
// The sync type register 0x5F exists (HR_C6000 only)
bool hrc6000_has_sync_reg(const hrc6000_t *dev);
// Bytes of a data / control burst's page 2 payload the MCU writes when the chip adds the CRC itself: the HR_C5000
// appends the masked CRC-CCITT to CSBK, MBC header and data header bursts from the first 10 bytes (80 bits). 0: the
// burst goes as given (the full payload length)
uint8_t hrc6000_chip_crc_len(const hrc6000_t *dev, uint8_t kind);
// The low bits of reg 0x50 for voice burst index 0..5 (A..F): the HR_C5000 takes the embedded LC's LCSS from the MCU
// (A 0, B 1 first, C and D 3 continuation, E 2 last, F 0); the HR_C6000 inserts it itself (0)
uint8_t hrc6000_voice_lcss(const hrc6000_t *dev, uint8_t voice_index);
// Access policy and vocoder control (0x20-0x22) are written at each TX start on the HR_C6000 only; the HR_C5000 has
// them set once at init and must not get the HR_C6000's TX start values
bool hrc6000_tx_start_access(const hrc6000_t *dev);

// --- Register access ---
bool hrc6000_write(hrc6000_t *dev, uint8_t page, uint8_t reg, uint8_t val);
bool hrc6000_read(hrc6000_t *dev, uint8_t page, uint8_t reg, uint8_t *val);
bool hrc6000_write_burst(hrc6000_t *dev, uint8_t page, uint8_t reg, const uint8_t *data, uint16_t len);
bool hrc6000_read_burst(hrc6000_t *dev, uint8_t page, uint8_t reg, uint8_t *data, uint16_t len);
// 11-bit address write (page 1 tone table)
bool hrc6000_write_ext(hrc6000_t *dev, uint8_t page, uint16_t addr, uint8_t val);
// 11-bit address burst, read or write (the 1.2K RAMs: page 2 writes the TX RAM, reads the RX RAM)
bool hrc6000_ram(hrc6000_t *dev, uint8_t page, uint16_t addr, uint8_t *buf, uint16_t len, bool write);

// --- Chip operations ---

// Power-on initialization, modem-hrc6000.md section 3 steps 1-14. The board has already taken PWD low and waited
// 10 ms. Leaves the chip in its DMR layer 2 receive set-up; call a hrc6000_set_mode_*() next
bool hrc6000_power_on_init(hrc6000_t *dev, uint8_t dmr_mic_gain);
// The radio's own DMR ID (4:0x14-0x16): most significant byte first on the HR_C6000, least significant first on the
// HR_C5000
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

// --- HR_C5000 only (hardware-md380.md 4.3, 4.5) ---

// The small DMR register set for a TX or RX switch (never the full DMR set-up, which resets the chip's DMR state
// machine, gotchas 3.3): RX 0x0E, mic gain, 0x37, 0x25, 0x26 = 0xFD; TX adds 0x0D, 0x26 = 0xFE, the modulation pair,
// 0x35 and 0x3F. False on an HR_C6000 (nothing written)
bool hrc6000_c5000_dmr_path(hrc6000_t *dev, bool tx, uint8_t mod2, uint8_t mod1, uint8_t mic_gain);
// Reference oscillator from the factory tune byte (calibration offset 9): 0x47 = 0x04 = 0x7F - tune, 0x48 = 0x00 below
// 0x80 else 0x03 (gotchas 11.1)
bool hrc6000_c5000_set_ref(hrc6000_t *dev, uint8_t ref_tune);
// Pure: the reference oscillator bias DAC value for the tune byte (tune x 4 + 0x600; the board's MOD2_BIAS DAC)
uint16_t hrc6000_c5000_ref_dac(uint8_t ref_tune);

// Pure: the FM TX deviation coefficients for a band and bandwidth
hrc6000_fm_dev_t hrc6000_fm_deviation(bool uhf, bool wide);

#endif
