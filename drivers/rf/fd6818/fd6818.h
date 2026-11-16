// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// FD6818 single-chip VHF/UHF transceiver: chip driver, independent of the board
// (docs/spec/hardware-dm32.md 8). No datasheet exists; register values are the ones the working firmware uses.
//
// The chip has a three-wire serial port; the board gives the driver its register bus (rf/rfbus/rfbus.h, rfbus_3w_t),
// whose frames run inside the board's critical section so that the modem interrupts (DMR per-burst RX/TX switching)
// can use the same port.
#ifndef FD6818_H
#define FD6818_H

#include <stdint.h>
#include <stdbool.h>
#include "rf/rfbus/rfbus.h"
#include "config.h"


typedef rfbus_regval_t fd6818_regval_t;

// What differs between the boards' chip set-ups: the register tables, the 0x30 sequences and the band words. The
// default set is fd6818_variant_default; a board with other tables defines its own and passes it to
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
	// rx / tx (the default)
	const fd6818_regval_t *burst_rx;
	uint16_t burst_rx_n;
	const fd6818_regval_t *burst_tx;
	uint16_t burst_tx_n;
	uint16_t bw_fm_25k, bw_fm_12k5, bw_dmr;	// 0x43 values
	uint32_t park_10hz;			// where post_init leaves the synthesizer, 0 if it doesn't tune
	// fd6818_rx_sleep's 0x37 words: asleep in FM, asleep in DMR, awake. 0 = the default's (0x1D11, 0x1D00, then
	// 0x1F1F and 0x1F0F)
	uint16_t sleep_fm, sleep_dmr, sleep_wake;
	// Band words for a frequency in 10 Hz units: *r1a == 0 leaves 0x1A alone; false writes neither
	bool (*band_words)(uint32_t freq_10hz, uint16_t *r1a, uint16_t *r40);
} fd6818_variant_t;

extern const fd6818_variant_t fd6818_variant_default;
// Set-up B (fd6818_setup_b.c), for the parts whose synthesizer doesn't lock on the default, with two sets of DMR
// receive words
extern const fd6818_variant_t fd6818_variant_b;
extern const fd6818_variant_t fd6818_variant_b_dmr6943;

// The AM receiver's gain loop (fd6818_am_status): an index into the 0x13 gain table, the hold before the gain goes up
// again, and the last 0x67 reading
typedef struct {
	uint8_t idx;
	uint8_t hold;
	uint16_t prev;
} fd6818_agc_t;

typedef struct {
	rfbus_t *bus;
	const fd6818_variant_t *variant;
	uint32_t freq_10hz;	// last frequency written, 10 Hz units (0 = none)
	uint16_t bw_reg;	// last 0x43 value
	uint16_t state;		// last 0x30 value
#if CONFIG_AM_RX
	bool am;		// the AM set-up is on (fd6818_set_am)
	uint8_t am_noise_off;	// fd6818_am_noise's offset, from the variant's 0x4F
	uint16_t r13, r4a;	// 0x13 and 0x4A as they were before AM, written back when AM ends
	fd6818_agc_t agc;
#endif
} fd6818_t;

// Registers with a known meaning
#define FD6818_REG_CTCSS_FREQ	0x07
#define FD6818_REG_DCS_CODE	0x08	// DCS word, low then high 12 bits (bit 15 marks the high half)
#define FD6818_REG_CSS_STATUS	0x0C	// b10 CTCSS, b14 DCS normal, b15 DCS inverted detected; b9 synthesizer locked
#define FD6818_CSS_PLL_LOCK	0x0200u
#define FD6818_REG_BAND_A	0x1A
#define FD6818_REG_STATE	0x30
#define FD6818_REG_GAIN		0x13	// receiver gain: FM and DMR the variant's word; in AM the gain loop's table (fd6818_am_status)
#define FD6818_REG_POWER	0x37	// block enables: 0x1F0F receive, 0x1D11 / 0x1D00 receiver asleep (FM / DMR)
#define FD6818_REG_FREQ_LO	0x38	// 10 Hz units, low 16 bits
#define FD6818_REG_FREQ_HI	0x39
#define FD6818_REG_BAND_B	0x40
#define FD6818_REG_BANDWIDTH	0x43
#define FD6818_REG_AF		0x47	// AF output: 0x6042 = muted in FM, FD6818_AF_FM / FD6818_AF_AM below
#define FD6818_REG_VOLUME	0x48
#define FD6818_REG_AM_4A	0x4A	// FD6818_AM_4A in AM, else the variant's init word (if any); meaning not known
#define FD6818_REG_SQ_NOISE	0x4F	// squelch noise thresholds: noise_sq_th_out 14:8, noise_sq_th_in 6:0 (guide, squelch)
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
// The default 0x43 bandwidths; fd6818_variant_t holds each board's
#define FD6818_BW_FM_25K	0x3028u
#define FD6818_BW_FM_12K5	0x4008u
#define FD6818_BW_DMR		0x4048u
// AM: the narrowest receive filter (firlpf_bw 14:12 = 0, about 1.7 kHz; weak-signal bandwidth 0; narrow, bit 5 = 0)
#define FD6818_BW_AM		0x0048u

