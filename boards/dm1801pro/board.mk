# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
# Baofeng DM-1801 Pro: HR_C7000 SoC (C-SKY CK803S + DMR modem) + FD6818 + 128x64 monochrome ST7567-class LCD on a
# bit-banged serial bus (docs/spec/hardware-dm1801pro.md). The PC link is UART0 on the programming cable
PLATFORM   := hrc7000
# The CPS-compatible codeplug image sits at +0x700000 in the 16 MB data flash, below it is all factory data
# (docs/spec/hardware-dm1801pro.md 6.1)
MCU_DEFS   := -DCODEPLUG_OPENFW_EEPROM_BASE=0x700000u -DCODEPLUG_OPENFW_FLASH_BASE=0x720000u
LDSCRIPT   := boards/dm1801pro/dm1801pro.ld
BOARD_DIRS := boards/dm1801pro drivers/rf/fd6818 drivers/rf/subaudio drivers/modem/hrc7000 drivers/display/st7567 drivers/flash/spi_nor \
	drivers/input/keymatrix drivers/input/sharedptt drivers/cal/baofeng \
	drivers/bcast/rda5807m drivers/bus/i2c_gpio drivers/sensor/lis3dh
# State the core marks ODR_CPU_RAM goes to the low SRAM below the codec window (dm1801pro.ld RAM_LO; zeroed by crt0)
MCU_DEFS   += -DODR_CPU_RAM_SECTION='".ram_lo"'
# The stock codec host (platform/hrc7000/vocoder.c): its profile id is dm1801pro-049
MCU_DEFS   += -DHRC_CODEC_BOARD='"dm1801pro"'
