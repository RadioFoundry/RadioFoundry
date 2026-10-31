// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// AnyTone AT-D578UV pin map, both MCU builds (docs/spec/hardware-d578uv.md 3). Each pin is a port and a bit mask
#ifndef D578UV_PINS_H
#define D578UV_PINS_H

#include "mcu_io.h"

// LCD, 8080 bus: data on PE7-PE14 (bit 0 on PE7), control on PB10-PB13, RD held high
#define LCD_DATA_PORT		GPIOE
#define LCD_DATA_SHIFT		7u
#define LCD_DATA_MASK		((uint16_t)(0xFFu << LCD_DATA_SHIFT))
#define LCD_WR_PORT		GPIOB
#define LCD_WR_BIT		MCU_PIN(10)
#define PIN_LCD_WR		LCD_WR_PORT, LCD_WR_BIT
#define PIN_LCD_RS		GPIOB, MCU_PIN(11)	// D/C: high = data
#define PIN_LCD_RESET		GPIOB, MCU_PIN(12)
#define PIN_LCD_CS		GPIOB, MCU_PIN(13)
#define PIN_LCD_RD		GPIOE, MCU_PIN(15)

// SPI NAND, bit-banged (the stock firmware drives these pins as GPIOs)
#define PIN_NAND_CS		GPIOB, MCU_PIN(8)
#define PIN_NAND_CLK		GPIOB, MCU_PIN(14)
#define PIN_NAND_MOSI		GPIOB, MCU_PIN(15)
#define PIN_NAND_MISO		GPIOA, MCU_PIN(15)

// The four RF chips' 3-wire bus: shared clock and data, one enable each (low = selected)
#define PIN_RF_SCLK		GPIOB, MCU_PIN(3)
#define PIN_RF_SDIO		GPIOB, MCU_PIN(4)
#define PIN_RF_CS0		GPIOB, MCU_PIN(5)
#define PIN_RF_CS1		GPIOB, MCU_PIN(2)
#define PIN_RF_CS2		GPIOC, MCU_PIN(13)
#define PIN_RF_CS3		GPIOE, MCU_PIN(1)
// The serial device on the same clock and data lines (4-bit address, 8-bit data), its enable low = selected. Its
// register 8 is the backlight level (radio_d578uv.c)
#define PIN_AUX_CS		GPIOA, MCU_PIN(5)

// SCT3258 host port interface: data PD8-PD15, strobes and handshake on PD2-PD6
#define HPI_DATA_PORT		GPIOD
#define HPI_DATA_SHIFT		8u
#define PIN_HPI_CS		GPIOD, MCU_PIN(2)
#define PIN_HPI_WR		GPIOD, MCU_PIN(3)
#define PIN_HPI_RD		GPIOD, MCU_PIN(4)
#define PIN_HPI_HOBI		GPIOD, MCU_PIN(5)	// input: the modem's handshake
#define PIN_HPI_RESET		GPIOD, MCU_PIN(6)	// RSTN
#define PIN_HPI_PD1		GPIOD, MCU_PIN(1)	// low in the reset sequence
#define PIN_HPI_PD7		GPIOD, MCU_PIN(7)	// low in the reset sequence
#define PIN_SCT_ENABLE		GPIOA, MCU_PIN(8)	// the modem's supply, high = on
#define PIN_SCT_PC6		GPIOC, MCU_PIN(6)	// high in the reset sequence
#define PIN_SCT_PC7		GPIOC, MCU_PIN(7)	// low in the reset sequence
#define PIN_SCT_PIO1		GPIOC, MCU_PIN(12)	// boot strap, low in the reset sequence

// The KTMicro AM/FM receiver on a bit-banged I2C bus (hardware-d578uv.md 5.4). The stock firmware switches PB6 between
// output and input with pull-up: PB6 is the data line
#define PIN_KT_SDA		GPIOB, MCU_PIN(6)
#define PIN_KT_SCL		GPIOB, MCU_PIN(7)

