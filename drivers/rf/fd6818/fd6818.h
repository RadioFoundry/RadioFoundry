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
	uint8_t reg;
	uint16_t val;
} fd6818_regval_t;

// What differs between the boards' chip set-ups: the register tables, the 0x30 sequences and the band words. The
// DM-32UV's set is fd6818_variant_dm32 (the default); a board with other tables defines its own and passes it to
// fd6818_attach_variant
typedef struct {
	const fd6818_regval_t *init;		// power-on table, the soft reset first
	uint16_t init_n;
	const fd6818_regval_t *post_init;	// after init: receiver parked, AF muted
	uint16_t post_init_n;
	const fd6818_regval_t *fm;		// Enter FM
	uint16_t fm_n;
	const fd6818_regval_t *dmr;		// Enter DMR; NULL where the board has no DMR set-up yet
	uint16_t dmr_n;
	const fd6818_regval_t *rx;		// from idle into RX, the last write sets 0x30
	uint16_t rx_n;
	const fd6818_regval_t *tx;		// from idle into TX
	uint16_t tx_n;
	const fd6818_regval_t *fm_tx_prepare;	// before the switch to TX, FM only
	uint16_t fm_tx_prepare_n;
	// DMR per-burst switch straight from the other state, no idle on the way and no bandwidth write; NULL = idle, then
	// rx / tx (the DM-32UV)
	const fd6818_regval_t *burst_rx;
	uint16_t burst_rx_n;
	const fd6818_regval_t *burst_tx;
	uint16_t burst_tx_n;
	uint16_t bw_fm_25k, bw_fm_12k5, bw_dmr;	// 0x43 values
	uint32_t park_10hz;			// where post_init leaves the synthesizer, 0 if it doesn't tune
	// Band words for a frequency in 10 Hz units: *r1a == 0 leaves 0x1A alone; false writes neither
	bool (*band_words)(uint32_t freq_10hz, uint16_t *r1a, uint16_t *r40);
} fd6818_variant_t;

extern const fd6818_variant_t fd6818_variant_dm32;

typedef struct {
	const fd6818_bus_t *bus;
	const fd6818_variant_t *variant;
	uint32_t freq_10hz;	// last frequency written, 10 Hz units (0 = none)
	uint16_t bw_reg;	// last 0x43 value
	uint16_t state;		// last 0x30 value
	uint32_t writes;	// frames sent (diagnostics)
} fd6818_t;

// Registers with a known meaning
#define FD6818_REG_CTCSS_FREQ	0x07
#define FD6818_REG_DCS_CODE	0x08	// DCS word, low then high 12 bits (bit 15 marks the high half)
#define FD6818_REG_CSS_STATUS	0x0C	// b10 CTCSS, b14 DCS normal, b15 DCS inverted detected
#define FD6818_REG_BAND_A	0x1A
#define FD6818_REG_STATE	0x30
#define FD6818_REG_POWER	0x37	// block enables: 0x1F0F receive, 0x1D11 / 0x1D00 receiver asleep (FM / DMR)
#define FD6818_REG_FREQ_LO	0x38	// 10 Hz units, low 16 bits
#define FD6818_REG_FREQ_HI	0x39
#define FD6818_REG_BAND_B	0x40
#define FD6818_REG_BANDWIDTH	0x43
#define FD6818_REG_AF		0x47	// 0x6042 = AF muted in FM
#define FD6818_REG_VOLUME	0x48
#define FD6818_REG_SUBAUDIO	0x51
#define FD6818_REG_CSS_THRESH	0x52
#define FD6818_REG_NOISE	0x65
#define FD6818_REG_RSSI		0x67
#define FD6818_REG_TONE1	0x71

// 0x30 states: always through idle between RX and TX (8.3). RX and TX are requests to fd6818_set_state: the variant's
// sequence ends in these words but may write more before them. dev->state holds the last word written
#define FD6818_STATE_IDLE	0x0000u
#define FD6818_STATE_RX		0xBFF1u
#define FD6818_STATE_TX		0xC1FEu
// DM-32UV 0x43 bandwidths; fd6818_variant_t holds each board's
#define FD6818_BW_FM_25K	0x3028u
#define FD6818_BW_FM_12K5	0x4008u
#define FD6818_BW_DMR		0x4048u

