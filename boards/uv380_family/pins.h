// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// UV380 family pin map (docs/spec/hardware-dm1701.md section 2), the same on every radio of the family but the MD-UV390
// Plus 10 W, which swaps PC6 and PD8 (hardware-mduv380.md 3: the mduv380 board's POWER=10w build), and the MD-2017
// (UVF_PINS_MD2017, hardware-md2017.md 2): its own RF bus, receiver switches, backlight and navigation ring, and a second
// receiver. Each board's own pins.h includes this one
#ifndef UVF_PINS_H
#define UVF_PINS_H

#include "platform.h"

// Analog
#define PIN_VOLUME		GPIOA, GPIO_PIN_0	// ADC1_IN0
#define PIN_BATTERY		GPIOA, GPIO_PIN_1	// ADC1_IN1
#define PIN_MIC_LEVEL		GPIOA, GPIO_PIN_3	// ADC1_IN3
#define PIN_APC_DAC		GPIOA, GPIO_PIN_4	// DAC_OUT1, PA power reference

// RF path
#if UVF_PINS_MD2017
#define PIN_RX_UHF_5V		GPIOC, GPIO_PIN_9	// the main receiver's
#define PIN_RX_VHF_5V		GPIOA, GPIO_PIN_5
#define PIN_RX2_UHF_5V		GPIOA, GPIO_PIN_8	// the second receiver's, kept off
#define PIN_RX2_VHF_5V		GPIOA, GPIO_PIN_2
#else
#define PIN_RX_UHF_5V		GPIOA, GPIO_PIN_2
#define PIN_RX_VHF_5V		GPIOA, GPIO_PIN_5
#endif
#define PIN_PA_EN1		GPIOC, GPIO_PIN_5	// TX supply
#define PIN_PA_EN2		GPIOC, GPIO_PIN_4	// APC loop enable
#if defined(RFOUNDRY_POWER_10W) && RFOUNDRY_POWER_10W
#define PIN_PA_BAND		GPIOD, GPIO_PIN_8	// low VHF, high UHF (10 W radio)
#else
#define PIN_PA_BAND		GPIOC, GPIO_PIN_6	// low VHF, high UHF (MD-2017: the other way round)
#endif
// PIN_PA_BAND's level for UHF
#if UVF_PINS_MD2017
#define PA_BAND_UHF		false
#else
#define PA_BAND_UHF		true
#endif

// Power, audio, LEDs
#define PIN_POWER_LATCH		GPIOA, GPIO_PIN_7
#if !UVF_PINS_MD2017	// the MD-2017 has no microphone power switch: PA13 is an input there
#define PIN_MIC_POWER		GPIOA, GPIO_PIN_13	// SWDIO: driving it needs connect-under-reset for SWD
#endif
#define PIN_GPS_POWER		GPIOA, GPIO_PIN_9	// high = on (hardware-mduv380.md 9, hardware-dm1701.md 14.2)
#define PIN_GPS_RX		GPIOA, GPIO_PIN_10	// USART1 RX (AF7), the module's NMEA output
#define PIN_SPK_MUTE		GPIOB, GPIO_PIN_8	// high = muted
#define PIN_AMP_EN		GPIOB, GPIO_PIN_9	// high = on
#define PIN_RX_AUDIO_MUX	GPIOD, GPIO_PIN_9	// high = HR_C6000 line out to the amp
#if UVF_PINS_MD2017
#define PIN_RX2_AUDIO_MUX	GPIOD, GPIO_PIN_8	// the second receiver's gate; low with the main one on (hardware-md2017.md 2)
#endif
#define PIN_LED_GREEN		GPIOE, GPIO_PIN_0
#define PIN_LED_RED		GPIOE, GPIO_PIN_1
#define PIN_CTCSS_PWM		GPIOC, GPIO_PIN_7	// unused, idles high (open drain)
#define PIN_BEEP_PWM		GPIOC, GPIO_PIN_8	// unused, idles low

