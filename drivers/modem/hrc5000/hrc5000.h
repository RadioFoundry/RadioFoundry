// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// HR_C5000 DMR / FM baseband chip driver (docs/spec/hardware-md380.md 4). Its register facts
// are the HR_C5000 user manual (datasheets/C5000/hrc5000.pdf) and the stock firmware's bring-up sequences; what neither
// confirms is marked (?) in the spec. Register access, the DMR slot engine and the DMR ID / modulation helpers are
// shared with the HR_C6000 (drivers/modem/hrc_common); nothing of the HR_C6000's own driver is used.
#ifndef HRC5000_H
#define HRC5000_H

#include "modem/hrc_common/hrc.h"

typedef enum { HRC5000_MODE_NONE, HRC5000_MODE_FM_RX, HRC5000_MODE_FM_TX, HRC5000_MODE_DMR } hrc5000_mode_t;

typedef struct {
	hrc_t hrc;		// first, so the shared code's hrc_t pointer is this chip's
	bool vhf;		// VHF board: receive I/Q order swapped (4:0x01 = 0xF0 where UHF has 0xB0)
	uint8_t mode;		// hrc5000_mode_t: what the chip was last put in
	uint8_t ref_tune;	// the reference oscillator tune last given to hrc5000_set_ref, re-applied by the full set-ups
	bool have_ref;
	bool spk_voice;		// a received call plays: the codec has the LINEOUT values (hrc5000_speaker_voice)
	bool i2s_on;		// the I2S clocks (CK, WS) wanted on: the full set-ups' 0x11 block turns them off, so they put it back
} hrc5000_t;

// The shared code's view of the chip: pass this to the DMR slot engine
static inline hrc_t *hrc5000_hrc(hrc5000_t *dev)
{
	return &dev->hrc;
}

void hrc5000_attach(hrc5000_t *dev, const hrc_bus_t *bus, bool vhf);

// Clock set-up, then FM receive. The board has already taken PWD low and waited for the chip to wake
bool hrc5000_power_on(hrc5000_t *dev);

// FM receive: the full FM set-up, or just the receive switch when coming from FM transmit
bool hrc5000_set_mode_fm_rx(hrc5000_t *dev);
// FM transmit with the calibrated two-point modulation pair (the "FM" pair of the calibration table)
bool hrc5000_set_mode_fm_tx(hrc5000_t *dev, bool wide, uint8_t mod2, uint8_t mod1);
// DMR: the full DMR set-up, left in receive. Also the shared engine's entry (hrc_chip_t dmr_enter)
bool hrc5000_set_mode_dmr(hrc5000_t *dev, uint8_t mod2, uint8_t mod1, uint8_t mic_gain);
// The small register set for a DMR RX <-> TX switch (never the full set-up, which resets the DMR state machine):
// RX: the codec and path registers; TX adds the modulation pair, deviation factor and limiter
bool hrc5000_dmr_path(hrc5000_t *dev, bool tx, uint8_t mod2, uint8_t mod1, uint8_t mic_gain);

// The chip's I2S clocks (BCLK, LRCK; the board's pins). The 0x11 block of every full set-up writes 0x36 = 0xF0, whose
// bit 7 turns the clocks off; on clears it. The board switches them on for the length of a PCM audio stream
bool hrc5000_i2s_clock(hrc5000_t *dev, bool on);
bool hrc5000_speaker_voice(hrc5000_t *dev, bool on);

// Reference oscillator from the factory tune byte (calibration offset 9): 4:0x47 = 4:0x04 = 0x7F - tune, 4:0x48 = 0
// below 0x80 else 3
bool hrc5000_set_ref(hrc5000_t *dev, uint8_t ref_tune);
// Pure: the reference oscillator bias DAC value for the tune byte (tune x 4 + 0x600; the board's MOD2_BIAS DAC)
uint16_t hrc5000_ref_dac(uint8_t ref_tune);

#endif