// 0x47 AF output words. Bits 11:8 pick the demodulator whose audio the output carries, as on the BK4819 class of chips
// (docs/spec/airband-and-broadcast.md 3.3): type 1 is FM, type 7 is AM. FD6818_AF_FM is the word the FM and DMR tables
// write (checked on the radio). FD6818_AF_AM is the radio's own air band word (hardware-dm32.md 8.7)
#define FD6818_AF_FM		0x0140u
#define FD6818_AF_AM		0x6740u
// The rest of the radio's own AM set-up (hardware-dm32.md 8.7): 0x13 at full gain (the top of the gain loop's table)
// and 0x4A
#define FD6818_GAIN_AM_FULL	0x03FFu
#define FD6818_AM_4A		0x5450u

typedef enum { FD6818_MODE_FM, FD6818_MODE_DMR } fd6818_mode_t;
typedef enum { FD6818_TONE_NONE, FD6818_TONE_CTCSS, FD6818_TONE_DCS, FD6818_TONE_DCS_INV } fd6818_tone_t;

// --- Pure helpers ---

// Band select words (0x1A, 0x40) for a frequency in 10 Hz units; false below 16 MHz (nothing is written then)
bool fd6818_band_words(uint32_t freq_10hz, uint16_t *r1a, uint16_t *r40);
// CTCSS frequency register: subaudio_ctcss_word(), rounded (the old port truncated the tone to whole tens of Hz first,
// hardware-dm32.md 16 #3)
// Whether a 0x0C reading shows the tone the receiver was set up for (none: always true)
bool fd6818_tone_found(uint16_t r0c, fd6818_tone_t type);
// RSSI raw value from reg 0x67 (bits 8:0, halved: 1 dB steps)
static inline uint16_t fd6818_rssi_raw(uint16_t r67) { return (uint16_t)((r67 & 0x1FFu) >> 1); }
// RSSI in dBm: reg 0x67 bits 8:0 / 2 - 160 (programming guide, squelch section; reg 0x2C's external LNA gain is
// already taken off by the chip)
static inline int16_t fd6818_rssi_dbm(uint16_t r67) { return (int16_t)((int16_t)fd6818_rssi_raw(r67) - 160); }
// A board's RSSI correction: the guide's formula reads 20-40 dB high behind a radio's front end, by an amount that
// changes with frequency (hardware-dm1801pro.md 5.4). Points by frequency, sorted, one band after the other
typedef struct {
	uint16_t mhz;
	int8_t db;	// added to fd6818_rssi_dbm
} fd6818_rssi_cal_t;
// The correction at hz: linear between the points of its band (a gap over 100 MHz between two points splits the
// bands), the nearest point's value outside them; 0 with no table
int8_t fd6818_rssi_cal_db(const fd6818_rssi_cal_t *t, uint8_t n, uint32_t hz);
// Noise for the core's squelch (hal_radio_t status, larger = more noise): reg 0x65 bits 7:0 plus an offset. Without a
// signal the raw value reads 43-63 (UHF 43-51 on most channels, VHF and 420 MHz 57-63), and a strong carrier reads 8 (on
// the SDR bench). The core's levels open below 64 (loosest) down to 40 (tightest), so the raw floor
// sits inside that range and every level opens on noise. The offset lifts the floor above 64 and keeps a strong
// carrier at 33
#define FD6818_NOISE_OFFSET	25u
// Reg 0x65 bits 7:0 plus offset, capped at 255 (FM and AM differ only in the offset)
static inline uint8_t fd6818_noise_plus(uint16_t r65, uint8_t offset)
{
	uint16_t n = (uint16_t)((r65 & 0xFFu) + offset);
	return n > 255u ? 255u : (uint8_t)n;
}
static inline uint8_t fd6818_noise(uint16_t r65) { return fd6818_noise_plus(r65, FD6818_NOISE_OFFSET); }

// --- Register access (one frame each, inside the critical section) ---

void fd6818_attach(fd6818_t *dev, rfbus_t *bus);	// the default variant
void fd6818_attach_variant(fd6818_t *dev, rfbus_t *bus, const fd6818_variant_t *variant);
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
// Polls 0x0C bit 9 for up to timeout_ms after a tune; true once the synthesizer is locked (FD6818_LOCK_READS reads
// in a row, 1 ms apart)
#define FD6818_LOCK_READS	3u
bool fd6818_pll_locked(fd6818_t *dev, uint32_t timeout_ms);
// Which of n set-ups (variants) the RF section takes: each attached in turn, tuned to hz in RX, the first whose
// synthesizer locks within timeout_ms. A later one can also lock a section an earlier one is right for, so when one
// after the first locks, the earlier ones are tried again with FD6818_DETECT_RETRY times the wait. Returns the index,
// the chip left on that set-up in RX; n when none locks, the chip left on v[0] in FM receive
#define FD6818_DETECT_RETRY	3u
uint8_t fd6818_detect(fd6818_t *dev, rfbus_t *bus, const fd6818_variant_t *const *v, uint8_t n, uint32_t hz,
		      uint32_t timeout_ms);
