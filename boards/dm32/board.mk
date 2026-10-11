# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
# Baofeng DM-32UV: HR_C7000 SoC (C-SKY CK803S + DMR modem) + FD6818 + ST7789 240x320 on i8080
PLATFORM   := hrc7000
# The CPS-compatible codeplug image sits at +0x700000 in the 16 MB data flash, below it is all stock data
# (docs/spec/open-firmware-cps.md 7.4, hardware-dm32.md 10.1)
MCU_DEFS   := -DCODEPLUG_OPENFW_EEPROM_BASE=0x700000u -DCODEPLUG_OPENFW_FLASH_BASE=0x720000u
LDSCRIPT   := boards/dm32/dm32.ld
BOARD_DIRS := boards/dm32 drivers/rf/fd6818 drivers/modem/hrc7000 drivers/display/st7789 drivers/flash/spi_nor \
	drivers/input/keymatrix drivers/input/quadrature
# State the core marks ODR_CPU_RAM goes to the low SRAM below the codec window (dm32.ld RAM_LO; zeroed by crt0)
MCU_DEFS   += -DODR_CPU_RAM_SECTION='".ram_lo"'
