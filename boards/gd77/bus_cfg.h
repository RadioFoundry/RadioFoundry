// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// GD-77 bus implementations (docs/spec/hardware-gd77.md 4.1): for each bus, software (bit-banged on GPIO) or the
// K22F's hardware peripheral, and what the hardware choice would need on those pins. The chip glue sees every bus
// through the generic interfaces (bus/i2c_bus.h, bus/spi_bus.h), so changing an entry here changes no chip code.
//
// Bring-up runs every bus the way the radio's wiring was designed to be driven: the HR_C6000 port on SPI0 with its chip
// select by GPIO, everything else in software. An entry can be switched at build time (for example
// MCU_DEFS += -DGD77_I2C_A_HW=1); gd77_bus_cfg_check() refuses a set of choices the pins can't carry. The pin mux
// facts below are unverified until a radio is on the bench. Pure: tests/host checks the table.
#ifndef GD77_BUS_CFG_H
#define GD77_BUS_CFG_H

#include <stdint.h>
#include <stdbool.h>

#ifndef GD77_I2C_A_HW
#define GD77_I2C_A_HW		0
#endif
#ifndef GD77_I2C_B_HW
#define GD77_I2C_B_HW		0
#endif
#ifndef GD77_C6K_HW_CS
#define GD77_C6K_HW_CS		0
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

typedef struct {
	bool hw;		// true = the peripheral below, false = software
	bool hw_cs;		// SPI: the chip select as the peripheral's PCS instead of a GPIO
	gd77_periph_t periph;	// the peripheral these pins can carry (unverified)
	uint8_t mux;		// the pins' ALT setting for it
	bool cs_mux;		// SPI: the chip select pin also carries the peripheral's PCS
} gd77_bus_cfg_t;

__attribute__((unused)) static const gd77_bus_cfg_t gd77_bus_cfg[GD77_BUS_COUNT] = {
	// I2C0 is on both two-wire buses' pins (ALT5 here, ALT2 on PTB2 / PTB3): only one of them can have it
	[GD77_BUS_I2C_A] = { .hw = GD77_I2C_A_HW, .periph = GD77_PERIPH_I2C0, .mux = 5 },
	[GD77_BUS_I2C_B] = { .hw = GD77_I2C_B_HW, .periph = GD77_PERIPH_I2C0, .mux = 2 },
	// PTE4..PTE6 carry only SPI1 chip selects, not its clock and data, and PTA19 is XTAL0: software only
	[GD77_BUS_NOR] = { .periph = GD77_PERIPH_NONE },
	// No SPI function on PTC8..PTC12: software only
	[GD77_BUS_LCD] = { .periph = GD77_PERIPH_NONE },
	// SPI0 on ALT2; PTD0 is SPI0_PCS0 on the same ALT, so the chip select could move to the peripheral
	[GD77_BUS_C6K] = { .hw = true, .hw_cs = GD77_C6K_HW_CS, .periph = GD77_PERIPH_SPI0, .mux = 2, .cs_mux = true },
};

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
		for (uint32_t j = i + 1; j < n; j++) {
			if (cfg[i].hw && cfg[j].hw && cfg[i].periph == cfg[j].periph) {
				return "one peripheral chosen for two buses";
			}
		}
	}
	return 0;
}

#endif
