// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Baofeng DM-1701 pin map (docs/spec/hardware-dm1701.md section 2)
#ifndef DM1701_PINS_H
#define DM1701_PINS_H

#include "platform.h"

// Analog
#define PIN_VOLUME		GPIOA, GPIO_PIN_0	// ADC1_IN0
#define PIN_BATTERY		GPIOA, GPIO_PIN_1	// ADC1_IN1
#define PIN_MIC_LEVEL		GPIOA, GPIO_PIN_3	// ADC1_IN3
#define PIN_APC_DAC		GPIOA, GPIO_PIN_4	// DAC_OUT1, PA power reference

// RF path
#define PIN_RX_UHF_5V		GPIOA, GPIO_PIN_2
#define PIN_RX_VHF_5V		GPIOA, GPIO_PIN_5
#define PIN_PA_EN1		GPIOC, GPIO_PIN_5	// TX supply
#define PIN_PA_EN2		GPIOC, GPIO_PIN_4	// APC loop enable
#define PIN_PA_BAND		GPIOC, GPIO_PIN_6	// low VHF, high UHF

// Power, audio, LEDs
#define PIN_POWER_LATCH		GPIOA, GPIO_PIN_7
#define PIN_MIC_POWER		GPIOA, GPIO_PIN_13	// SWDIO: driving it needs connect-under-reset for SWD
#define PIN_GPS_POWER		GPIOA, GPIO_PIN_9
#define PIN_SPK_MUTE		GPIOB, GPIO_PIN_8	// high = muted
#define PIN_AMP_EN		GPIOB, GPIO_PIN_9	// high = on
#define PIN_RX_AUDIO_MUX	GPIOD, GPIO_PIN_9	// high = HR_C6000 line out to the amp
#define PIN_LED_GREEN		GPIOE, GPIO_PIN_0
#define PIN_LED_RED		GPIOE, GPIO_PIN_1
#define PIN_CTCSS_PWM		GPIOC, GPIO_PIN_7	// unused, idles high (open drain)
#define PIN_BEEP_PWM		GPIOC, GPIO_PIN_8	// unused, idles low

// AT1846S on I2C3
#define PIN_I2C3_SCL		GPIOA, GPIO_PIN_8	// AF4 open drain
#define PIN_I2C3_SDA		GPIOC, GPIO_PIN_9	// AF4 open drain
#define AT1846S_I2C_ADDR	0x2E

// HR_C6000
#define PIN_C6000_CS		GPIOE, GPIO_PIN_2
#define PIN_C6000_CLK		GPIOE, GPIO_PIN_3
#define PIN_C6000_MOSI		GPIOE, GPIO_PIN_4
#define PIN_C6000_MISO		GPIOE, GPIO_PIN_5
#define PIN_C6000_PWD		GPIOE, GPIO_PIN_6	// high = powered down
#define PIN_C6000_VCS		GPIOB, GPIO_PIN_12	// voice SPI2 chip select
#define PIN_C6000_TS_INT	GPIOC, GPIO_PIN_0
#define PIN_C6000_SYS_INT	GPIOC, GPIO_PIN_1
#define PIN_C6000_RFTX_INT	GPIOC, GPIO_PIN_2

// SPI flash on SPI1 (AF5). The STM32F4 stub's board record reads these four (tools/boards.py F4_SPI_PINS)
#define PIN_FLASH_SCK		GPIOB, GPIO_PIN_3
#define PIN_FLASH_MISO		GPIOB, GPIO_PIN_4
#define PIN_FLASH_MOSI		GPIOB, GPIO_PIN_5
#define PIN_FLASH_CS		GPIOD, GPIO_PIN_7

// LCD on FSMC bank 1 (data lines double as keypad columns)
#define PIN_LCD_CS		GPIOD, GPIO_PIN_6
#define PIN_LCD_RST		GPIOD, GPIO_PIN_13
#define PIN_LCD_BACKLIGHT	GPIOD, GPIO_PIN_8
#define LCD_CMD_ADDR		((volatile uint8_t *)0x60000000)
#define LCD_DATA_ADDR		((volatile uint8_t *)0x60040000)

// Keypad rows (driven high one at a time); the columns are the LCD data lines D0..D7 (platform.h STM32_FSMC_*)
#define PIN_KEY_ROW0		GPIOA, GPIO_PIN_6
#define PIN_KEY_ROW1		GPIOD, GPIO_PIN_2
#define PIN_KEY_ROW2		GPIOD, GPIO_PIN_3

// PTT and encoder
#define PIN_PTT			GPIOE, GPIO_PIN_11	// active low
#define PIN_PTT_EXT		GPIOE, GPIO_PIN_12	// active low
#define PIN_ENC_A		GPIOE, GPIO_PIN_14
#define PIN_ENC_B		GPIOB, GPIO_PIN_11	// EXTI11, both edges

// Helpers that take the two-argument pin macros above: PIN_WRITE(PIN_LED_RED, true)
#define PIN_PORT(...)		PIN_PORT2_(__VA_ARGS__)
#define PIN_MASK(...)		PIN_MASK2_(__VA_ARGS__)
#define PIN_PORT2_(port, pin)	(port)
#define PIN_MASK2_(port, pin)	(pin)
#define PIN_WRITE(p, level)	stm32_pin_write(PIN_PORT(p), PIN_MASK(p), (level))
#define PIN_READ(p)		stm32_pin_read(PIN_PORT(p), PIN_MASK(p))

#endif
