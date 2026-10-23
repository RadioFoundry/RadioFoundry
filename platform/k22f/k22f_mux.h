// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The K22F pin alternates the platform's boards may choose (PORTx_PCRn MUX values), as one table: a board's
// configuration check (boards/gd77/bus_cfg.h) refuses a peripheral on a pin unless the pin and its ALT number are
// listed here. Only the alternates ODR uses or offers are in it. Rows marked unverified haven't been checked against
// the data sheet's signal multiplexing table or on a radio; the others are in use on a board already
// (docs/spec/hardware-gd77.md 4.2). Pure: the host tests include it.
#ifndef K22F_MUX_H
#define K22F_MUX_H

#include <stdint.h>
#include <stdbool.h>
#include "k22f_pin.h"

typedef enum {
	K22F_FN_NONE,
	K22F_FN_I2C0_SCL, K22F_FN_I2C0_SDA,
	K22F_FN_SPI0_PCS0, K22F_FN_SPI0_SCK, K22F_FN_SPI0_SOUT, K22F_FN_SPI0_SIN,
	K22F_FN_SPI1_PCS0, K22F_FN_SPI1_SCK, K22F_FN_SPI1_SOUT, K22F_FN_SPI1_SIN,
	K22F_FN_I2S0_TXD0, K22F_FN_I2S0_RX_BCLK, K22F_FN_I2S0_RXD0, K22F_FN_I2S0_RX_FS,
	K22F_FN_FTM0_CH3, K22F_FN_FTM3_CH3,
} k22f_fn_t;

typedef struct {
	uint8_t pin;		// K22F_PIN()
	uint8_t alt;		// PCR MUX value
	uint8_t fn;		// k22f_fn_t
	bool verified;		// in use on a radio's pins already
} k22f_mux_t;

__attribute__((unused)) static const k22f_mux_t k22f_mux_table[] = {
	{ K22F_PIN(K22F_PORT_D, 1), 2, K22F_FN_SPI0_SCK, true },
	{ K22F_PIN(K22F_PORT_D, 2), 2, K22F_FN_SPI0_SOUT, true },
	{ K22F_PIN(K22F_PORT_D, 3), 2, K22F_FN_SPI0_SIN, true },
	{ K22F_PIN(K22F_PORT_B, 10), 2, K22F_FN_SPI1_PCS0, true },
	{ K22F_PIN(K22F_PORT_B, 11), 2, K22F_FN_SPI1_SCK, true },
	{ K22F_PIN(K22F_PORT_B, 16), 2, K22F_FN_SPI1_SOUT, true },
	{ K22F_PIN(K22F_PORT_B, 17), 2, K22F_FN_SPI1_SIN, true },
	{ K22F_PIN(K22F_PORT_A, 12), 6, K22F_FN_I2S0_TXD0, true },
	{ K22F_PIN(K22F_PORT_A, 14), 6, K22F_FN_I2S0_RX_BCLK, true },
	{ K22F_PIN(K22F_PORT_A, 15), 6, K22F_FN_I2S0_RXD0, true },
	{ K22F_PIN(K22F_PORT_A, 16), 6, K22F_FN_I2S0_RX_FS, true },
	// Unverified alternates, the hardware options of hardware-gd77.md 4.2
	{ K22F_PIN(K22F_PORT_D, 0), 2, K22F_FN_SPI0_PCS0, false },
	{ K22F_PIN(K22F_PORT_E, 24), 5, K22F_FN_I2C0_SCL, false },
	{ K22F_PIN(K22F_PORT_E, 25), 5, K22F_FN_I2C0_SDA, false },
	{ K22F_PIN(K22F_PORT_B, 2), 2, K22F_FN_I2C0_SCL, false },
	{ K22F_PIN(K22F_PORT_B, 3), 2, K22F_FN_I2C0_SDA, false },
	{ K22F_PIN(K22F_PORT_C, 4), 4, K22F_FN_FTM0_CH3, false },
};

#define K22F_MUX_COUNT	(sizeof(k22f_mux_table) / sizeof(k22f_mux_table[0]))

// The table's entry for fn on pin, NULL when the pin can't carry it
static inline const k22f_mux_t *k22f_mux_find(uint8_t pin, uint8_t fn)
{
	for (unsigned i = 0; i < K22F_MUX_COUNT; i++) {
		if (k22f_mux_table[i].pin == pin && k22f_mux_table[i].fn == fn) {
			return &k22f_mux_table[i];
		}
	}
	return 0;
}

// fn on pin at exactly this ALT number
static inline bool k22f_mux_allowed(uint8_t pin, uint8_t alt, uint8_t fn)
{
	const k22f_mux_t *m = k22f_mux_find(pin, fn);
	return m != 0 && m->alt == alt;
}

#endif
