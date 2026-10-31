// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// GD32F30x platform services for boards built on it
#ifndef PLATFORM_GD32F30X_H
#define PLATFORM_GD32F30X_H

#include <stdint.h>
#include <stdbool.h>
#include "gd32f30x.h"
#include "hal_system.h"

extern uint32_t SystemCoreClock;

typedef struct {
	uint32_t hxtal_hz;	// the crystal
	bool hxtal_bypass;	// an external clock fed in, no crystal
	bool hxtal_div2;	// PREDV0: HXTAL / 2 into the PLL
	uint8_t pll_mul;	// 2..63
} gd32_clock_t;

// IRC8M first (the bootloader may have left the PLL running), then HXTAL and the PLL from the table, with the high
// driver mode above 108 MHz. APB1 runs at half the core clock, APB2 at the core clock, the USB at 48 MHz where the core
// clock allows it. False when a clock never became stable: the core then runs from whatever it reached (SystemCoreClock
// says what)
bool gd32_clock_init(const gd32_clock_t *c);
uint32_t gd32_apb1_hz(void);
uint32_t gd32_apb2_hz(void);

// System services for board.system (system.c): chip identity, memory reads, reboot
extern const hal_system_t gd32_system;

// Runtime flash-stub entry (CONFIG_STUB_ENTRY, stub_entry.c; docs/spec/stub-gd32f30x.md 3). No RAM is spared for the
// image: the core stages it in the frame buffer (hal_system_t.enter_stub_staged), and this copies it to the SRAM base
// and jumps. The transmitter goes off first (board_rf_safe); the power latch, display and USB pins stay
bool gd32_enter_stub_staged(const uint8_t *img, uint32_t len, uint32_t entry);
// With CONFIG_WATCHDOG and the watchdog running: true only for an image whose header (stub/gd32f30x/gd32stub_header.h)
// says the stub feeds it; without, any GD32F30x stub image
bool gd32_stub_image_ok(const uint8_t *img, uint32_t len);
// CONFIG_WATCHDOG (watchdog.c): the free watchdog can't be stopped, so before the stub entry it gets its longest
// timeout (about 26 s) and a feed. The stub feeds it from then on
void gd32_watchdog_stretch(void);

// Pin modes (the CTL nibble: MD in the low two bits, CTL in the high two)
#define GD32_PIN_ANALOG		0x0u
#define GD32_PIN_IN_FLOAT	0x4u
#define GD32_PIN_IN_PULL	0x8u
#define GD32_PIN_OUT_PP		0x3u	// 50 MHz
#define GD32_PIN_OUT_OD		0x7u
#define GD32_PIN_AF_PP		0xBu
#define GD32_PIN_AF_OD		0xFu

typedef enum { GD32_PULL_NONE, GD32_PULL_UP, GD32_PULL_DOWN } gd32_pull_t;

#define GD32_PIN(n)		((uint16_t)(1u << (n)))

void gd32_gpio_clock(gd32_gpio_t *port);
// For board_rf_safe: the port's clock on, the level, then push-pull output, with plain register writes. Safe in a
// fault handler and before the clocks are set up
void gd32_pin_force(gd32_gpio_t *port, uint16_t pins, bool level);
void gd32_pin_out(gd32_gpio_t *port, uint16_t pins, bool level, bool open_drain);
void gd32_pin_in(gd32_gpio_t *port, uint16_t pins, gd32_pull_t pull);
void gd32_pin_af(gd32_gpio_t *port, uint16_t pins, bool open_drain);
void gd32_pin_analog(gd32_gpio_t *port, uint16_t pins);
// The SWD and JTAG pins (AFIO_PCF0_SWJ_*): AFIO_PCF0_SWJ_SWDP frees PA15, PB3 and PB4, AFIO_PCF0_SWJ_OFF PA13 and PA14
// too (a debugger then has to connect under reset)
void gd32_swj(uint32_t mode);
// EXTI line `line` (0..15) from this port's pin of the same number
void gd32_exti_route(gd32_gpio_t *port, uint8_t line);

static inline void gd32_pin_write(gd32_gpio_t *port, uint16_t pins, bool level)
{
	port->BOP = level ? pins : ((uint32_t)pins << 16);
}

static inline bool gd32_pin_read(gd32_gpio_t *port, uint16_t pin)
{
	return (port->ISTAT & pin) != 0;
}

// ADC0 (adc.c): calibrate and power up once, then read a channel (0..17), 12 bits. Any task may read
void gd32_adc_init(void);
uint16_t gd32_adc_read(uint8_t ch);
// The sampling channel (adc.c): ch converted by the inserted group, started from an interrupt (mcu_io.h)
void gd32_adc_sample_init(uint8_t ch);

// DAC (dac.c): channel 0 on PA4, 1 on PA5. Init makes the pin analog and starts the channel at 0; write takes 12 bits
void gd32_dac_init(uint8_t ch);
void gd32_dac_write(uint8_t ch, uint16_t value);

// A USART a byte at a time (uart.c): 8N1, receive and its interrupt on, transmit too when tx. The board sets the pins
// and calls gd32_uart_take from its handler: the byte, GD32_UART_ERROR (overrun, framing, noise) or GD32_UART_NONE.
// put waits for room
#define GD32_UART_NONE		(-1)
#define GD32_UART_ERROR		(-2)
void gd32_uart_start(gd32_usart_t *u, uint32_t baud, bool tx);
void gd32_uart_stop(gd32_usart_t *u);
int32_t gd32_uart_take(gd32_usart_t *u);
void gd32_uart_put(gd32_usart_t *u, uint8_t c);

// USB identity (a board's board_usb_ids): the pair every RadioFoundry radio enumerates with (platform/stm32f4 has the
// reasons), so the host tools find this one too
#define GD32_USB_VID		0x1FC9
#define GD32_USB_PID_SERIAL	0x0094
#define GD32_USB_PID_NETWORK	0x0096
// usb_ids_t.serial_number: the chip's 96-bit unique ID in hex (usb.c, CONFIG_USB)
void gd32_usb_serial_number(char *buf, uint8_t len);
// The board's D+ pull-up, if it switches it: on (true) to be seen by the host. Weak default does nothing (a fixed
// resistor)
void board_usb_pullup(bool on);

#endif
