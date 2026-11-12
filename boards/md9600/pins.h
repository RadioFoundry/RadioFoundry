// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// TYT MD-9600 / RT90 pin map (docs/spec/hardware-md9600.md section 2). The same on every board revision, except that
// on V5 PE11-PE14 are IF filter and route lines instead of the AK2365A bus and PE15's use is unknown. Nothing here has
// been checked on a radio yet.
#ifndef MD9600_PINS_H
#define MD9600_PINS_H

#include "platform.h"

// Analog: ADC1 scan (power_md9600.c), DAC
#define PIN_RSSI_V		GPIOA, GPIO_PIN_0	// ADC1 IN0, VHF RSSI
#define PIN_SUPPLY		GPIOA, GPIO_PIN_1	// ADC1 IN1, supply voltage
#define PIN_QT_IN		GPIOA, GPIO_PIN_2	// ADC IN2, sub-audio tone input
#define PIN_MIC_LEVEL		GPIOA, GPIO_PIN_3	// ADC1 IN3, mic / VOX level
#define PIN_TUNE_U_DAC		GPIOA, GPIO_PIN_4	// DAC1: UHF receiver front-end tuning
#define PIN_APC_TUNE_V_DAC	GPIOA, GPIO_PIN_5	// DAC2: PA power reference in TX, VHF front-end tuning in RX
#define PIN_MICKEY_ROW		GPIOA, GPIO_PIN_6	// ADC1 IN6, hand-mic keypad level P2
#define PIN_MICKEY_COL		GPIOA, GPIO_PIN_7	// ADC1 IN7, hand-mic keypad level P1
#define PIN_RSSI_U		GPIOB, GPIO_PIN_0	// ADC1 IN8, UHF RSSI
#define PIN_NOISE_U		GPIOB, GPIO_PIN_1	// ADC1 IN9, UHF noise (squelch) level
#define PIN_DISC_AF		GPIOC, GPIO_PIN_3	// ADC IN13, discriminator audio (tone detection)
#define PIN_NOISE_V		GPIOC, GPIO_PIN_5	// ADC1 IN15, VHF noise level
#define ADC_CH_RSSI_V		0
#define ADC_CH_SUPPLY		1
#define ADC_CH_MIC		3
#define ADC_CH_MICKEY_ROW	6
#define ADC_CH_MICKEY_COL	7
#define ADC_CH_RSSI_U		8
#define ADC_CH_NOISE_U		9
#define ADC_CH_NOISE_V		15

// RF supplies and switches (high = on unless noted)
#define PIN_C5_U		GPIOA, GPIO_PIN_8	// C5_U_SW, UHF 5 V (receiver side)
#define PIN_C5_V		GPIOC, GPIO_PIN_9	// C5_V_SW, VHF 5 V (receiver side)
#define PIN_R5_U		GPIOA, GPIO_PIN_13	// R5_U_SW, UHF receiver 5 V; also SWDIO
#define PIN_R5_V		GPIOA, GPIO_PIN_14	// R5_V_SW, VHF receiver 5 V; also SWCLK
#define PIN_T5_V		GPIOD, GPIO_PIN_5	// T5_V_SW, VHF transmitter 5 V
#define PIN_T5_U		GPIOD, GPIO_PIN_6	// T5_U_SW, UHF transmitter 5 V
#define PIN_APC_EN		GPIOC, GPIO_PIN_4	// RF_APC_SW, PA power loop
#define PIN_VCO_U		GPIOE, GPIO_PIN_8	// VCO_VCC_U_SW: high = RX VCO, low = TX VCO
#define PIN_VCO_V		GPIOE, GPIO_PIN_9	// VCO_VCC_V_SW: high = RX VCO, low = TX VCO
#define PIN_MOD_SW		GPIOB, GPIO_PIN_7	// U_V_MOD_SW: HR_C6000 MOD2 to the VHF (high) or UHF (low) VCO
#define PIN_POWER		GPIOD, GPIO_PIN_15	// Power_Control: RF and audio sections on

// Synthesizers: one bus, a chip select per band (low = selected)
#define PIN_PLL_CS_V		GPIOD, GPIO_PIN_8
#define PIN_PLL_CS_U		GPIOD, GPIO_PIN_9
#define PIN_PLL_DATA		GPIOD, GPIO_PIN_10
#define PIN_PLL_CLK		GPIOD, GPIO_PIN_11

// IF receivers. V1-V4: AK2365A bus, a chip select per band and a shared reset. V5: PE11 / PE12 VHF / UHF IF filter
// (high = 25 kHz), PE13 / PE14 VHF / UHF IF to the HR_C6000
#define PIN_IF_CS_V		GPIOE, GPIO_PIN_11
#define PIN_IF_CS_U		GPIOE, GPIO_PIN_12
#define PIN_IF_DATA		GPIOE, GPIO_PIN_13
#define PIN_IF_CLK		GPIOE, GPIO_PIN_14
#define PIN_IF_RST		GPIOE, GPIO_PIN_15	// low = reset; V5 use unknown: left an input there