// Serial ports: GPS (USART2, 9600), Bluetooth (UART4, 115200)
#define PIN_GPS_TX		GPIOA, MCU_PIN(2)
#define PIN_GPS_RX		GPIOA, MCU_PIN(3)
#define PIN_BT_TX		GPIOC, MCU_PIN(10)
#define PIN_BT_RX		GPIOC, MCU_PIN(11)
#define BT_TX_AF		8u	// PC10 as UART4 TX (the AT32's multiplexed function; the GD32 has one per pin)
// The hand microphone. A plain microphone pulls these two lines low (pull-ups; front_io.h): PA10 alone is PTT. The
// serial keypad microphone uses the same pins as the USART the manuals call USART1 (the GD32's USART0): PA9 the radio's
// TX, PA10 its RX, 115200 (input_d578uv.c)
#define MIC_USART		MCU_USART1
#define MIC_USART_IRQn		MCU_USART1_IRQn
#define MIC_USART_HANDLER	MCU_USART1_HANDLER
#define MIC_TX_AF		7u	// PA9 as USART1 TX (the AT32's multiplexed function; the GD32 has one per pin)
#define PIN_MIC_LINE9		GPIOA, MCU_PIN(9)
#define PIN_MIC_LINE10		GPIOA, MCU_PIN(10)

// Power: the hold output (high = stay on) and the power key (low = pressed)
#define PIN_POWER_LATCH		GPIOC, MCU_PIN(3)
#define PIN_POWER_KEY		GPIOA, MCU_PIN(0)

// Keys: two resistor ladders on the ADC (four keys each), one key on its own pin, the encoder. PTT is the microphone's
// (above)
#define PIN_KEY_LADDER0		GPIOB, MCU_PIN(0)
#define ADC_KEY_LADDER0		8u
#define PIN_KEY_LADDER1		GPIOB, MCU_PIN(1)
#define ADC_KEY_LADDER1		9u
#define PIN_KEY_SINGLE		GPIOA, MCU_PIN(1)	// low = pressed
#define PIN_DMR_TX_BURST	GPIOD, MCU_PIN(0)	// high while the baseband sends a burst (EXTI 0; inferred source)
#define PIN_ENC_A		GPIOC, MCU_PIN(8)
#define PIN_ENC_B		GPIOC, MCU_PIN(9)

// Analog inputs: supply voltage (an 11:1 divider), the two volume pots (A and B channel), the ignition sense
#define PIN_SUPPLY_SENSE	GPIOC, MCU_PIN(1)
#define ADC_SUPPLY_SENSE	11u
#define PIN_VOL_A		GPIOC, MCU_PIN(2)
#define ADC_VOL_A		12u
#define PIN_VOL_B		GPIOC, MCU_PIN(5)
#define ADC_VOL_B		15u
#define PIN_IGNITION_SENSE	GPIOA, MCU_PIN(6)
#define ADC_IGNITION_SENSE	6u
// Demodulated receive audio, biased at mid-scale (hardware-d578uv.md 3.4): APRS receive samples it (afsk_d578uv.c)
#define PIN_RX_AUDIO		GPIOC, MCU_PIN(0)
#define ADC_RX_AUDIO		10u

// The audio and RF switches behind an 8-bit shift register: data, clock, latch
#define PIN_SR_DATA		GPIOE, MCU_PIN(3)
#define PIN_SR_CLK		GPIOE, MCU_PIN(4)
#define PIN_SR_LATCH		GPIOE, MCU_PIN(5)
#define PIN_SPK_MUTE		GPIOA, MCU_PIN(7)	// high = speaker muted
#define PIN_TX_ENABLE		GPIOE, MCU_PIN(6)	// high while transmitting

// The PA drive (DAC0 on PA4) and the tone timer output (PB9)
#define PIN_PA_DAC		GPIOA, MCU_PIN(4)
#define PIN_TONE		GPIOB, MCU_PIN(9)	// beeper, TIM4 CH4 PWM

// The two-wire level device on the SWD pins (A / B channel volume, inferred); the SWD port is off while it runs
#define PIN_LEVEL_DATA		GPIOA, MCU_PIN(13)
#define PIN_LEVEL_CLK		GPIOA, MCU_PIN(14)

#endif
