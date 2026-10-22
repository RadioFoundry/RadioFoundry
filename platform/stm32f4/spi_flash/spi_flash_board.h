// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// What a board gives the STM32F4 data flash (spi_flash.h): the chip's wiring, the parts it accepts and the core's areas
#ifndef STM32_SPI_FLASH_BOARD_H
#define STM32_SPI_FLASH_BOARD_H

#include "platform.h"
#include "spi_flash.h"

typedef struct {
	SPI_TypeDef *spi;
	GPIO_TypeDef *bus_port;		// SCK, MISO and MOSI: one port
	uint16_t bus_pins;
	uint8_t af;
	GPIO_TypeDef *cs_port;
	uint16_t cs_pin;
	bool cpol, cpha;		// SPI mode
	uint8_t br;			// clock PCLK / 2^(br + 1) (stm32_spi_master_init)
	uint16_t device, device_mask;	// the JEDEC memory type and capacity accepted: (id & mask) == device
	// The core's areas, given only on a part of at least map_min_size bytes (smaller: nothing is stored)
	const hal_flash_map_t *map;
	uint32_t map_min_size;
} stm32_spi_flash_cfg_t;

// Probe the chip (from board_init; cfg stays valid). False: no accepted chip, size 0
bool stm32_spi_flash_init(const stm32_spi_flash_cfg_t *cfg);

#endif