// Audio
#define PIN_FM_SW		GPIOB, GPIO_PIN_2	// FM audio stages on
#define PIN_AF_MUTE		GPIOB, GPIO_PIN_6	// AF_SW: high = speaker amplifier muted
#define PIN_AF_BAND		GPIOC, GPIO_PIN_13	// U_V_AF_SW: receive audio from the VHF (high) or UHF (low) IF chip
#define PIN_FM_MUTE		GPIOE, GPIO_PIN_7	// FM_MUTE: high = FM audio to the volume control
#define PIN_MIC_GPS_POWER	GPIOA, GPIO_PIN_9	// hand-mic and GPS supply (also USART1 TX)
#define PIN_GPS_RX		GPIOA, GPIO_PIN_10	// USART1 RX, GPS NMEA

// Display backlight and the PWM outputs, TIM8 (AF3)
#define PIN_BACKLIGHT		GPIOC, GPIO_PIN_6	// TIM8 CH1: LCD and key backlight
#define PIN_TONE_PWM		GPIOC, GPIO_PIN_7	// TIM8 CH2: sub-audio tone
#define PIN_BEEP_PWM		GPIOC, GPIO_PIN_8	// TIM8 CH3: beep

// HR_C6000: configuration port bit-banged, voice port on SPI1 (PB3-PB5), audio on I2S3 (PA15, PC10-PC12)
#define PIN_C6000_CS		GPIOE, GPIO_PIN_2
#define PIN_C6000_CLK		GPIOE, GPIO_PIN_3
#define PIN_C6000_MISO		GPIOE, GPIO_PIN_4
#define PIN_C6000_MOSI		GPIOE, GPIO_PIN_5
#define PIN_C6000_PWD		GPIOE, GPIO_PIN_6	// high = powered down
#define PIN_C6000_VCS		GPIOD, GPIO_PIN_7	// voice port chip select
#define PIN_C6000_TS_INT	GPIOC, GPIO_PIN_0
#define PIN_C6000_SYS_INT	GPIOC, GPIO_PIN_1
#define PIN_C6000_RFTX_INT	GPIOC, GPIO_PIN_2

// SPI2 (AF5): the data flash and the LCD
#define PIN_FLASH_SCK		GPIOB, GPIO_PIN_13
#define PIN_FLASH_MISO		GPIOB, GPIO_PIN_14
#define PIN_FLASH_MOSI		GPIOB, GPIO_PIN_15
#define PIN_FLASH_CS		GPIOB, GPIO_PIN_12
#define PIN_LCD_RST		GPIOD, GPIO_PIN_12	// low = reset
#define PIN_LCD_A0		GPIOD, GPIO_PIN_13	// low = command
#define PIN_LCD_CS		GPIOD, GPIO_PIN_14

// Front panel: 4 x 3 key matrix (rows driven low one at a time, columns read with pull-ups), the channel encoder, PTT
#define PIN_KEY_K0		GPIOE, GPIO_PIN_0	// row
#define PIN_KEY_K1		GPIOE, GPIO_PIN_1	// row
#define PIN_KEY_K2		GPIOD, GPIO_PIN_0	// row
#define PIN_KEY_K3		GPIOD, GPIO_PIN_1	// row
#define PIN_KEY_K4		GPIOD, GPIO_PIN_2	// column
#define PIN_KEY_K5		GPIOD, GPIO_PIN_3	// column
#define PIN_KEY_K6		GPIOD, GPIO_PIN_4	// column
#define PIN_ENC_A		GPIOB, GPIO_PIN_10	// EXTI10, both edges
#define PIN_ENC_B		GPIOB, GPIO_PIN_11
#define PIN_PTT			GPIOE, GPIO_PIN_10	// hand mic, active low

// I2C1 (PB8 SCL, PB9 SDA, AF4): one device at 7-bit address 0x3D, not identified
#define PIN_I2C_SCL		GPIOB, GPIO_PIN_8
#define PIN_I2C_SDA		GPIOB, GPIO_PIN_9

// Helpers that take the two-argument pin macros above: PIN_WRITE(PIN_POWER, true)
#define PIN_PORT(...)		PIN_PORT2_(__VA_ARGS__)
#define PIN_MASK(...)		PIN_MASK2_(__VA_ARGS__)
#define PIN_PORT2_(port, pin)	(port)
#define PIN_MASK2_(port, pin)	(pin)
#define PIN_WRITE(p, level)	stm32_pin_write(PIN_PORT(p), PIN_MASK(p), (level))
#define PIN_READ(p)		stm32_pin_read(PIN_PORT(p), PIN_MASK(p))
#define PIN_OUT(p, level)	stm32_pin_out(PIN_PORT(p), PIN_MASK(p), (level), false)

#endif
