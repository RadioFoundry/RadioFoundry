// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// GD-77 pin map (docs/spec/hardware-gd77.md 3). Nothing here has been checked on a radio yet: every role is unverified,
// and the ones marked "candidate" are the likeliest reading of a pin whose role is still open (hardware-gd77.md 11).
#ifndef GD77_PINS_H
#define GD77_PINS_H

#include "k22f_pin.h"


// Power
#define PIN_POWER_HOLD		K22F_PIN(K22F_PORT_E, 26)	// high = stay on
#define PIN_POWER_SWITCH	K22F_PIN(K22F_PORT_A, 13)	// power knob switch, high = on

// Display: UC1701 / ST7567-class 128x64 on a write-only bit-banged serial bus
#define PIN_LCD_CS		K22F_PIN(K22F_PORT_C, 8)		// low = selected
#define PIN_LCD_RESET		K22F_PIN(K22F_PORT_C, 9)		// low = reset
#define PIN_LCD_A0		K22F_PIN(K22F_PORT_C, 10)	// low = command, high = data
#define PIN_LCD_SCK		K22F_PIN(K22F_PORT_C, 11)	// data taken on the rising edge
#define PIN_LCD_SDA		K22F_PIN(K22F_PORT_C, 12)	// MSB first
#define PIN_BACKLIGHT		K22F_PIN(K22F_PORT_C, 4)		// high = on (no PWM: plain GPIO)

// Keys: a 4 x 5 matrix (columns driven low one at a time, rows read with pull-ups) and four direct inputs
#define PIN_KEY_COL0		K22F_PIN(K22F_PORT_C, 0)		// PTC0..PTC3
#define PIN_KEY_ROW0		K22F_PIN(K22F_PORT_B, 19)	// PTB19..PTB23
#define KEY_COLS		4
#define KEY_ROWS		5
#define PIN_KEY_B9		K22F_PIN(K22F_PORT_B, 9)		// side key candidate (with PTB1: the side key pair)
#define PIN_KEY_B1		K22F_PIN(K22F_PORT_B, 1)		// side key candidate
#define PIN_KEY_A2		K22F_PIN(K22F_PORT_A, 2)		// top key candidate
#define PIN_KEY_A1		K22F_PIN(K22F_PORT_A, 1)		// PTT candidate (no internal pull)

// Data flash: SPI NOR, bit-banged, mode 0
#define PIN_NOR_CS		K22F_PIN(K22F_PORT_A, 19)	// low = selected; released to input (external pull-up) to deselect
#define PIN_NOR_MOSI		K22F_PIN(K22F_PORT_E, 4)
#define PIN_NOR_SCK		K22F_PIN(K22F_PORT_E, 5)
#define PIN_NOR_MISO		K22F_PIN(K22F_PORT_E, 6)

// Bit-banged I2C bus A: the AT1846S (7-bit 0x71) and the EEPROM (7-bit 0x50)
#define PIN_I2CA_SCL		K22F_PIN(K22F_PORT_E, 24)
#define PIN_I2CA_SDA		K22F_PIN(K22F_PORT_E, 25)
// Bit-banged I2C bus B: one device at 7-bit 0x3D, no radio function known
#define PIN_I2CB_SCL		K22F_PIN(K22F_PORT_B, 2)
#define PIN_I2CB_SDA		K22F_PIN(K22F_PORT_B, 3)

// HR_C6000 configuration port (U_SPI) on SPI0, chip select by GPIO
#define PIN_C6K_CS		K22F_PIN(K22F_PORT_D, 0)		// low = selected
#define PIN_C6K_SCK		K22F_PIN(K22F_PORT_D, 1)		// SPI0 SCK, ALT2
#define PIN_C6K_SOUT		K22F_PIN(K22F_PORT_D, 2)		// SPI0 SOUT, ALT2
#define PIN_C6K_SIN		K22F_PIN(K22F_PORT_D, 3)		// SPI0 SIN, ALT2
#define PIN_C6K_E0		K22F_PIN(K22F_PORT_E, 0)		// high at start-up; reset candidate (released high)
#define PIN_C6K_PWD		K22F_PIN(K22F_PORT_E, 1)		// power-down candidate: high 10 ms, then low = running
// HR_C6000 interrupt outputs (falling edge), roles by candidate
#define PIN_C6K_RF_RX		K22F_PIN(K22F_PORT_C, 7)
#define PIN_C6K_RF_TX		K22F_PIN(K22F_PORT_C, 16)
#define PIN_C6K_SYS		K22F_PIN(K22F_PORT_C, 17)
#define PIN_C6K_SLOT		K22F_PIN(K22F_PORT_C, 18)	// either edge
// HR_C6000 vocoder port (V_SPI) on SPI1
#define PIN_VSPI_PCS0		K22F_PIN(K22F_PORT_B, 10)	// ALT2
#define PIN_VSPI_SCK		K22F_PIN(K22F_PORT_B, 11)
#define PIN_VSPI_SOUT		K22F_PIN(K22F_PORT_B, 16)
#define PIN_VSPI_SIN		K22F_PIN(K22F_PORT_B, 17)
// I2S0 to the HR_C6000's codec, ALT6; the codec is the clock master
#define PIN_I2S_TXD0		K22F_PIN(K22F_PORT_A, 12)
#define PIN_I2S_RX_BCLK		K22F_PIN(K22F_PORT_A, 14)
#define PIN_I2S_RXD0		K22F_PIN(K22F_PORT_A, 15)
#define PIN_I2S_RX_FS		K22F_PIN(K22F_PORT_A, 16)

// Outputs whose roles are open: all low (or inactive) at start-up so nothing keys up
#define PIN_AUDIO_EN		K22F_PIN(K22F_PORT_B, 0)		// on while tones play: speaker amplifier candidate
#define PIN_TX_EN		K22F_PIN(K22F_PORT_A, 17)	// set at TX start, cleared at RX: PA / TX enable candidate
#define PIN_RF_SW1		K22F_PIN(K22F_PORT_E, 2)		// RF path switch candidates
#define PIN_RF_SW2		K22F_PIN(K22F_PORT_E, 3)
#define PIN_RF_SW3		K22F_PIN(K22F_PORT_C, 15)	// with PTC13: a switched pair
#define PIN_RF_SW4		K22F_PIN(K22F_PORT_C, 13)
#define PIN_LED_A		K22F_PIN(K22F_PORT_C, 14)	// LED candidates: blinked by indicator patterns
#define PIN_LED_B		K22F_PIN(K22F_PORT_B, 18)
#define PIN_IND_C5		K22F_PIN(K22F_PORT_C, 5)		// indicator pair candidate, active low
#define PIN_IND_C6		K22F_PIN(K22F_PORT_C, 6)		// indicator pair candidate, active high

// Analog (dedicated pins): ADC0 differential-pair inputs read single-ended
#define ADC_CH_BATTERY		1u	// ADC0_DP1: battery candidate
#define ADC_CH_DP2		2u	// ADC0_DP2: role open
#define ADC_CH_DP3		3u	// ADC0_DP3: role open (a level compared with thresholds)
// DAC0_OUT: TX power drive candidate (ramped at TX start and stop); left at 0

#endif
