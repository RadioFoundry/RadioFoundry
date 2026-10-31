// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// AT32F43x platform services for boards built on it
#ifndef PLATFORM_AT32F43X_H
#define PLATFORM_AT32F43X_H

#include <stdint.h>
#include <stdbool.h>
#include "at32f43x.h"
#include "hal_system.h"

extern uint32_t SystemCoreClock;

typedef struct {
	uint32_t hext_hz;	// the crystal
	bool hext_bypass;	// an external clock fed in, no crystal
	uint32_t sys_hz;	// the core clock the PLL makes from it
} at32_clock_t;

// HICK first (the bootloader may have left the PLL running), then HEXT and the PLL to sys_hz, with the regulator and
// the flash divider set for it and the automatic step on while the system clock switches. AHB at the core clock,
// APB1 and APB2 at half of it (the USB takes the 48 MHz HICK: usb.c). False when the PLL can't make sys_hz or a
// clock never became stable: the core then runs from whatever it reached (SystemCoreClock says what)
bool at32_clock_init(const at32_clock_t *c);
uint32_t at32_apb1_hz(void);
uint32_t at32_apb2_hz(void);

// System services for board.system (system.c): chip identity, memory reads, reboot
extern const hal_system_t at32_system;
// Runtime flash-stub entry (CONFIG_STUB_ENTRY, stub_entry.c; docs/spec/stub-at32f43x.md 3). No RAM is spared for the
// image: the core stages it in the frame buffer (hal_system_t.enter_stub_staged), and this copies it to the SRAM base
// and jumps; false (nothing changed) when the image or the stack doesn't allow it
bool at32_enter_stub_staged(const uint8_t *img, uint32_t len, uint32_t entry);
// With CONFIG_WATCHDOG and the watchdog running: true only for an image whose header (stub/at32f43x/at32stub_header.h)
// says the stub feeds it; without, any AT32F43x stub image
bool at32_stub_image_ok(const uint8_t *img, uint32_t len);
// CONFIG_WATCHDOG (watchdog.c): the watchdog can't be stopped, so before the stub entry it gets its longest timeout
// (about 26 s) and a feed. The stub feeds it from then on
void at32_watchdog_stretch(void);

typedef enum { AT32_PULL_NONE, AT32_PULL_UP, AT32_PULL_DOWN } at32_pull_t;

#define AT32_PIN(n)		((uint16_t)(1u << (n)))

void at32_gpio_clock(at32_gpio_t *port);
// For board_rf_safe: the port's clock on, the level, then push-pull output, with plain register writes. Safe in a
// fault handler and before the clocks are set up
void at32_pin_force(at32_gpio_t *port, uint16_t pins, bool level);
void at32_pin_out(at32_gpio_t *port, uint16_t pins, bool level, bool open_drain);
void at32_pin_in(at32_gpio_t *port, uint16_t pins, at32_pull_t pull);
// The multiplexed function af (0..15, the data sheet's pin table) on the pins
void at32_pin_af(at32_gpio_t *port, uint16_t pins, uint8_t af, bool open_drain);
void at32_pin_analog(at32_gpio_t *port, uint16_t pins);
// EXINT line `line` (0..15) from this port's pin of the same number
void at32_exint_route(at32_gpio_t *port, uint8_t line);

static inline void at32_pin_write(at32_gpio_t *port, uint16_t pins, bool level)
{
	port->SCR = level ? pins : ((uint32_t)pins << 16);
}

static inline bool at32_pin_read(at32_gpio_t *port, uint16_t pin)
{
	return (port->IDT & pin) != 0;
}

// ADC1 (adc.c): calibrate and power up once, then read a channel (0..18), 12 bits. Any task may read
void at32_adc_init(void);
uint16_t at32_adc_read(uint8_t ch);
// The sampling channel (adc.c): ch converted by the preempted group, started from an interrupt (mcu_io.h)
void at32_adc_sample_init(uint8_t ch);

// DAC (dac.c): channel 0 (DAC1) on PA4, 1 (DAC2) on PA5. Init makes the pin analog and starts the channel at 0; write
// takes 12 bits
void at32_dac_init(uint8_t ch);
void at32_dac_write(uint8_t ch, uint16_t value);

// A USART a byte at a time (uart.c): 8N1, receive and its interrupt on, transmit too when tx. The board sets the pins
// and calls at32_uart_take from its handler: the byte, AT32_UART_ERROR (overrun, framing, noise) or AT32_UART_NONE.
// put waits for room
#define AT32_UART_NONE		(-1)
#define AT32_UART_ERROR		(-2)
void at32_uart_start(at32_usart_t *u, uint32_t baud, bool tx);
void at32_uart_stop(at32_usart_t *u);
int32_t at32_uart_take(at32_usart_t *u);
void at32_uart_put(at32_usart_t *u, uint8_t c);

// USB identity (a board's board_usb_ids): the pair every RadioFoundry radio enumerates with (platform/stm32f4 has the
// reasons), so the host tools find this one too
#define AT32_USB_VID		0x1FC9
#define AT32_USB_PID_SERIAL	0x0094
#define AT32_USB_PID_NETWORK	0x0096
// usb_ids_t.serial_number: the chip's 96-bit unique ID in hex (usb.c, CONFIG_USB)
void at32_usb_serial_number(char *buf, uint8_t len);

#endif
