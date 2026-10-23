// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The external SPI NOR flash's bus for the K22F stub: GPIO pins from the board record, mode 0, bit-banged with ODR's
// own software master (drivers/bus/spi_gpio). The chip's commands are shared with the other stubs
// (stub/common/stubnor.c). The pins are set up from scratch (GPIO function, directions), since ODR may have left the
// bus in any state; every other pin keeps the level ODR left. A chip select that idles released (on some boards a pin
// that is also a crystal input) is only ever driven low.
#include "k22f.h"
#include "k22stub_spi.h"
#include "spi_gpio.h"
#include "stubnor.h"

static k22_board_t bus;

static void gpio_mode(uint8_t pin)
{
	SIM_SCGC5 |= SIM_SCGC5_PORTA << k22f_pin_port(pin);
	uint32_t pcr = PORT_PCR(k22f_pin_port(pin), k22f_pin_num(pin));
	PORT_PCR(k22f_pin_port(pin), k22f_pin_num(pin)) = (pcr & ~(PORT_PCR_MUX_MASK | PORT_PCR_IRQC_MASK)) | PORT_PCR_MUX(1);
}

static void level(uint8_t pin, bool high)
{
	if (high) {
		GPIO_PSOR(k22f_pin_port(pin)) = k22f_pin_bit(pin);
	} else {
		GPIO_PCOR(k22f_pin_port(pin)) = k22f_pin_bit(pin);
	}
}

static void output(uint8_t pin, bool on)
{
	if (on) {
		GPIO_PDDR(k22f_pin_port(pin)) |= k22f_pin_bit(pin);
	} else {
		GPIO_PDDR(k22f_pin_port(pin)) &= ~k22f_pin_bit(pin);
	}
}

static void sck(bool high) { level(bus.sck, high); }
static void mosi(bool high) { level(bus.mosi, high); }
static bool miso(void) { return (GPIO_PDIR(k22f_pin_port(bus.miso)) & k22f_pin_bit(bus.miso)) != 0; }

static const spi_gpio_t soft = { .sck = sck, .mosi = mosi, .miso = miso, .cpol = false, .cpha = false };

void nor_hw_select(bool on)
{
	if (bus.cs_released) {
		level(bus.cs, false);
		output(bus.cs, on);	// low while selected, else released to the pull-up
	} else {
		level(bus.cs, !on);
	}
}

bool nor_hw_xfer(const uint8_t *tx, uint8_t *rx, uint32_t n)
{
	spi_gpio_xfer(&soft, tx, rx, n);
	return true;
}

uint32_t k22spi_init(const k22_board_t *b, uint8_t id[3])
{
	id[0] = id[1] = id[2] = 0;
	if (b->xflash == 0) {
		return 0;
	}
	bus = *b;
	// The chip deselected before the clock line moves
	gpio_mode(b->cs);
	nor_hw_select(false);
	if (!b->cs_released) {
		output(b->cs, true);
	}
	gpio_mode(b->sck);
	gpio_mode(b->mosi);
	gpio_mode(b->miso);
	level(b->sck, false);
	output(b->sck, true);
	output(b->mosi, true);
	output(b->miso, false);
	spi_gpio_idle(&soft);
	return stubnor_probe(id);
}
