// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// QA4818N / WX4818N-class VHF/UHF transceiver: chip driver, independent of the board.
//
// No data sheet was found. The register use is BK4819-like (frequency in 0x38/0x39, sub-audio in 0x07/0x08/0x51/0x52,
// noise and RSSI in 0x65/0x67) but the enables, the pages (0x7F) and the gain registers differ, so this is its own
// driver. Every value here is one the stock firmware of a radio built on it writes; what a register means is inferred
// from those values and the call patterns (docs/spec/hardware-d578uv.md 5, the only radio so far).
//
// A radio can use the chip in two roles on one 3-wire bus with separate enables: a transceiver (receives at an IF,
// transmits on the channel) and a local oscillator for it. One qa4818_t per chip; the board gives each its register
// bus (rf/rfbus/rfbus.h) and composes the superhet: qa4818_rx_plan() says where the LO goes for a receive frequency.
#ifndef QA4818_H
#define QA4818_H

#include <stdint.h>
#include <stdbool.h>
#include "rf/rfbus/rfbus.h"

typedef enum { QA4818_TONE_NONE, QA4818_TONE_CTCSS, QA4818_TONE_DCS, QA4818_TONE_DCS_INV } qa4818_tone_t;

typedef struct {
	rfbus_t *bus;
	bool lo;		// a local oscillator (sub-band 0 has its own 0x1A value)
	uint8_t subband;	// last VCO sub-band written, QA4818_SUBBAND_NONE after attach
	uint16_t gpio;		// shadow of the output register 0x25 (transceivers)
	uint8_t page;		// the page page() selected last (the register tables end on page 0)
} qa4818_t;

// Registers with a known use
#define QA4818_REG_RESET	0x00	// 0x8000 then 0: soft reset
#define QA4818_REG_ENABLE	0x03
#define QA4818_REG_CTCSS	0x07	// bits 12:0 tone word (Hz x 20.649), bits 15:13 which tone (0, then 1 latches)
#define QA4818_REG_DCS		0x08	// the 23-bit DCS word: bits 11:0, then bits 22:12 with bit 15 set
#define QA4818_REG_STATUS	0x0C	// bit 1: signal present
#define QA4818_REG_GPIO		0x25	// output pins (the radio's band switches)
#define QA4818_REG_FREQ_LO	0x38	// 10 Hz units, low 16 bits
#define QA4818_REG_FREQ_HI	0x39
#define QA4818_REG_DRIVE	0x40	// TX drive: band base + level
#define QA4818_REG_SUBAUDIO	0x51	// sub-audio mode; low 7 bits a calibrated TX level
#define QA4818_REG_CSS_DETECT	0x52	// CTCSS detector thresholds
#define QA4818_REG_TONE_FLAGS	0x60	// bit 0 CTCSS found, bit 1 DCS found
#define QA4818_REG_TONE_GEN	0x70	// the TX tone generator: 0xA000 on, 0 off (the only values the stock firmware writes)
#define QA4818_REG_TONE_FREQ	0x71	// its tone: Hz x 10.32, halved (qa4818_tone_word)
#define QA4818_REG_NOISE	0x65	// bits 6:0
#define QA4818_REG_RSSI		0x67	// bits 8:0, half dB + 160 (inferred)
#define QA4818_REG_SQ		0x78	// gain code << 8 | 0xFF
#define QA4818_REG_PAGE		0x7F	// 0 or 1: which page 0x10-0x3F address

// 0x03 values
#define QA4818_ENABLE_OFF	0x0000u
#define QA4818_ENABLE_RX	0xBFF5u		// a transceiver receiving
#define QA4818_ENABLE_LO	0xC1FEu		// a local oscillator running
#define QA4818_ENABLE_TX	0xC1FEu		// a transceiver transmitting (the same value)

// 0x28 values (the output stage, inferred)
#define QA4818_OUT_RX		0x0005u		// a transceiver receiving
#define QA4818_OUT_LO		0x0020u		// a local oscillator feeding a receiver
#define QA4818_OUT_TX_VHF	0x0018u		// a transceiver transmitting below 200 MHz
#define QA4818_OUT_TX_UHF	0x0009u

#define QA4818_SUBBANDS		9u
#define QA4818_SUBBAND_NONE	0xFFu
#define QA4818_SUBBAND_IF	7u		// a transceiver receiving at its IF

#define QA4818_RSSI_FLOOR	0x46u		// the lowest gain code the stock firmware uses as a threshold

typedef struct {
	uint32_t lo_hz;
	uint8_t lo_subband;
} qa4818_rx_plan_t;

// ---- Pure parts ----

