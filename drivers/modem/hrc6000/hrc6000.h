// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// HR_C6000 DMR / FM baseband chip driver (docs/spec/radios.md): its power-on set-up and FM modes
// (docs/spec/modem-hrc6000.md, docs/spec/hardware-dm1701.md 4 and 12). The register access and the DMR slot engine
// are shared with the HR_C5000 (drivers/modem/hrc_common); the HR_C5000 has its own driver (drivers/modem/hrc5000).
#ifndef HRC6000_H
#define HRC6000_H

#include "modem/hrc_common/hrc.h"

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

// Bind the HR_C6000's chip description to a bus
void hrc6000_attach(hrc_t *dev, const hrc_bus_t *bus);

// Power-on initialization, modem-hrc6000.md section 3 steps 1-14. The board has already taken PWD low and waited
// 10 ms. Leaves the chip in its DMR layer 2 receive set-up; call a hrc6000_set_mode_*() next
bool hrc6000_power_on_init(hrc_t *dev, uint8_t dmr_mic_gain);

// FM receive (modem-hrc6000.md 8). The audio feed-through is left as it was: see hrc6000_fm_audio()
bool hrc6000_set_mode_fm_rx(hrc_t *dev);
// FM transmit: two-point modulation of the microphone with the deviation for the band and bandwidth
bool hrc6000_set_mode_fm_tx(hrc_t *dev, const hrc6000_fm_tx_t *cfg);
// FM receive audio feed-through to the line out (4:0x36): on, or muted
bool hrc6000_fm_audio(hrc_t *dev, bool on);
// Reference oscillator offset (hardware-dm1701.md 12.4). fm_tx: the FM TX form (0x48 = 0)
bool hrc6000_set_ref_offset(hrc_t *dev, int8_t offset, bool fm_tx);
// RX audio gain, -31..+31 dB (4:0x37 = 0x80 + 0x40 if positive + the low 5 bits of the gain, modem-hrc6000.md 2)
bool hrc6000_set_rx_gain(hrc_t *dev, int8_t db);

// Pure: the FM TX deviation coefficients for a band and bandwidth
hrc6000_fm_dev_t hrc6000_fm_deviation(bool uhf, bool wide);

#endif
