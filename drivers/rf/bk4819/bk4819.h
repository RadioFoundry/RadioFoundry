// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Beken BK4819 VHF/UHF transceiver: chip driver, independent of the board (docs/spec/hardware-db25d.md 4).
//
// One instance per chip. A superhet radio can have two on one 3-wire bus with separate enables: the "main" chip receives at
// the 51.55 MHz IF and transmits, the "LO" chip runs as the superhet's local oscillator. Register meanings are the
// BK4819's public ones; the init values are the ones the stock firmware writes.
//
// The board gives each chip its register bus (rf/rfbus/rfbus.h), the 3-wire bus on the radios so far.
#ifndef BK4819_H
#define BK4819_H

#include <stdint.h>
#include <stdbool.h>
#include "rf/rfbus/rfbus.h"

typedef enum { BK4819_TONE_NONE, BK4819_TONE_CTCSS, BK4819_TONE_DCS, BK4819_TONE_DCS_INV } bk4819_tone_t;

typedef struct {
	rfbus_t *bus;
	uint32_t freq_10hz;	// last frequency written, 10 Hz units (0 = none)
	uint16_t state;		// last 0x30 value
	uint16_t af_gain;	// last 0x48 value
	uint16_t gpio;		// last 0x33 value
	bool digital;		// bk4819_dmr_rx_enter() is in effect
	uint16_t saved_27, saved_48;	// what it replaced, for bk4819_dmr_rx_end()
	uint16_t idle;		// this chip's idle value of 0x30: 0 for the LO, 0x0200 for the main chip (bit 9 stays set)
	uint16_t tone_hz;	// TX tone set by bk4819_tone_start(), 0 = the microphone is live
	uint8_t tone_gain;
	// The receiver ahead of the chip inverts the audio (a high-side LO turns the IF spectrum over, so the deviation's
	// sign flips): a DCS code then arrives as its inverse, and the chip flags it in the other 0x0C bit. Set by the board
	bool rx_inverted;
	bool tx_dcs;		// the transmission bk4819_tx_prepare() set up sends DCS, so it has a turn-off tail
	bool tail_on;		// bk4819_tx_tail_start() is in effect
} bk4819_t;

// Registers with a known meaning
#define BK4819_REG_ID		0x00	// reads 0x4819; bit 15 written = soft reset
#define BK4819_REG_CTCSS_FREQ	0x07	// CTCSS word, or the DCS bit rate
#define BK4819_REG_DCS_CODE	0x08	// DCS word: low 12 bits, then the high 12 bits with bit 15 set
#define BK4819_REG_CSS_STATUS	0x0C	// b10 CTCSS found, b14 DCS found, b15 inverted DCS found (as the FD6818)
#define BK4819_REG_CSS_DETECT	0x52	// b15 sub-audio tail; CTCSS found / lost thresholds (as the FD6818)
#define BK4819_REG_ENABLES	0x30
#define BK4819_REG_GPIO		0x33	// GPIO outputs: bits 14:8 output disable, bits 6:0 level (bk4819_gpio)
#define BK4819_REG_PA		0x36	// PA bias and gain (the LO's output level)
#define BK4819_REG_POWER	0x37
#define BK4819_REG_FREQ_LO	0x38	// 10 Hz units, low 16 bits
#define BK4819_REG_FREQ_HI	0x39
#define BK4819_REG_BANDWIDTH	0x43
#define BK4819_REG_AF		0x47
#define BK4819_REG_AF_GAIN	0x48	// bits 3:0 AF DAC gain
#define BK4819_REG_TX_AUDIO	0x50	// bit 15 set: transient while a tone is set up; the stock analog value is 0x3B18 (live)
#define BK4819_REG_SUBAUDIO	0x51
#define BK4819_REG_TONE_CTRL	0x70	// bit 15 tone 1 on, bits 14:8 its gain (bit 7 / bits 6:0: tone 2)
#define BK4819_REG_TONE1_FREQ	0x71	// tone 1 frequency word
#define BK4819_REG_TONE2_FREQ	0x72	// tone 2 frequency word (the stock DTMF digit's second word)
#define BK4819_REG_NOISE	0x65
#define BK4819_REG_RSSI		0x67
#define BK4819_REG_AF_FILTER	0x2B	// AF filter bypass bits (RX: 15 HPF, 14 LPF, 13 de-emphasis; TX: 10, 9, 8)
#define BK4819_REG_AGC_DC	0x7E	// AGC fix mode and index, DC filter bandwidths
#define BK4819_REG_AFC		0x73	// bit 4 = AFC off
#define BK4819_REG_SQ_RSSI	0x78	// RSSI squelch: open threshold << 8 | close threshold, half dB + 160 dB