// Frequency word in 10 Hz units, rounded to nearest
uint32_t qa4818_freq_word(uint32_t hz);
// Where the LO goes for receiving hz at if_hz: below (hz - IF) when that is at least 300 MHz, else above (hz + IF)
void qa4818_rx_plan(uint32_t hz, uint32_t if_hz, qa4818_rx_plan_t *p);
// The transceiver's VCO sub-band for transmitting on hz
uint8_t qa4818_tx_subband(uint32_t hz);
// CTCSS detector thresholds (0x52) for a tone in Hz x 10: per tone of the standard set, a catch-all otherwise
uint16_t qa4818_ctcss_detect(uint16_t hz_x10);
// The drive register (0x40) for transmitting on hz at drive level drive; narrow: 12.5 kHz (one step less)
uint16_t qa4818_drive_word(uint32_t hz, bool narrow, uint8_t drive);
// RSSI register reading to dBm (half dB + 160, inferred)
int16_t qa4818_rssi_dbm(uint16_t raw);
// A drive word (0x40) from a band base and a level, the stock firmware's rule: base | (level << 1) & 0xFF, one step
// (0x100) less for 12.5 kHz and one more when level << 1 carries
uint16_t qa4818_drive_raw(uint16_t base, uint8_t level, bool narrow);
// The tone generator's word (0x71) for hz: Hz x 10.32, truncated, halved (the stock firmware's arithmetic)
uint16_t qa4818_tone_word(uint16_t hz);
// The drive (0x40) while the tone generator sends on hz: fixed levels by band, the stock firmware's tone burst values
uint16_t qa4818_tone_drive_word(uint32_t hz, bool narrow);
// The drive (0x40) while the audio input pin sends on hz (the stock DTMF): the band's base and a calibrated level
// (the caller's: the stock firmware takes it from its calibration, by band)
uint16_t qa4818_line_drive_word(uint32_t hz, bool narrow, uint8_t level);

// ---- Register sequences ----

void qa4818_attach(qa4818_t *c, rfbus_t *bus, bool lo);
// Power-on init, the stock order: reset, constants, VCO calibration at 435.125 MHz (with two 20 ms waits), off
void qa4818_init_lo(qa4818_t *c);
// gpio: the output register's initial value. r49: the radio's 0x49 value (models differ)
void qa4818_init_trx(qa4818_t *c, uint16_t gpio, uint16_t r49);

void qa4818_enable(qa4818_t *c, uint16_t value);
// A transceiver's receiver on (its front-end constants, then QA4818_ENABLE_RX); its transmitter on (QA4818_ENABLE_TX)
void qa4818_rx_on(qa4818_t *c);
void qa4818_tx_on(qa4818_t *c);
// A local oscillator on (through off, as the stock firmware does)
void qa4818_lo_on(qa4818_t *c);
void qa4818_out(qa4818_t *c, uint16_t value);			// 0x28
// Sub-band (skipped when unchanged) then frequency
void qa4818_tune(qa4818_t *c, uint8_t subband, uint32_t hz);
// Output register: bits in mask take val's; written when it changes
void qa4818_gpio(qa4818_t *c, uint16_t mask, uint16_t val);

// The FM (analog) or DMR (digital) path of a transceiver, receive or transmit (both write it): mixer and filter
// constants, and for FM the 12.5 / 25 kHz choice (inferred)
void qa4818_path(qa4818_t *c, bool tx, bool digital, bool narrow, uint16_t r49);
// The receive-only part, after qa4818_path(); then the gain code (0x4F, 0x78)
void qa4818_rx_setup(qa4818_t *c, bool digital);
void qa4818_gain(qa4818_t *c, uint8_t code);
// Receive sub-audio: value is Hz x 10 (CTCSS) or the code (DCS)
void qa4818_rx_tone(qa4818_t *c, qa4818_tone_t type, uint16_t value);
// FM transmit, after qa4818_path(tx): the TX constants and output stage for hz, then the tone (cal_ctcss / cal_dcs: the
// calibrated 0x51 levels, low 7 bits), then the voice audio constants, the drive (0x40) and its 0x51 level (level:
// cal_ctcss or cal_dcs as the tone is). Keyed by qa4818_tx_on()
void qa4818_fm_tx(qa4818_t *c, uint32_t hz);
// DMR transmit, after qa4818_path(tx, digital): the output stage for hz, the digital TX constants, sub-audio off and
// drive word 0 (the baseband modulates; the board switches its PA per burst). Keyed by qa4818_tx_on() for the call
void qa4818_dmr_tx(qa4818_t *c, uint32_t hz);
void qa4818_tx_tone(qa4818_t *c, qa4818_tone_t type, uint16_t value, uint8_t cal_ctcss, uint8_t cal_dcs);
void qa4818_tx_drive(qa4818_t *c, uint32_t hz, bool narrow, uint8_t drive, uint8_t level);
// Drive off (0x40 = 0, 0x51 level 0), at the end of a transmission
void qa4818_tx_drive_off(qa4818_t *c);

// ---- FM TX tones, in place of the microphone (the board mutes the microphone itself) ----
// The modulator's audio for a tone, during FM TX: page 1 0x19, for the audio input pin (line) 0x18 and page 1 0x17
// bit 0 as well, the drive r40, page 1 0x1B = 5 (the tone path, inferred). The stock firmware's set-up before its 1750
// Hz burst (line false) and its DTMF, which it synthesizes on an MCU pin (line true)
void qa4818_tx_aux(qa4818_t *c, bool line, uint16_t r40);
// After qa4818_tx_aux(false): the tone generator on at hz (0x71, then 0x70)
void qa4818_tone_start(qa4818_t *c, uint16_t hz);
// The tone generator on (the tone set last) or off, the rest unchanged: keying a tone
void qa4818_tone_gen(qa4818_t *c, bool on);
// The microphone again after either: page 1 0x1B = 4 and 0x17 the voice value, the tone generator off, the voice
// drive r40 and its 0x51 level
void qa4818_tx_mic(qa4818_t *c, uint16_t r40, uint8_t level);

typedef struct {
	bool signal;		// 0x0C bit 1
	uint8_t noise;		// 0x65 bits 6:0
	uint16_t rssi;		// 0x67 bits 8:0
	uint16_t tone_flags;	// 0x60
} qa4818_status_t;

void qa4818_status(qa4818_t *c, qa4818_status_t *st);

#endif