// Analog TX preparation writes (0x3F, 0x40, 0x37) before switching to TX (8.3, FM only)
void fd6818_fm_tx_prepare(fd6818_t *dev);
// TX CTCSS from the FD6818 (hz_x10 = 0: sub-audio off)
void fd6818_set_tx_ctcss(fd6818_t *dev, uint16_t hz_x10);
// Sub-audio detect for receive: CTCSS (value = Hz x 10), DCS normal or inverted (value = the 9-bit code), or off.
// The chip detects on its own demodulated audio; the result is polled from 0x0C (fd6818_rx_tone_ok). Bit 15 of 0x51
// also turns the chip's sub-audio transmitter on, so a radio that makes its tones elsewhere sets NONE before TX
void fd6818_set_rx_tone(fd6818_t *dev, fd6818_tone_t type, uint16_t value);
bool fd6818_rx_tone_ok(fd6818_t *dev, fd6818_tone_t type);
#if CONFIG_AM_RX
// The air band AM set-up (true) or back to FM (false), the radio's own sequence (hardware-dm32.md 8.7). On: 0x47 to
// the AM output, 0x13 to full gain, 0x4A to its AM word and the gain loop reset. The caller tunes as for FM receive
// with FD6818_BW_AM in 0x43, calls this on every FM-mode tune (am false outside the air band), and fd6818_am_status on
// each status reading. Off (from AM): 0x47 to the FM word, 0x13 and 0x4A to what they were before AM; outside AM it
// writes nothing. fd6818_set_mode ends AM first
void fd6818_set_am(fd6818_t *dev, bool am);
// In AM, one status reading (0x65, 0x67): rssi_dbm gets the gain the loop took off added back, noise the AM squelch
// value (fd6818_am_noise), and the gain loop takes its step (writing 0x13 when the gain changes). Outside AM it changes
// nothing and returns false
bool fd6818_am_status(fd6818_t *dev, uint16_t r65, uint16_t r67, int16_t *rssi_dbm, uint8_t *noise);
#endif
static inline bool fd6818_am_on(const fd6818_t *dev)
{
#if CONFIG_AM_RX
	return dev->am;
#else
	(void)dev;
	return false;
#endif
}

// --- AM gain loop and squelch (pure) ---

// The gain loop keeps a signal's 0x67 reading near FD6818_AGC_TARGET: a reading (averaged with the one before) 10 dB or
// more above it takes the gain down at once by the excess less 8 dB; one 6 dB or more below it raises the gain one step,
// once the gain has held for FD6818_AGC_HOLD readings. An empty channel reads up to about 8 dB over the target at full
// gain, so a noisy desk or a strong noise burst can take a few dB off; the squelch doesn't use 0x67 (fd6818_am_noise)
#define FD6818_AGC_TARGET	142u
#define FD6818_AGC_HOLD		30u
void fd6818_agc_reset(fd6818_agc_t *a);
bool fd6818_agc_step(fd6818_agc_t *a, uint16_t r67);	// true: the 0x13 word changed
uint16_t fd6818_agc_word(const fd6818_agc_t *a);	// 0x13
uint8_t fd6818_agc_atten_db(const fd6818_agc_t *a);	// gain taken off, dB (0 = full gain)
// The core's squelch value in AM (radio_status_t.noise): the chip's noise indicator, as in FM, with its own offset. The
// chip's squelch is two thresholds (programming guide, squelch section): RSSI 0x67 over 0x78 and noise_sq (0x65 bits
// 7:0, 1 dB/step) under noise_sq_th_in (0x4F bits 6:0). In AM the gain loop holds 0x67 near its target with or without
// a signal, so the RSSI half passes on noise and only noise_sq tells a carrier from an empty channel (59-72 without a
// signal on the bench, lower as a carrier rises). The offset puts the core's level 1 (opens below
// FD6818_SQL_OPEN_L1) at the chip's own noise_sq_th_in, as the board's tables set 0x4F; each level is 3 dB more
#define FD6818_SQL_OPEN_L1	64u	// the core's level 1 (radio_sm.h RADIO_SQL_OPEN_L1)
static inline uint8_t fd6818_am_noise_offset(uint16_t r4f)
{
	uint8_t th = (uint8_t)(r4f & 0x7Fu);
	return th < FD6818_SQL_OPEN_L1 ? (uint8_t)(FD6818_SQL_OPEN_L1 - th) : 0u;
}
static inline uint8_t fd6818_am_noise(uint16_t r65, uint8_t offset) { return fd6818_noise_plus(r65, offset); }
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
