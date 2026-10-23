// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// DB25-D pin map by GPIO pin index (0-31 bank A, 32-63 B, 64-95 C; docs/spec/hardware-db25d.md 3)
#ifndef DB25D_PINS_H
#define DB25D_PINS_H

#define PIN_MIC_RXD		0x09	// UART1 RXD; the mic's PTT pulls it low
#define PIN_SPK_MIC		0x0F	// mic speaker, low = on. Also must stay high for the LCD backlight (hardware-db25d.md 3)
#define PIN_NAND_CS		0x12	// SPI NAND chip select, low = selected
#define PIN_BCAST_AUDIO		0x13	// FM broadcast audio enable
#define PIN_RX_SUPPLY		0x17	// high in RX, low in TX
#define PIN_SPK_BODY		0x18	// body speaker, low = on
#define PIN_UHF_RX		0x19
#define PIN_UHF_TX		0x1A
// PTA16 (0x10) is the LCD backlight, PWM1 (display_db25d.c)
#define PIN_PARK_A17		0x11	// unused, the stock firmware parks it high
#define PIN_TX_SUPPLY_A		0x1B	// low in RX, raised 200 us into TX

#define PIN_LED_GREEN		0x20	// status LEDs (3.3)
#define PIN_LED_RED		0x21
#define PIN_TX_SUPPLY_B		0x22
#define PIN_VHF_TX		0x23
#define PIN_VHF_RX		0x24
#define PIN_ENC_A		0x25
#define PIN_ENC_B		0x28
#define PIN_KEY_SENSE0		0x29	// key matrix sense lines 0x29-0x2B, low = closed
#define PIN_POWER_KEY		0x2C	// low = pressed (INF)
#define PIN_POWER_LATCH		0x2D	// 1 = stay on
#define PIN_MIC_PATH		0x2E	// as the other HR_C7000 boards: 1 = mic to the analog path (INF)
#define PIN_SPK_SOURCE		0x2F	// as the other HR_C7000 boards: 0 = audio on, 1 = muted (INF)
#define PIN_RF_CLK		0x30	// BK4819 serial clock (both chips)
#define PIN_GPS_POWER		0x31
// The level that powers the GPS module. The stock firmware clears PTB17 at GPS init and at power-off and sets it when GPS is
// switched on, so high = on (hardware-db25d.md 3.1; confirmed on the radio)
#define GPS_POWER_ON_LEVEL	true
#define PIN_RF_DATA		0x38	// BK4819 serial data, bidirectional
#define PIN_RF_SEN_MAIN		0x39	// main BK4819 enable, low = selected

#define PIN_LCD_RESET		0x42	// low = reset
#define PIN_LCD_CS		0x43
#define PIN_LCD_SCL		0x44
#define PIN_LCD_DC		0x46	// low = command
#define PIN_LCD_SDA		0x47
#define PIN_PTT2		0x4B	// hand mic PTT, low = pressed
#define PIN_AUDIO_SWITCH	0x52	// speaker source: 1 = the main BK4819's AF, 0 = the modem's line out (INF)
#define PIN_RX_FRONT_END	0x53	// on in both RX paths
#define PIN_RF_SEN_LO		0x54	// LO BK4819 enable, low = selected

// Key matrix: five drive lines driven low one at a time (0x32, then 0x34-0x37; 0x33 belongs to the Bluetooth option), three sense
#define PIN_KEY_DRIVE0		0x32
#define PIN_KEY_DRIVE1		0x34	// 0x34-0x37
#define KEY_DRIVES		5
#define KEY_SENSES		3

#endif
