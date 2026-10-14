// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// TYT MD-380 pin map (docs/spec/hardware-md380.md section 2). Nothing here has been checked on a radio: the sources
// are public schematic notes and pin tables, one of which warns that its schematic is wrong in places. Pins marked
// (?) disagree between sources or are guesses; the bring-up checklist (docs/testing/md380-bringup.md) confirms them.
#ifndef MD380_PINS_H
#define MD380_PINS_H

#include "platform.h"

// Analog (ADC1)
#define PIN_VOLUME		GPIOA, GPIO_PIN_0	// ADC IN0, volume knob position (?: one source has the TX LED here)
#define PIN_BATTERY		GPIOA, GPIO_PIN_1	// ADC IN1
#define PIN_QT_IN		GPIOA, GPIO_PIN_2	// ADC IN2, sub-audio tone input (stock decodes CTCSS / DCS from it)
#define PIN_MIC_LEVEL		GPIOA, GPIO_PIN_3	// ADC IN3, VOX
#define PIN_APC_DAC		GPIOA, GPIO_PIN_4	// DAC_OUT1: PA power reference in TX, front-end tuning voltage in RX
#define PIN_REF_DAC		GPIOA, GPIO_PIN_5	// DAC_OUT2: reference oscillator bias (MOD2_BIAS)
#define PIN_RSSI		GPIOB, GPIO_PIN_0	// ADC IN8, GT3136 RSSI
#define PIN_BUSY		GPIOB, GPIO_PIN_1	// ADC IN9, GT3136 noise squelch level
#define PIN_AF_ADC		GPIOC, GPIO_PIN_3	// ADC IN13, FM discriminator audio (unused)
#define ADC_CH_VOLUME		0
#define ADC_CH_BATTERY		1
#define ADC_CH_MIC		3
#define ADC_CH_RSSI		8
#define ADC_CH_BUSY		9

// RF path
#define PIN_VCO_SEL		GPIOA, GPIO_PIN_9	// VCOVCC_SW: RX or TX VCO supply
#define PIN_DMR_SW		GPIOA, GPIO_PIN_10	// high: receiver IF and audio to the HR_C5000 (low while transmitting)
#define PIN_WIDE		GPIOA, GPIO_PIN_13	// W/N_SW, IF bandwidth; SWDIO: driving it needs connect-under-reset
#define PIN_FM_SW		GPIOB, GPIO_PIN_2	// FM receive path select (polarity ?)
#define PIN_APC_EN		GPIOC, GPIO_PIN_4	// RF_APC_SW: PA power control loop on
#define PIN_TX_5V		GPIOC, GPIO_PIN_5	// 5TC: transmitter supply
#define PIN_RX_5V		GPIOC, GPIO_PIN_9	// 5RC: receiver supply
#define PIN_SAVE		GPIOA, GPIO_PIN_8	// "SAVE": high enables the 5C rail (U404 CE) that feeds the PLL

// SKY72310 synthesizer: three-wire bus with its own clock (PE4) and chip select (PD11); the data line is the
// HR_C5000's MOSI (PE5)
#define PIN_PLL_CS		GPIOD, GPIO_PIN_11
#define PIN_PLL_CLK		GPIOE, GPIO_PIN_4	// idles high, the chip latches on the rising edge
#define PIN_PLL_DATA		GPIOE, GPIO_PIN_5	// shared with PIN_C5000_MOSI
#define PIN_PLL_LD		GPIOD, GPIO_PIN_10	// SKY72310 PS, out-of-lock, open collector: pulled up, high = locked

// Power, audio, LEDs
#define PIN_POWER_LATCH		GPIOA, GPIO_PIN_7	// POW_C
#define PIN_MIC_POWER		GPIOA, GPIO_PIN_14	// MICPWR_SW; SWCLK: driving it breaks SWD
#define PIN_SPK_MUTE		GPIOB, GPIO_PIN_8	// SPK_C, high = muted (speaker anti-pop)
#define PIN_AMP_EN		GPIOB, GPIO_PIN_9	// AFCO, high = audio amplifier on
#define PIN_FM_MUTE		GPIOE, GPIO_PIN_13	// FM receive audio mute (polarity ?)
#define PIN_LED_GREEN		GPIOE, GPIO_PIN_0	// RX LED
#define PIN_LED_RED		GPIOE, GPIO_PIN_1	// TX LED
#define PIN_CTCSS_PWM		GPIOC, GPIO_PIN_7	// CTC/DCS_OUT (unused: no tone encoder yet)
#define PIN_BEEP_PWM		GPIOC, GPIO_PIN_8	// BEEP, TIM8_CH3 (unused: beeps go through the HR_C5000)
#define PIN_LCD_BACKLIGHT	GPIOC, GPIO_PIN_6	// LAMP, TIM8_CH1 PWM

