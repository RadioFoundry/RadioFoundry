// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// SPI master on GPIO pins (bit-banged), MSB first, any of the four modes: for chips wired to plain pins (an SPI NOR
// flash or an LCD, as in docs/spec/hardware-gd77.md 4). The chip select stays with the caller, so a transaction is as
// long as the caller keeps it selected. CPHA 0: the data line is set before the leading clock edge and the input is
// sampled on it; CPHA 1: set on the leading edge, sampled on the trailing one.
#ifndef SPI_GPIO_H
#define SPI_GPIO_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
	void (*sck)(bool high);
	void (*mosi)(bool high);
	bool (*miso)(void);		// NULL: write only (reads return 0)
	void (*delay)(void);		// half a clock period; NULL: as fast as the pins go
	bool cpol;			// the clock's idle level
	bool cpha;
} spi_gpio_t;

// Put the clock at its idle level (before the first chip select)
void spi_gpio_idle(const spi_gpio_t *b);
uint8_t spi_gpio_byte(const spi_gpio_t *b, uint8_t out);
// len bytes; tx NULL sends 0xFF, rx NULL discards
void spi_gpio_xfer(const spi_gpio_t *b, const uint8_t *tx, uint8_t *rx, uint32_t len);

#endif
