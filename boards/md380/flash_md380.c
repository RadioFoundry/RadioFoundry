// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// MD-380 data flash: a Winbond W25Q on SPI1 (PB3 SCK, PB4 MISO, PB5 MOSI, AF5), mode 0, APB2/16, chip select PD7
// (docs/spec/hardware-md380.md 12), through the shared driver (platform/stm32f4/spi_flash). Early radios are reported
// with 1 MB parts, later ones with 16 MB: the size is probed, and the core gets its areas only on a 16 MB part (the
// 16 MB STM32 layout). On a smaller part nothing is stored (settings live in RAM) and the stock codeplug is left alone.
#include "md380.h"
#include "pins.h"
#include "spi_flash/spi_flash_board.h"

// The 16 MB STM32 layout (hardware-md380.md 12.2). The open-firmware codeplug stays where the CPS tools expect it; our
// own areas sit below the open-firmware flash image (0x011000..0x01FFFF, which no CPS address maps to) and above the 14
// MB mark that qdmr's DMR ID database may reach. The stock TYT codeplug at 0 is overwritten by the first open-firmware
// write
static const hal_flash_map_t map = {
	.settings_addr = 0x011000u,
	.settings_size = 0x008000u,	// 8 sectors
	.journal_addr = 0x019000u,	// 2 sectors
	.openfw_eeprom = 0x000000u,
	.openfw_flash = 0x020000u,
	.openfw_end = 0xE00000u,
	.calibration_addr = 0,		// the calibration is in the security registers (calibration_md380.c)
	.calibration_size = 0,
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
	.device = 0x4000u, .device_mask = 0xFF00u,	// any capacity of the W25Q family (JEDEC memory type 0x40)
	.map_min_size = 0x1000000u,			// 16 MB: room for the open-firmware layout
	.map = &map,
};

bool md380_flash_init(void)
{
	return stm32_spi_flash_init(&cfg);
}