// HR_C5000: bit-banged configuration port, voice port on SPI2, three interrupt lines (as the DM-1701, but MOSI and
// MISO are the other way round and MOSI also goes to the synthesizer)
#define PIN_C5000_CS		GPIOE, GPIO_PIN_2
#define PIN_C5000_CLK		GPIOC, GPIO_PIN_13	// idles low; the chip samples MOSI on the falling edge
#define PIN_C5000_MISO		GPIOE, GPIO_PIN_3	// chip SDO -> MCU
#define PIN_C5000_MOSI		GPIOE, GPIO_PIN_5	// MCU -> chip SDI, also the SKY72310 data
#define PIN_C5000_PWD		GPIOE, GPIO_PIN_6	// DMR_SLEEP, high = powered down
#define PIN_C5000_VCS		GPIOB, GPIO_PIN_12	// voice SPI2 chip select (SPI2 on PB13-PB15)
#define PIN_C5000_TS_INT	GPIOC, GPIO_PIN_0
#define PIN_C5000_SYS_INT	GPIOC, GPIO_PIN_1
#define PIN_C5000_RFTX_INT	GPIOC, GPIO_PIN_2

// SPI flash on SPI1 (PB3 SCK, PB4 MISO, PB5 MOSI, AF5)
#define PIN_FLASH_CS		GPIOD, GPIO_PIN_7

// GPS on the G models (?): module power on PD8 (a second flash chip select on radios without GPS, so it is only
// driven once the module has been heard), NMEA in on PD9 as USART3 RX (AF7)
#define PIN_GPS_POWER		GPIOD, GPIO_PIN_8
#define PIN_GPS_RX		GPIOD, GPIO_PIN_9

// LCD on FSMC bank 1 (data lines double as keypad columns), the same wiring as the DM-1701
#define PIN_LCD_CS		GPIOD, GPIO_PIN_6
#define PIN_LCD_RST		GPIOD, GPIO_PIN_13
#define LCD_CMD_ADDR		((volatile uint8_t *)0x60000000)
#define LCD_DATA_ADDR		((volatile uint8_t *)0x60040000)	// A17 (PD12) = RS

// Keypad rows (driven high one at a time) and columns (the LCD data lines D0..D7)
#define PIN_KEY_ROW0		GPIOA, GPIO_PIN_6	// K1
#define PIN_KEY_ROW1		GPIOD, GPIO_PIN_2	// K2
#define PIN_KEY_ROW2		GPIOD, GPIO_PIN_3	// K3 (side keys ?)
#define KEY_COLS_PORTD		(GPIO_PIN_14 | GPIO_PIN_15 | GPIO_PIN_0 | GPIO_PIN_1)	// D0 D1 D2 D3
#define KEY_COLS_PORTE		(GPIO_PIN_7 | GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_10)	// D4 D5 D6 D7

// PTT and the 16-position channel switch (four code bits, active low ?)
#define PIN_PTT			GPIOE, GPIO_PIN_11	// active low
#define PIN_PTT_EXT		GPIOE, GPIO_PIN_12	// active low
#define PIN_CH_B0		GPIOE, GPIO_PIN_14	// ECN0
#define PIN_CH_B1		GPIOE, GPIO_PIN_15	// ECN1
#define PIN_CH_B2		GPIOB, GPIO_PIN_10	// ECN2
#define PIN_CH_B3		GPIOB, GPIO_PIN_11	// ECN3

// Helpers that take the two-argument pin macros above: PIN_WRITE(PIN_LED_RED, true)
#define PIN_PORT(...)		PIN_PORT2_(__VA_ARGS__)
#define PIN_MASK(...)		PIN_MASK2_(__VA_ARGS__)
#define PIN_PORT2_(port, pin)	(port)
#define PIN_MASK2_(port, pin)	(pin)
#define PIN_WRITE(p, level)	stm32_pin_write(PIN_PORT(p), PIN_MASK(p), (level))
#define PIN_READ(p)		stm32_pin_read(PIN_PORT(p), PIN_MASK(p))

#endif