#define BK4819_ID		0x4819u

// 0x30 values: always through idle between RX and TX. The main chip keeps bit 9 set in every state the stock firmware
// uses (idle 0x0200, RX 0xBFF1, TX 0xC3FE); the LO never has it (idle 0, TX stage 0xC1FE). BK4819_STATE_IDLE given to
// bk4819_set_state() means the chip's own idle value
#define BK4819_STATE_IDLE	0x0000u
#define BK4819_STATE_IDLE_MAIN	0x0200u
#define BK4819_STATE_RX		0xBFF1u
#define BK4819_STATE_TX		0xC1FEu		// the LO's carrier-only stage
#define BK4819_STATE_TX_MAIN	0xC3FEu		// the main chip's transmitter
#define BK4819_STATE_TX_TONE	0xC3FAu		// TX with the microphone ADC off (bit 2), for a tone
// 0x37 values
#define BK4819_POWER_ON		0x1D0Fu
#define BK4819_POWER_OFF	0x1D00u
// 0x43 receive bandwidths (both among the values the stock firmware writes; which is which is INF)
#define BK4819_BW_25K		0x3028u
#define BK4819_BW_12K5		0x4048u
#define BK4819_BW_DMR		0x4444u		// the stock digital set's value
// 0x4F goes with the bandwidth in the stock firmware's channel set
#define BK4819_4F_25K		0x3332u
#define BK4819_4F_12K5		0x3D3Cu
// 0x47 audio output: muted, or the FM demodulator (the AF select field, bits 11:8 = 1)
#define BK4819_AF_MUTE		0x6042u
#define BK4819_AF_FM		0x6140u
// The stock word is 0x6042 | select << 8 | digital: its FM audio on is 0x6142 (bit 1 kept), and a digital channel
// adds bit 0. RadioFoundry's FM keeps 0x6140 (heard working); DMR uses the stock value
#define BK4819_AF_DMR		0x6143u
// The digital set's AF gain word: AF DAC gain nibble 4, RX gain field (bits 9:4) 0x17 (analog: 0xB3B3)
#define BK4819_AF_GAIN_DMR	0xB174u
#define BK4819_AFC_OFF		0x0010u
// 0x36: output level 15 of 15 (level x 63 / 15, bit 7 set). The stock firmware writes it once to the main chip at
// detection and to the LO at its init, and never again
#define BK4819_PA_MAX		0x00BFu
#define BK4819_LO_PA_MAX	BK4819_PA_MAX
// TX deviation and sub-audio words the stock firmware writes to 0x40 / 0x51 before every transmission, with no
// sub-audio and with CTCSS
#define BK4819_TX_DEV_PLAIN	0x34D0u
#define BK4819_TX_DEV_CTCSS	0x34A8u
#define BK4819_SUBAUDIO_TX_NONE	0x1050u
#define BK4819_SUBAUDIO_TX_CTCSS	0x9045u
// DCS, receive and transmit alike: 0x51 b15 on, b13 inverted, b12 clear (DCS, not CTCSS), gain 0x30; 0x07 the bit
// rate (134.4 Hz). After the code, receive writes the rate again with bit 14 set (the stock value; meaning unknown)
#define BK4819_SUBAUDIO_DCS	0x8030u
#define BK4819_SUBAUDIO_DCS_INV	0x2000u
#define BK4819_DCS_RATE		0x0AD7u
#define BK4819_DCS_RATE_RX	0x4AD7u
#define BK4819_DCS_HIGH_HALF	0x8000u
// 0x50 in analog mode (bit 15 clear: microphone live) and while a tone is set up
#define BK4819_TX_AUDIO_LIVE	0x3B18u
#define BK4819_TX_AUDIO_TONE	0xBB18u
// 0x70 for a single tone: bit 15 on, gain in bits 14:8. The stock gain is 0x30
#define BK4819_TONE_GAIN_STOCK	0x30u

// --- Pure helpers ---

