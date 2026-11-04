// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// FD6818 in a superheterodyne: the main transceiver (receives at the IF, transmits on the channel) and the local
// oscillator, an FD6818 or its 0x7813 sibling (docs/spec/hardware-db25d.md 4.8). Independent of the board.
//
// This is the chip's register map with the state at 0x03 and a page select at 0x7F, as its datasheet lays it out;
// fd6818.h drives the same part through another map. Frequency in 0x38 / 0x39 (10 Hz units) with a band word in 0x1A,
// sub-audio 0x07 / 0x08 / 0x51 / 0x52, AF output 0x47, tones 0x70-0x72, RSSI 0x67. The values are the ones a working
// firmware writes.
//
// The board passes the chip's register bus (rf/rfbus/rfbus.h).
#ifndef FD6818SH_H
#define FD6818SH_H

#include <stdint.h>
#include <stdbool.h>
#include "rf/rfbus/rfbus.h"

#define FD6818SH_ID		0x6818u
#define FD6818SH_ID_7813	0x7813u		// the sibling part, seen only as an LO

#define FD6818SH_REG_STATE	0x03
#define FD6818SH_REG_PAGE	0x7F		// 1 = page 1, for the registers a table marks so

typedef enum { FD6818SH_MAIN, FD6818SH_LO, FD6818SH_LO_7813 } fd6818sh_role_t;
typedef enum { FD6818SH_TONE_NONE, FD6818SH_TONE_CTCSS, FD6818SH_TONE_DCS, FD6818SH_TONE_DCS_INV } fd6818sh_tone_t;

typedef struct {
	rfbus_t *bus;
	fd6818sh_role_t role;
	uint32_t freq_10hz;	// last frequency written (0 = none)
	uint16_t state;		// last 0x03 value
	uint16_t r25;		// the GPIO output register as last written
	uint16_t r7e;		// main: 0x7E after the init, the base of the digital value
	uint16_t r53;		// main: the analog audio gain word (the volume)
	uint16_t tone_hz;	// TX tone running, 0 = the microphone is live
	uint8_t tone_gain;
	uint8_t pa_gain;	// 0x28 level for the next transmission
	bool wide;		// analog bandwidth, kept across the digital mode
	bool digital;		// the DMR set is in effect
	bool tx_sub, tail_on;	// the transmission sends sub-audio; its tail is on
	bool rx_inverted;	// set by the board: a high-side LO turns the IF spectrum over, so DCS arrives inverted
} fd6818sh_t;

// 0x03 values. Every change goes through idle, as the working firmware does
#define FD6818SH_STATE_IDLE	0x0000u
#define FD6818SH_STATE_RX	0xBFF1u
#define FD6818SH_STATE_TX	0xC3FEu
#define FD6818SH_STATE_TX_TONE	0xC3FAu		// TX with the microphone ADC off, for a tone
#define FD6818SH_STATE_LO	0xC1FAu		// an LO's synthesizer output: PLL and PA driver, no audio

// 0x47 AF output: bits 11:8 pick the source
#define FD6818SH_AF_MUTE	0x7040u
#define FD6818SH_AF_RX		0x7140u
#define FD6818SH_AF_ALARM	0x7540u
#define FD6818SH_AF_SIDETONE	0x7840u
#define FD6818SH_AF_DMR		0x6140u		// RX audio in the digital mode, for the modem

// The main chip's GPIO0 output, set by fd6818sh_init_main: the sub-audio compare result (high = the tone or code set by
// fd6818sh_set_rx_tone is heard). The chip has no status bit for it
#define FD6818SH_GPIO_CSS	0

// --- Pure helpers ---

