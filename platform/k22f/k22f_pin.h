// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Kinetis pins as one byte (port * 32 + pin, port 0..4 = A..E) and a pin table entry that says how a board wants each
// pin: function, direction, level at start-up, pull, drive, interrupt edge. Pure: the host tests include it.
#ifndef K22F_PIN_H
#define K22F_PIN_H

#include <stdint.h>
#include <stdbool.h>

enum { K22F_PORT_A, K22F_PORT_B, K22F_PORT_C, K22F_PORT_D, K22F_PORT_E, K22F_PORT_COUNT };

#define K22F_PIN(port, n)	((uint8_t)((uint32_t)(port) * 32u + (uint32_t)(n)))
#define K22F_PIN_NONE		0xFFu

static inline uint8_t k22f_pin_port(uint8_t pin) { return (uint8_t)(pin >> 5); }
static inline uint8_t k22f_pin_num(uint8_t pin) { return (uint8_t)(pin & 31u); }
static inline uint32_t k22f_pin_bit(uint8_t pin) { return 1u << (pin & 31u); }
static inline bool k22f_pin_valid(uint8_t pin) { return pin < K22F_PORT_COUNT * 32u; }

// PORTx_PCRn fields (reference manual 11.5.1)
#define PORT_PCR_PS		(1u << 0)	// pull select: 1 = up
#define PORT_PCR_PE		(1u << 1)	// pull enable
#define PORT_PCR_SRE		(1u << 2)	// slow slew rate
#define PORT_PCR_PFE		(1u << 4)	// passive input filter
#define PORT_PCR_ODE		(1u << 5)	// open drain
#define PORT_PCR_DSE		(1u << 6)	// high drive strength
#define PORT_PCR_MUX(x)		((uint32_t)(x) << 8)	// 0 analog, 1 GPIO, 2..7 the pin's alternatives
#define PORT_PCR_MUX_MASK	(7u << 8)
#define PORT_PCR_IRQC(x)	((uint32_t)(x) << 16)
#define PORT_PCR_IRQC_MASK	(0xFu << 16)
#define PORT_PCR_ISF		(1u << 24)	// interrupt status, write 1 to clear
#define PORT_IRQC_NONE		0x0u
#define PORT_IRQC_RISING	0x9u
#define PORT_IRQC_FALLING	0xAu
#define PORT_IRQC_EITHER	0xBu

// Pin table flags (k22f_pin_cfg_t.flags)
#define K22F_MUX(x)		((uint32_t)(x) & 7u)	// the PCR MUX value
#define K22F_MUX_ANALOG		K22F_MUX(0)
#define K22F_MUX_GPIO		K22F_MUX(1)
#define K22F_OUT		(1u << 3)	// GPIO output (else input)
#define K22F_HIGH		(1u << 4)	// output level at start-up
#define K22F_PULLUP		(1u << 5)
#define K22F_PULLDOWN		(1u << 6)
#define K22F_OPEN_DRAIN		(1u << 7)
#define K22F_DRIVE_HIGH		(1u << 8)
#define K22F_FILTER		(1u << 9)
#define K22F_IRQ(irqc)		(((uint32_t)(irqc) & 0xFu) << 12)	// PORT_IRQC_* value

typedef struct {
	uint8_t pin;		// K22F_PIN()
	uint16_t flags;		// K22F_MUX_* | K22F_OUT | ...
} k22f_pin_cfg_t;

static inline uint32_t k22f_pin_mux_of(uint32_t flags) { return flags & 7u; }
static inline bool k22f_pin_is_output(uint32_t flags) { return (flags & K22F_OUT) != 0 && k22f_pin_mux_of(flags) == 1u; }

// The PCR value for a pin table entry. A pull-up wins over a pull-down when both are set
static inline uint32_t k22f_pcr_value(uint32_t flags)
{
	uint32_t pcr = PORT_PCR_MUX(k22f_pin_mux_of(flags));
	if (flags & K22F_PULLUP) {
		pcr |= PORT_PCR_PE | PORT_PCR_PS;
	} else if (flags & K22F_PULLDOWN) {
		pcr |= PORT_PCR_PE;
	}
	if (flags & K22F_OPEN_DRAIN) {
		pcr |= PORT_PCR_ODE;
	}
	if (flags & K22F_DRIVE_HIGH) {
		pcr |= PORT_PCR_DSE;
	}
	if (flags & K22F_FILTER) {
		pcr |= PORT_PCR_PFE;
	}
	pcr |= PORT_PCR_IRQC((flags >> 12) & 0xFu);
	return pcr;
}

#endif