// RSSI in dBm from reg 0x67 (bits 8:0 in half dB, 160 dB offset), at the chip's input
static inline int16_t bk4819_rssi_dbm(uint16_t r67) { return (int16_t)((int16_t)((r67 & 0x1FFu) >> 1) - 160); }
// 0x78 for squelch opening at open_dbm and closing hyst_db lower, in reg 0x67's scale. Clamped to what the bytes hold
uint16_t bk4819_squelch_rssi(int16_t open_dbm, uint8_t hyst_db);
// CTCSS and tone generator words: subaudio_ctcss_word() (the stock transmitter's truncated, the receiver's rounded) and
// subaudio_tone_word() for DTMF (the stock table, rounded: 697 Hz = 7196, 1633 Hz = 16860). A single tone's word is the
// stock's own, Hz x 10.3244 truncated (bk4819_tone_start)
#define BK4819_TONE_GAIN_MAX	127u

// 0x36 for output level 0..15: 0 is off, anything else level x 63 / 15 with bit 7 set (the LO's formula, 4.3)
uint16_t bk4819_pa_reg(uint8_t level);

// --- Register access (one frame each, inside the critical section) ---

void bk4819_attach(bk4819_t *dev, rfbus_t *bus);
void bk4819_write(bk4819_t *dev, uint8_t reg, uint16_t val);
uint16_t bk4819_read(bk4819_t *dev, uint8_t reg);

// --- Sequences (hardware-db25d.md 4.3) ---

// Main chip: soft reset and the power-on table; ends idle with the audio muted. Task level
void bk4819_init_main(bk4819_t *dev);
// LO chip: soft reset, the power-on table and synthesizer set-up with output level pa (0x36); ends off
void bk4819_init_lo(bk4819_t *dev, uint16_t pa);
// Analog mode set of the main chip, as the stock firmware on entering an analog channel: 0x2A, AFC on (0x73 bit 4
// clear), AF select 0, 0x7E, 0x2B, 0x50 live microphone. Task level, idle chip
void bk4819_analog_mode(bk4819_t *dev);
// Idle, then the frequency. The state stays idle: set_state next
void bk4819_set_freq(bk4819_t *dev, uint32_t hz);
// Never RX <-> TX directly: through idle
void bk4819_set_state(bk4819_t *dev, uint16_t state);
// 0x52 detect thresholds, then the bandwidth word (BK4819_BW_*) and its 0x4F, as the stock channel set
void bk4819_set_bandwidth(bk4819_t *dev, uint16_t bw_reg);
void bk4819_set_af(bk4819_t *dev, uint16_t af);
// Drive one of the chip's GPIO outputs: pin p (0..6) is 0x33 bit 6 - p, its output
// disable bit 14 - p (cleared: the output on). The other pins keep their value; 0x33 is written only when it changes.
// A board wires them to its own uses
void bk4819_gpio(bk4819_t *dev, uint8_t pin, bool high);
// AF DAC gain 0..15 (the received audio level)
void bk4819_set_af_gain(bk4819_t *dev, uint8_t gain);
// DMR receive (hardware-db25d.md 4.5): the stock firmware's digital mode set for the main chip, written when it enters a
// digital channel: 0x2A, 0x51 = 0, AFC off, 0x52, 0x47, 0x7E, 0x43, 0x40 = 0, 0x27, 0x2B, 0x48, then the AF output on
// with the digital bit (0x47 = 0x6143). The modem samples the chip's AF output, so the output stays on in DMR. Idle the
// chip first; the caller tunes and sets the state. End: the stock analog mode set (AFC on, AF muted), 0x27 and 0x48 as
// they were before the enter; the 12.5 / 25 kHz bandwidth, 0x40 and 0x51 are the caller's again
void bk4819_dmr_rx_enter(bk4819_t *dev);
void bk4819_dmr_rx_end(bk4819_t *dev);