// Band word (0x1A) for a frequency: by the chip's band edges in MHz
uint16_t fd6818sh_band_word(uint32_t hz);
// 0x40 deviation word for a transmission on hz, with or without sub-audio
uint16_t fd6818sh_tx_dev(uint32_t hz, bool subaudio);
// 0x28 PA gain word for a calibration value 0..15
uint16_t fd6818sh_pa_word(uint8_t v);
// Page-1 0x17 microphone gain word for a level 0..63
uint16_t fd6818sh_mic_word(uint8_t v);
// 0x53 audio gain word for a volume 0..15
uint16_t fd6818sh_volume_word(uint8_t v);
// CTCSS and tone words: subaudio_ctcss_word() (truncated, as the working firmware's) and subaudio_tone_word()
// (truncated for a tone, rounded for DTMF)

// The 0x7813 LO's VCO section (0..8) for an LO frequency
uint8_t fd6818sh_7813_section(uint32_t hz);

// --- Main transceiver ---

void fd6818sh_attach(fd6818sh_t *dev, rfbus_t *bus, fd6818sh_role_t role);
// Power-on table, offset calibration, the GPIO set-up; ends idle. Task level
void fd6818sh_init_main(fd6818sh_t *dev);
// The analog mode set (also the end of DMR) and the digital one (DMR receive, the modem drives the modulation).
// Both leave the chip idle; the caller tunes and sets the state
void fd6818sh_analog_mode(fd6818sh_t *dev);
void fd6818sh_dmr_mode(fd6818sh_t *dev);
void fd6818sh_set_state(fd6818sh_t *dev, uint16_t state);
// Band word and frequency; the state is left as it is
void fd6818sh_tune(fd6818sh_t *dev, uint32_t hz);
void fd6818sh_set_bandwidth(fd6818sh_t *dev, bool wide);
void fd6818sh_set_af(fd6818sh_t *dev, uint16_t af);
// Volume 0..15 (analog only; the digital mode has its own gain)
void fd6818sh_set_af_gain(fd6818sh_t *dev, uint8_t gain);
void fd6818sh_set_rx_tone(fd6818sh_t *dev, fd6818sh_tone_t type, uint16_t value);
// FM TX words while still receiving: deviation, sub-audio, PA gain
void fd6818sh_tx_prepare(fd6818sh_t *dev, fd6818sh_tone_t type, uint16_t value, uint32_t hz, uint8_t pa_gain);
// Tune to hz and enter TX
void fd6818sh_tx_tune(fd6818sh_t *dev, uint32_t hz);
// Tones, tail and sub-audio off, back to RX (still on the TX frequency)
void fd6818sh_tx_end(fd6818sh_t *dev);
// Turn-off tail on (false and nothing written when the transmission sends no sub-audio) or off (true). The tail is
// the second tone slot: 55 Hz after CTCSS, 134.4 Hz after DCS
bool fd6818sh_tx_tail(fd6818sh_t *dev, bool on);
void fd6818sh_tone_start(fd6818sh_t *dev, uint16_t hz, uint8_t gain);	// hz 0 = stop
void fd6818sh_tone_key(fd6818sh_t *dev, bool down);
void fd6818sh_tone_stop(fd6818sh_t *dev);
void fd6818sh_dtmf_start(fd6818sh_t *dev, uint16_t low_hz, uint16_t high_hz);	// low 0 = stop
// DMR TX: the PA gain for the bursts, then the per-burst tune (TX on hz, or back to receive at hz)
void fd6818sh_dmr_tx_prepare(fd6818sh_t *dev, uint8_t pa_gain);
void fd6818sh_burst_tune(fd6818sh_t *dev, uint32_t hz, bool tx);

// --- LO (FD6818 or 0x7813) ---

// Power-on table, output level and GPIO set-up; ends off
void fd6818sh_init_lo(fd6818sh_t *dev);
// Tune (only when the frequency changed) and run the synthesizer
void fd6818sh_lo_tune(fd6818sh_t *dev, uint32_t hz);
void fd6818sh_lo_off(fd6818sh_t *dev);
void fd6818sh_lo_resume(fd6818sh_t *dev);

// Either role: a GPIO pin (0..7) as an output, high or low
void fd6818sh_gpio(fd6818sh_t *dev, uint8_t pin, bool high);

#endif
