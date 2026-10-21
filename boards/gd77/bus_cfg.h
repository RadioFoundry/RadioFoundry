// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// GD-77 bus implementations (docs/spec/hardware-gd77.md 4.1): for each bus, software (bit-banged on GPIO) or the
// K22F's hardware peripheral, and what the hardware choice would need on those pins. The chip glue sees every bus
// through the generic interfaces (bus/i2c_bus.h, bus/spi_bus.h), so changing an entry here changes no chip code.
//
// Bring-up runs every bus the way the radio's wiring was designed to be driven: the HR_C6000 port on SPI0 with its chip
// select by GPIO, everything else in software. An entry can be switched at build time (for example
// GD77_DEFS="-DGD77_I2C_A_HW=1", board.mk); gd77_bus_cfg_check() refuses a set of choices the pins can't carry: each
// pin a hardware choice puts on a peripheral must be in the K22F alternate table (platform/k22f/k22f_mux.h) at the
// ALT number given here. The backlight's PWM option is checked the same way. The pin mux facts are unverified until a
// radio is on the bench. Pure: tests/host checks the table.
#ifndef GD77_BUS_CFG_H
#define GD77_BUS_CFG_H

#include <stdint.h>
#include <stdbool.h>
#include "pins.h"
#include "k22f_mux.h"

#ifndef GD77_I2C_A_HW
#define GD77_I2C_A_HW		0
#endif
#ifndef GD77_I2C_B_HW
#define GD77_I2C_B_HW		0
#endif
#ifndef GD77_C6K_HW_CS
#define GD77_C6K_HW_CS		0
#endif
// The backlight on PTC4 dimmed by an FTM channel instead of switched on and off (display_gd77.c)
#ifndef GD77_BACKLIGHT_PWM
#define GD77_BACKLIGHT_PWM	0
#endif

typedef enum {
	GD77_BUS_I2C_A,		// PTE24 / PTE25: AT1846S, EEPROM
	GD77_BUS_I2C_B,		// PTB2 / PTB3: the device at 0x3D
	GD77_BUS_NOR,		// PTE4 / PTE5 / PTE6, chip select PTA19: SPI NOR flash
	GD77_BUS_LCD,		// PTC8..PTC12: the display
	GD77_BUS_C6K,		// PTD0..PTD3: HR_C6000 configuration port
	GD77_BUS_COUNT
} gd77_bus_id_t;

typedef enum {
	GD77_PERIPH_NONE,	// no peripheral carries this bus on these pins: software only
	GD77_PERIPH_I2C0,
	GD77_PERIPH_SPI0,
} gd77_periph_t;

#define GD77_BUS_PINS	4

typedef struct {
	bool hw;		// true = the peripheral below, false = software
	bool hw_cs;		// SPI: the chip select as the peripheral's PCS instead of a GPIO
	gd77_periph_t periph;	// the peripheral these pins can carry (unverified)
	uint8_t mux;		// the pins' ALT setting for it
	bool cs_mux;		// SPI: the chip select pin also carries the peripheral's PCS
	// The pins the peripheral takes over and the function each must carry (k22f_fn_t); the chip select last, only
	// taken with hw_cs. K22F_FN_NONE ends the list
	struct {
		uint8_t pin, fn;
	} pins[GD77_BUS_PINS];
} gd77_bus_cfg_t;

__attribute__((unused)) static const gd77_bus_cfg_t gd77_bus_cfg[GD77_BUS_COUNT] = {
	// I2C0 is on both two-wire buses' pins (ALT5 here, ALT2 on PTB2 / PTB3): only one of them can have it
	[GD77_BUS_I2C_A] = { .hw = GD77_I2C_A_HW, .periph = GD77_PERIPH_I2C0, .mux = 5,
			     .pins = { { PIN_I2CA_SCL, K22F_FN_I2C0_SCL }, { PIN_I2CA_SDA, K22F_FN_I2C0_SDA } } },
	[GD77_BUS_I2C_B] = { .hw = GD77_I2C_B_HW, .periph = GD77_PERIPH_I2C0, .mux = 2,
			     .pins = { { PIN_I2CB_SCL, K22F_FN_I2C0_SCL }, { PIN_I2CB_SDA, K22F_FN_I2C0_SDA } } },
	// PTE4..PTE6 carry only SPI1 chip selects, not its clock and data, and PTA19 is XTAL0: software only
	[GD77_BUS_NOR] = { .periph = GD77_PERIPH_NONE },
	// No SPI function on PTC8..PTC12: software only
	[GD77_BUS_LCD] = { .periph = GD77_PERIPH_NONE },
	// SPI0 on ALT2; PTD0 is SPI0_PCS0 on the same ALT, so the chip select could move to the peripheral
	[GD77_BUS_C6K] = { .hw = true, .hw_cs = GD77_C6K_HW_CS, .periph = GD77_PERIPH_SPI0, .mux = 2, .cs_mux = true,
			   .pins = { { PIN_C6K_SCK, K22F_FN_SPI0_SCK }, { PIN_C6K_SOUT, K22F_FN_SPI0_SOUT },
				     { PIN_C6K_SIN, K22F_FN_SPI0_SIN }, { PIN_C6K_CS, K22F_FN_SPI0_PCS0 } } },
};

// The backlight's PWM option: the FTM channel on PIN_BACKLIGHT, from the alternate table (unverified)
#define GD77_BACKLIGHT_FN	K22F_FN_FTM0_CH3
#define GD77_BACKLIGHT_FTM	0u
#define GD77_BACKLIGHT_CH	3u
#define GD77_BACKLIGHT_HZ	1000u	// well above flicker, low enough for the panel's LED driver (unverified)

// The pins of one hardware choice are on the alternate table at its ALT number
static inline bool gd77_bus_pins_ok(const gd77_bus_cfg_t *c)
{
	for (uint32_t k = 0; k < GD77_BUS_PINS && c->pins[k].fn != K22F_FN_NONE; k++) {
		bool cs = c->cs_mux && (k + 1u == GD77_BUS_PINS || c->pins[k + 1u].fn == K22F_FN_NONE);
		if (cs && !c->hw_cs) {
			continue;	// the chip select stays a GPIO
		}
		if (!k22f_mux_allowed(c->pins[k].pin, c->mux, c->pins[k].fn)) {
			return false;
		}
	}
	return true;
}

// 0 when every hardware choice is one the pins carry and no peripheral is chosen twice, else what is wrong
static inline const char *gd77_bus_cfg_check(const gd77_bus_cfg_t *cfg, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++) {
		if (cfg[i].hw && cfg[i].periph == GD77_PERIPH_NONE) {
			return "hardware chosen for a bus whose pins carry no peripheral";
		}
		if (cfg[i].hw_cs && (!cfg[i].hw || !cfg[i].cs_mux)) {
			return "hardware chip select without the peripheral on that pin";
		}
		if (cfg[i].hw && !gd77_bus_pins_ok(&cfg[i])) {
			return "a pin of a hardware choice doesn't carry that function at that ALT number";
		}
		for (uint32_t j = i + 1; j < n; j++) {
			if (cfg[i].hw && cfg[j].hw && cfg[i].periph == cfg[j].periph) {
				return "one peripheral chosen for two buses";
			}
		}
	}
	return 0;
}

// The backlight choice: PWM only on a pin that carries the FTM channel
static inline const char *gd77_backlight_cfg_check(bool pwm)
{
	return (!pwm || k22f_mux_find(PIN_BACKLIGHT, GD77_BACKLIGHT_FN) != 0) ? 0 : "no FTM channel on the backlight pin";
}

#endif
