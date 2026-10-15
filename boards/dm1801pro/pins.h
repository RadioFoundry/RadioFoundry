// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// DM-1801 Pro pin map by GPIO pin index (0-31 bank A, 32-63 B, 64-95 C; docs/spec/hardware-dm1801pro.md 3). Pins marked
// provisional are inferred and not yet confirmed on a board
#ifndef DM1801PRO_PINS_H
#define DM1801PRO_PINS_H

#define PIN_RF_CLK		0x07	// FD6818 serial clock (bit-banged)
#define PIN_RF_DATA		0x08	// FD6818 serial data, bidirectional
#define PIN_LCD_BACKLIGHT	0x0F	// high = on
#define PIN_AMP_EN		0x10	// PTA16: audio power amplifier, high = on (checked on the radio)
#define PIN_BCAST_SDA		0x19	// broadcast FM tuner I2C data (RDA5807-class, address 0x11)
#define PIN_BCAST_SCL		0x1A	// broadcast FM tuner I2C clock
#define PIN_POWER_LATCH		0x11	// 1 = stay on
#define PIN_FLASH_CS		0x12	// data flash chip select, low = selected
#define PIN_RX_VHF		0x1C	// RX path select, VHF side (provisional band)
#define PIN_BCAST_EN		0x1D	// broadcast FM tuner enable, low = on (provisional)
#define PIN_RF_CS		0x1E	// FD6818 chip select, low = selected
#define PIN_TX_VHF		0x1F	// TX path select, VHF side (provisional band)

#define PIN_ACCEL_SCL		0x22	// accelerometer I2C clock (PTB2)
#define PIN_ACCEL_SDA		0x37	// accelerometer I2C data (PTB23)
#define PIN_LED_GREEN		0x20	// PTB0; colors checked on the radio
#define PIN_LED_RED		0x21	// PTB1
#define PIN_KEY_ROW0		0x23	// keypad inputs PTB3-PTB6, low = key closed
#define PIN_KEY_ROW3		0x26
#define PIN_TX_UHF		0x27	// TX path select, UHF side (provisional band)
#define PIN_RX_UHF		0x28	// RX path select, UHF side (provisional band)
#define PIN_POWER_KEY		0x29	// power knob switch: the level at start-up is "on" (switches off on the radio)
#define PIN_MIC_PATH		0x2E	// 1 = mic to the analog FM path (as on the DM-32UV, provisional)
#define PIN_SPK_SOURCE		0x2F	// 0 = audio on, 1 = muted (checked on the radio)
#define PIN_KEY_STROBE0		0x32	// keypad strobes PTB18-PTB22, driven low one at a time
#define PIN_KEY_STROBE4		0x36
#define PIN_VIBRATION		0x38	// PTB24 vibration motor, high = on (checked on the radio)

// The four direct key inputs, low = pressed (checked on the radio, hardware-dm1801pro.md 9.1)
#define PIN_KEY_D0		0x2B	// PTB11 PTT
#define PIN_KEY_D1		0x2C	// PTB12 SOS (top key)
#define PIN_KEY_D2		0x3A	// PTB26 SK1 (upper side key; with PTT at power-on: update mode)
#define PIN_KEY_D3		0x3B	// PTB27 SK2 (lower side key)

#define PIN_LCD_RESET		0x42	// low = reset
#define PIN_LCD_CS		0x43	// low = selected
#define PIN_LCD_CLK		0x44	// idle high, a bit is taken on the rising edge
#define PIN_LCD_DC		0x46	// low = command
#define PIN_LCD_DATA		0x48	// MSB first

#define KEY_STROBES		5
#define KEY_INPUTS		4

#endif