typedef enum { FD6818_MODE_FM, FD6818_MODE_DMR } fd6818_mode_t;
typedef enum { FD6818_TONE_NONE, FD6818_TONE_CTCSS, FD6818_TONE_DCS, FD6818_TONE_DCS_INV } fd6818_tone_t;

// --- Pure helpers ---

// Band select words (0x1A, 0x40) for a frequency in 10 Hz units; false below 16 MHz (nothing is written then)
bool fd6818_band_words(uint32_t freq_10hz, uint16_t *r1a, uint16_t *r40);
// CTCSS frequency register for a tone in Hz x 10: Hz x 20.64888, rounded (the old port truncated the tone to whole
// tens of Hz first, hardware-dm32.md 16 #3)
uint16_t fd6818_ctcss_word(uint16_t hz_x10);
// Whether a 0x0C reading shows the tone the receiver was set up for (none: always true)
bool fd6818_tone_found(uint16_t r0c, fd6818_tone_t type);
// RSSI raw value from reg 0x67 (bits 8:0, halved: 1 dB steps)
static inline uint16_t fd6818_rssi_raw(uint16_t r67) { return (uint16_t)((r67 & 0x1FFu) >> 1); }
// RSSI in dBm: reg 0x67 bits 8:0 / 2 - 160 (programming guide, squelch section; reg 0x2C's external LNA gain is
// already taken off by the chip)
static inline int16_t fd6818_rssi_dbm(uint16_t r67) { return (int16_t)((int16_t)fd6818_rssi_raw(r67) - 160); }
// Noise for the core's squelch (hal_radio_t status, larger = more noise): reg 0x65 bits 7:0 plus an offset. Without a
// signal the raw value reads 43-63 (UHF 43-51 on most channels, VHF and 420 MHz 57-63), and a strong carrier reads 8 (a
// DM-1801 Pro on the SDR bench). The core's levels open below 64 (loosest) down to 40 (tightest), so the raw floor
// sits inside that range and every level opens on noise. The offset lifts the floor above 64 and keeps a strong
// carrier at 33
#define FD6818_NOISE_OFFSET	25u
static inline uint8_t fd6818_noise(uint16_t r65)
{
	uint16_t n = (uint16_t)((r65 & 0xFFu) + FD6818_NOISE_OFFSET);
	return n > 255u ? 255u : (uint8_t)n;
}

// --- Register access (one frame each, inside the critical section) ---

void fd6818_attach(fd6818_t *dev, const fd6818_bus_t *bus);	// the DM-32UV variant
void fd6818_attach_variant(fd6818_t *dev, const fd6818_bus_t *bus, const fd6818_variant_t *variant);
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
// Sub-audio detect for receive: CTCSS (value = Hz x 10), DCS normal or inverted (value = the 9-bit code), or off.
// The chip detects on its own demodulated audio; the result is polled from 0x0C (fd6818_rx_tone_ok). Bit 15 of 0x51
// also turns the chip's sub-audio transmitter on, so a radio that makes its tones elsewhere sets NONE before TX
void fd6818_set_rx_tone(fd6818_t *dev, fd6818_tone_t type, uint16_t value);
bool fd6818_rx_tone_ok(fd6818_t *dev, fd6818_tone_t type);
// Receiver power saving (8.8, the radio's own sequence): sleep = idle, then 0x37 to its low-power word (FM 0x1D11,
// DMR 0x1D00); wake = 0x37 back to the receive word (0x1F1F, then 0x1F0F) with the state still idle: the caller
// retunes and sets the RX state next (fd6818_set_freq, fd6818_set_state)
void fd6818_rx_sleep(fd6818_t *dev, bool sleep, fd6818_mode_t mode);
// From an interrupt: idle then TX, or idle then RX with the bandwidth (DMR per-burst switching, 8.5); a variant with
// burst tables writes only those
void fd6818_dmr_burst(fd6818_t *dev, bool tx, uint16_t rx_bw);
// The same switch for a split channel (RX and TX frequencies differ, a repeater or duplex hotspot): the synthesizer is
// retuned to hz first, so the receiver listens on the RX frequency between bursts and a burst goes out on the TX
// frequency. hz already tuned (simplex): no extra frames
void fd6818_dmr_burst_at(fd6818_t *dev, bool tx, uint16_t rx_bw, uint32_t hz);

#endif
