// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// UV380 family data flash: W25Q128 on SPI1 (PB3 SCK, PB4 MISO, PB5 MOSI, AF5), mode 0, APB2/16, chip select PD7
// (docs/spec/hardware-dm1701.md 13), through the shared driver (platform/stm32f4/spi_flash).
#include "uvf.h"
#include "pins.h"
#include "spi_flash/spi_flash_board.h"

// docs/spec/hardware-dm1701.md 13.2. The open-firmware codeplug stays where the CPS tools expect it; our own areas sit
// in the gap between the calibration copy and the open-firmware flash image (0x011000..0x01FFFF, which no CPS address
// maps to) and above the 14 MB mark that qdmr's DMR ID database may reach
static const hal_flash_map_t map = {
	.settings_addr = 0x011000u,
	.settings_size = 0x008000u,	// 8 sectors
	.journal_addr = 0x019000u,	// 2 sectors
	.openfw_eeprom = 0x000000u,
	.openfw_flash = 0x020000u,
	.openfw_end = 0xE00000u,
	.calibration_addr = 0x010000u,
	.calibration_size = 0x200u,
	.native_addr = 0xE00000u,
	.native_size = 0x100000u,
	.staging_addr = 0xF00000u,	// 256 KB of the unused top megabyte (docs/spec/tftp.md 4)
	.staging_size = 0x040000u,
};

static const stm32_spi_flash_cfg_t cfg = {
	.spi = SPI1,
	.bus_port = PIN_PORT(PIN_FLASH_SCK),	// SCK, MISO and MOSI are on one port (pins.h)
	.bus_pins = PIN_MASK(PIN_FLASH_SCK) | PIN_MASK(PIN_FLASH_MISO) | PIN_MASK(PIN_FLASH_MOSI),
	.af = GPIO_AF5_SPI1,
	.cs_port = PIN_PORT(PIN_FLASH_CS),
	.cs_pin = PIN_MASK(PIN_FLASH_CS),
	.cpol = false, .cpha = false,		// mode 0
	.br = 3,				// APB2 72 MHz / 16 = 4.5 MHz
	.device = 0x4018u, .device_mask = 0xFFFFu,	// W25Q128: memory type 0x40, capacity 0x18
	.map_min_size = 0,
	.map = &map,
};

bool uvf_flash_init(void)
{
	return stm32_spi_flash_init(&cfg);
}
