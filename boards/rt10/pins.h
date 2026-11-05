// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// RT10 pin map by GPIO pin index (0-31 bank A, 32-63 B, 64-95 C; docs/spec/hardware-rt10.md 3)
#ifndef RT10_PINS_H
#define RT10_PINS_H

#define PIN_FD_SCL		0x07	// FD6818 two-wire clock (bit-banged)
#define PIN_FD_SDA		0x08	// FD6818 two-wire data, bidirectional
#define PIN_FLASH_CS		0x12	// data flash chip select, low = selected

#define PIN_TX_SUPPLY		0x20	// TX supply, high = on
#define PIN_KNOB_B2		0x21	// channel knob, code bit 2 (bit 3: PIN_KNOB_B3)
#define PIN_KNOB_B1		0x22	// code bit 1
#define PIN_PA_SELECT		0x23	// PA select, high = on
#define PIN_RX_SUPPLY		0x24	// RX supply, high = on
#define PIN_LED_A		0x25	// LED, high = on
#define PIN_KNOB_B3		0x26	// code bit 3
#define PIN_LED_B		0x27	// LED, high = on (the one lit at power-up)
#define PIN_VOICE_EN		0x28	// voice prompt chip enable, high = on
#define PIN_SK1			0x29	// side key, low = pressed
#define PIN_SK2			0x2A	// side key, low = pressed
#define PIN_PTT			0x2B	// low = pressed; also the cable's serial line (shared_ptt)
#define PIN_POWER_LATCH		0x2C	// 1 = stay on
#define PIN_AMP_EN		0x2D	// audio amplifier, high = on
#define PIN_POWER_SWITCH	0x31	// power switch sense
#define PIN_KNOB_B0		0x35	// code bit 0
#define PIN_RF_SQ		0x3A	// PTB26, from the FD6818's GPIO6: its squelch output: high = open, gates PTB27
#define PIN_RF_CSS		0x3B	// PTB27, from the FD6818's GPIO0: the sub-audio compare result, high = heard
#define PIN_VOICE_CLK		0x3C	// voice prompt chip serial clock
#define PIN_VOICE_DATA		0x3D	// its data

#endif
