// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// GD-88 SoC A pin map by GPIO pin index (0-31 bank A, 32-63 B, 64-95 C; docs/spec/hardware-gd88.md 9). The RF chips'
// bus and the NAND's chip select are the same on both SoCs (common/gd88_rf.h)
#ifndef GD88_PINS_H
#define GD88_PINS_H

#define PIN_SPARE_SCL		0x05	// PTA5 / PTA6: a second bit-banged pair, unused
#define PIN_SPARE_SDA		0x06
#define PIN_I2C_SCL		0x07	// bit-banged I2C: the IMU (0x68) and the FM broadcast receiver (RDA5807 class, 0x11)
#define PIN_I2C_SDA		0x08
#define PIN_PTT			0x09	// UART1 RXD; low = PTT (the K1 jack's sleeve carries both)
// PTA11 / PTA12 are UART2 (GPS), PTA13 / PTA14 UART3 (the link to B), PTA15 PWM0 (LCD backlight), PTA16 PWM1 (key
// backlight)
// TODO(gd88): unknown, needs a radio: PTA30's role. Low from start-up on, as at the key set-up
#define PIN_PARK_A30		0x1E

#define PIN_CHAIN_CLK		0x20	// PTB0: the 64-bit serial output chain (chain_gd88.c)
#define PIN_CHAIN_DATA		0x21	// PTB1
#define PIN_TX_RX		0x22	// PTB2: high = transmit (A's TX / RX switch line), low at start-up
#define PIN_CHAIN_LATCH		0x23	// PTB3, idle high
#define PIN_ENC_A		0x25	// PTB5
#define PIN_ENC_B		0x28	// PTB8
#define PIN_KEY_SENSE0		0x29	// PTB9-PTB11, low = closed
#define PIN_POWER_KEY		0x2C	// PTB12, high = pressed
#define PIN_POWER_LATCH		0x2D	// PTB13, high = stay on
#define PIN_GPS_POWER		0x31	// PTB17
#define GPS_POWER_ON_LEVEL	true
#define PIN_KEY_ROW0		0x32	// PTB18-PTB23, idle low
#define PIN_RF_STATUS0		0x3A	// PTB26 / PTB27: RF chip status lines, inputs
#define PIN_RF_STATUS1		0x3B

#define PIN_LCD_RESET		0x42	// PTC2, low = reset
#define PIN_SPK_AMP		0x52	// PTC18, high = speaker amplifier on
// PTC20: low cuts the modem's audio line (taken low while a beep plays over DMR audio). Low at start-up
#define PIN_DIG_AUDIO		0x54

#define KEY_ROWS		6
#define KEY_SENSES		3

// The status LEDs are on SoC B (PTA16, PTB17): A switches them over the link (RX2_LED)

#endif