// The AT1846S on I2C3, the MD-2017's two on I2C1 (the addresses: uvf_variant.rf_addr)
#if UVF_PINS_MD2017
#define UVF_RF_I2C		I2C1
#define UVF_RF_I2C_AF		GPIO_AF4_I2C1
#define UVF_RF_I2C_CLK_ENABLE()	__HAL_RCC_I2C1_CLK_ENABLE()
#define PIN_RF_I2C_SCL		GPIOB, GPIO_PIN_6	// AF4 open drain
#define PIN_RF_I2C_SDA		GPIOB, GPIO_PIN_7	// AF4 open drain
#else
#define UVF_RF_I2C		I2C3
#define UVF_RF_I2C_AF		GPIO_AF4_I2C3
#define UVF_RF_I2C_CLK_ENABLE()	__HAL_RCC_I2C3_CLK_ENABLE()
#define PIN_RF_I2C_SCL		GPIOA, GPIO_PIN_8	// AF4 open drain
#define PIN_RF_I2C_SDA		GPIOC, GPIO_PIN_9	// AF4 open drain
#endif

// HR_C6000: the configuration port on port E, the MD-2017's clock on PC13
#define PIN_C6000_CS		GPIOE, GPIO_PIN_2
#if UVF_PINS_MD2017
#define PIN_C6000_CLK		GPIOC, GPIO_PIN_13
#define PIN_C6000_MOSI		GPIOE, GPIO_PIN_5
#define PIN_C6000_MISO		GPIOE, GPIO_PIN_3
#else
#define PIN_C6000_CLK		GPIOE, GPIO_PIN_3
#define PIN_C6000_MOSI		GPIOE, GPIO_PIN_4
#define PIN_C6000_MISO		GPIOE, GPIO_PIN_5
#endif
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
#if defined(RFOUNDRY_POWER_10W) && RFOUNDRY_POWER_10W
#define PIN_LCD_BACKLIGHT	GPIOC, GPIO_PIN_6	// the 10 W radio
#elif UVF_PINS_MD2017
#define PIN_LCD_BACKLIGHT	GPIOB, GPIO_PIN_2
#else
#define PIN_LCD_BACKLIGHT	GPIOD, GPIO_PIN_8
#endif
#define LCD_CMD_ADDR		((volatile uint8_t *)0x60000000)
#define LCD_DATA_ADDR		((volatile uint8_t *)0x60040000)

// Keypad rows (driven high one at a time); the columns are the LCD data lines D0..D7 (platform.h STM32_FSMC_*)
#define PIN_KEY_ROW0		GPIOA, GPIO_PIN_6
#define PIN_KEY_ROW1		GPIOD, GPIO_PIN_2
#define PIN_KEY_ROW2		GPIOD, GPIO_PIN_3

// PTT, and the encoder or the MD-2017's top key and navigation ring
#define PIN_PTT			GPIOE, GPIO_PIN_11	// active low
#define PIN_PTT_EXT		GPIOE, GPIO_PIN_12	// active low
#if UVF_PINS_MD2017
#define PIN_KEY_TOP		GPIOE, GPIO_PIN_13	// the orange key, active low
// The ring: each line changes level once per step in its direction (input_uvf.c)
#define PIN_NAV_UP		GPIOB, GPIO_PIN_10
#define PIN_NAV_DOWN		GPIOB, GPIO_PIN_11
#define PIN_NAV_LEFT		GPIOE, GPIO_PIN_15
#define PIN_NAV_RIGHT		GPIOE, GPIO_PIN_14
#else
#define PIN_ENC_A		GPIOE, GPIO_PIN_14
#define PIN_ENC_B		GPIOB, GPIO_PIN_11	// EXTI11, both edges
#endif

// Helpers that take the two-argument pin macros above: PIN_WRITE(PIN_LED_RED, true)
#define PIN_PORT(...)		PIN_PORT2_(__VA_ARGS__)
#define PIN_MASK(...)		PIN_MASK2_(__VA_ARGS__)
#define PIN_PORT2_(port, pin)	(port)
#define PIN_MASK2_(port, pin)	(pin)
#define PIN_WRITE(p, level)	stm32_pin_write(PIN_PORT(p), PIN_MASK(p), (level))
#define PIN_READ(p)		stm32_pin_read(PIN_PORT(p), PIN_MASK(p))

#endif
