// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// MD-9600 data flash: a 25Q128-class SPI NOR (16 MB) on SPI2 with the LCD, chip select PB12 (docs/spec/hardware-md9600.md
// 12), through the shared driver (platform/stm32f4/spi_flash) under the board's SPI2 lock. The map and the board's
// record sector are in store_md9600.c. The security registers hold the factory calibration (1-2) and the hardware
// options (3) and are only read.
#include "md9600.h"
#include "pins.h"
#include "store_md9600.h"
#include "spi_flash/spi_flash_board.h"

static const stm32_spi_flash_cfg_t cfg = {
	.spi = SPI2,
	.bus_port = PIN_PORT(PIN_FLASH_SCK),	// SCK, MISO and MOSI are on one port (pins.h)
	.bus_pins = PIN_MASK(PIN_FLASH_SCK) | PIN_MASK(PIN_FLASH_MISO) | PIN_MASK(PIN_FLASH_MOSI),
	.af = GPIO_AF5_SPI2,
	.cs_port = PIN_PORT(PIN_FLASH_CS),
	.cs_pin = PIN_MASK(PIN_FLASH_CS),
	.cpol = true, .cpha = true,		// mode 3, as the LCD on the same bus
	.br = 0,				// APB1 36 MHz / 2 = 18 MHz (hardware-md9600.md 12.1)
	// No assumption about the maker: any part that answers with a size (hardware-md9600.md 12.1)
	.device = 0, .device_mask = 0,
	.map = &md9600_flash_map,
	.map_min_size = MD9600_STORE_FULL_SIZE,
	.bus_take = md9600_spi2_lock,
	.bus_give = md9600_spi2_unlock,
};

bool md9600_flash_init(void)
{
	md9600_spi2_init();	// the LCD's chip select high before the bus runs
	return stm32_spi_flash_init(&cfg);
}

bool md9600_flash_read_security(uint8_t reg, uint32_t offset, void *buf, uint32_t len)
{
	return stm32_spi_flash_read_security(reg, offset, buf, len);
}

md9600_rev_t md9600_flash_rev_load(void)
{
	return stm32_spi_flash.size != 0 ? md9600_store_rev_load(&stm32_spi_flash) : MD9600_REV_UNKNOWN;
}

bool md9600_flash_rev_store(md9600_rev_t rev)
{
	return stm32_spi_flash.size != 0 && md9600_store_rev_save(&stm32_spi_flash, rev);
}
