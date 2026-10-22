// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The data flash of an STM32F4 radio: a SPI NOR chip (drivers/flash/spi_nor) on one of the SPI blocks, as hal_flash_t,
// with its security registers. One mutex serializes every transaction once the scheduler runs. A board adds
// platform/stm32f4/spi_flash and drivers/flash/spi_nor to BOARD_DIRS, calls stm32_spi_flash_init() from board_init
// (spi_flash_board.h) and points board_t.flash at stm32_spi_flash. This header has no STM32 types, so the board's host
// tests can include it.
#ifndef STM32_SPI_FLASH_H
#define STM32_SPI_FLASH_H

#include <stdint.h>
#include <stdbool.h>
#include "hal_flash.h"

extern hal_flash_t stm32_spi_flash;

// Security registers 1..3, 256 bytes each
bool stm32_spi_flash_read_security(uint8_t reg, uint32_t offset, void *buf, uint32_t len);
// The open-firmware CPS's area 10 (flash security registers), by the chip's register address (reg << 12 | offset)
bool stm32_spi_flash_cps_security_read(uint32_t addr, void *buf, uint32_t len);
// JEDEC memory type and capacity, 0 = none
uint16_t stm32_spi_flash_device(void);

#endif