// LO: run the synthesizer on hz (its transmitter stage drives the mixer), or stop it. The stop is 0x30 = 0 only; 0x37
// stays on, as the stock firmware leaves it in TX. A tune to the frequency the chip already has doesn't rewrite it
void bk4819_lo_tune(bk4819_t *dev, uint32_t hz);
void bk4819_lo_off(bk4819_t *dev);
// TX, as the stock firmware does it (hardware-db25d.md 4.4). Prepare, while the chip is still in RX: the deviation
// word (0x40) and the sub-audio: 0x51, with CTCSS its word in 0x07 (value = Hz x 10), with DCS the rate in 0x07 and
// the code's Golay word in 0x08 (value = the 9-bit code)
void bk4819_tx_prepare(bk4819_t *dev, bk4819_tone_t type, uint16_t value);
// Tune to hz and enter the TX state (0x37, idle, frequency, 0x30 = 0xC3FE). The caller selects the board's PA afterwards
// (the stock waits a calibrated time first)
void bk4819_tx_tune(bk4819_t *dev, uint32_t hz);
// End of TX: tone and sub-audio generators off, then the main chip back to its RX state (still on the TX frequency, so
// the PA can ramp down before it moves). The caller sets up receive next
void bk4819_tx_end(bk4819_t *dev);
// DCS turn-off (hardware-db25d.md 4.7), still transmitting: tail_start sets 0x52 bit 15 and the chip sends the 134.4 Hz
// tail in place of the code; tail_end (or bk4819_tx_end) writes 0x52 back. False, and nothing written, when the
// transmission sends no DCS
bool bk4819_tx_tail_start(bk4819_t *dev);
void bk4819_tx_tail_end(bk4819_t *dev);
// TX tone: during the TX state, send a single tone of hz at gain (0..127, the tone generator's own level, which sets
// the deviation) instead of the microphone: 0x30 = 0xC3FA turns the microphone ADC off. A second start retunes. hz = 0
// is a stop
void bk4819_tone_start(bk4819_t *dev, uint16_t hz, uint8_t gain);
// Key the running tone: down = on, false = a silent carrier (the tone generator off, the microphone still out). No-op
// without a tone
void bk4819_tone_key(bk4819_t *dev, bool down);
// Tone off and the microphone back (TX state 0xC3FE, 0x40 as with CTCSS). No-op without a tone
void bk4819_tone_stop(bk4819_t *dev);
// A DTMF pair during the TX state, the stock DTMF sequence (hardware-db25d.md 4.4): tone 1 at low, tone 2 at high,
// both at the stock DTMF gain (0x70 = 0x9090), 0x40 = 0x348A, the microphone ADC off. A second start retunes; stop
// with bk4819_tone_stop. low = 0 is a stop
void bk4819_dtmf_start(bk4819_t *dev, uint16_t low_hz, uint16_t high_hz);
#define BK4819_DTMF_CTRL	0x9090u		// 0x70: tone 1 and tone 2 on, gain 0x10 each (stock)
#define BK4819_TX_DEV_DTMF	0x348Au		// 0x40 while DTMF sounds (stock)
// --- DMR transmit (hardware-db25d.md 4.5): the modem's 4FSK goes on the reference oscillator (single-point
// modulation), so the main chip sends a plain carrier, retuned for every burst ---

// The digital channel's TX words, written while the chip still receives: deviation off (0x40 = 0) and no sub-audio
// (0x51 = 0), the values the stock digital mode set leaves. FM's bk4819_tx_prepare() writes both again
#define BK4819_TX_DEV_OFF	0x0000u
#define BK4819_SUBAUDIO_OFF	0x0000u
void bk4819_dmr_tx_prepare(bk4819_t *dev);
// One burst's retune in the stock order: 0x38 / 0x39 = hz first (no idle before it), then 0x37 on, idle, state.
// Main chip: TX on f with BK4819_STATE_TX_MAIN, back to the IF with BK4819_STATE_RX. From the DMR RF task
void bk4819_burst_tune(bk4819_t *dev, uint32_t hz, uint16_t state);
// LO: its synthesizer back on after a burst on the frequency it kept (0x37 on, idle, 0xC1FE), the stock's three
// writes. bk4819_lo_off() is the other half
void bk4819_lo_resume(bk4819_t *dev);

// RX sub-audio detect: CTCSS (value = Hz x 10), DCS normal or inverted (value = the 9-bit code), or off. The chip
// decodes its own demodulated audio; the answer is polled from 0x0C
void bk4819_set_rx_tone(bk4819_t *dev, bk4819_tone_t type, uint16_t value);
// Whether a 0x0C reading shows the tone the chip hears (none: always true). The chip's view: see rx_inverted
bool bk4819_tone_found(uint16_t r0c, bk4819_tone_t type);
// The same for the channel's tone as sent on air: DCS polarity swapped when dev->rx_inverted
bool bk4819_heard(const bk4819_t *dev, uint16_t r0c, bk4819_tone_t type);
bool bk4819_rx_tone_ok(bk4819_t *dev, bk4819_tone_t type);

#endif
