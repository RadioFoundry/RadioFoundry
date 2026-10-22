# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
# Baofeng DM-1801 Pro: HR_C7000 SoC (C-SKY CK803S + DMR modem) + FD6818 + 128x64 monochrome ST7567-class LCD on a
# bit-banged serial bus (docs/spec/hardware-dm1801pro.md). The PC link is UART0 on the programming cable
PLATFORM   := hrc7000
# Behind the Baofeng bootloader, as on the DM-32UV (hardware-dm1801pro.md)
APP_BASE   := 0x0300C000
# The CPS-compatible codeplug image sits at +0x700000 in the 16 MB data flash, below it is all factory data
# (docs/spec/hardware-dm1801pro.md 6.1)
MCU_DEFS   := -DCODEPLUG_OPENFW_EEPROM_BASE=0x700000u -DCODEPLUG_OPENFW_FLASH_BASE=0x720000u
BOARD_DIRS := boards/dm1801pro drivers/rf/fd6818 drivers/rf/subaudio drivers/modem/hrc7000 drivers/display/st7567 drivers/flash/spi_nor \
	drivers/input/keymatrix drivers/input/sharedptt drivers/cal/baofeng \
	drivers/bcast/rda5807m drivers/bus/i2c_gpio drivers/sensor/lis3dh
