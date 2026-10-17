// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// DM-32UV pin map by GPIO pin index (0-31 bank A, 32-63 B, 64-95 C; docs/spec/hardware-dm32.md 5)
#ifndef DM32_PINS_H
#define DM32_PINS_H

#define PIN_FD_CLK		0x07	// FD6818 serial clock (bit-banged)
#define PIN_FD_DATA		0x08	// FD6818 serial data, bidirectional
#define PIN_GPS_RXD		0x09	// UART1 RXD (peripheral)
#define PIN_LCD_BACKLIGHT	0x0F	// GPIO or PWM0, high = on
#define PIN_POWER_SWITCH	0x10	// 1 = knob turned off
#define PIN_POWER_LATCH		0x11	// 1 = stay on
#define PIN_FLASH_CS		0x12	// data flash chip select, low = selected
#define PIN_FD_CS		0x18	// FD6818 chip select, low = selected

// The two LEDs, high = on. Colors checked on the radio at bring-up (hardware-dm32.md 5)
#define PIN_LED_GREEN		0x20
#define PIN_LED_RED		0x21
#define PIN_VHF_LNA		0x22	// VHF RX supply, high = on
#define PIN_UHF_PA		0x23	// UHF PA select, high = on
#define PIN_UHF_LNA		0x24	// UHF RX supply, high = on
#define PIN_ENC_B		0x26
#define PIN_ENC_A		0x27
#define PIN_KEY_BACKLIGHT	0x28
#define PIN_PTT			0x2B	// low = pressed
#define PIN_GPS_POWER		0x2C
#define GPS_POWER_ON_LEVEL	false	// low = GPS on (hardware-dm32.md 12)
#define PIN_MIC_PATH		0x2E	// 1 = mic to the analog FM path, 0 = to the DMR codec
#define PIN_SPK_SOURCE		0x2F	// 1 = the FD6818's audio to the speaker, 0 = into the HR_C7000 (DMR receive)
#define PIN_KEY_COL1		0x31	// keypad columns 1-4 (0x31-0x34), low = key closed
#define PIN_KEY_COL4		0x34
#define PIN_AMP_EN		0x35	// audio power amplifier, high = on
#define PIN_VHF_PA		0x37	// VHF PA select, high = on
#define PIN_FD_GPIO1		0x3A	// FD6818 GPIO1 / GPIO0: function unknown, inputs, no interrupt
#define PIN_FD_GPIO0		0x3B
#define PIN_LCD_RESET		0x42	// low = reset
#define PIN_LCD_RD		0x46	// i8080 RD, held high as a GPIO
#define PIN_KEY_ROW0		0x47	// keypad rows 0-4 = LCD DB0-DB4 (0x47-0x4B)
#define PIN_LCD_DB6		0x4D

#define KEY_ROWS		5
#define KEY_COLS		4

#endif
