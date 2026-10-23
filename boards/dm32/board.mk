# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# Baofeng DM-32UV: HR_C7000 SoC (C-SKY CK803S + DMR modem) + FD6818 + ST7789 240x320 on i8080
PLATFORM   := hrc7000
# Behind the Baofeng bootloader (hardware-dm32.md 2)
APP_BASE   := 0x0300C000
# The CPS-compatible codeplug image sits at +0x700000 in the 16 MB data flash, below it is all stock data
# (docs/spec/open-firmware-cps.md 7.4, hardware-dm32.md 10.1)
MCU_DEFS   := -DCODEPLUG_OPENFW_EEPROM_BASE=0x700000u -DCODEPLUG_OPENFW_FLASH_BASE=0x720000u
BOARD_DIRS := boards/dm32 drivers/rf/fd6818 drivers/rf/subaudio drivers/modem/hrc7000 drivers/display/st7789 drivers/flash/spi_nor \
	drivers/input/keymatrix drivers/input/quadrature drivers/input/sharedptt drivers/cal/baofeng
# The 240x320 panel at full resolution in 16 colors (display_dm32.c): the frame buffer is 37.5 KB (2.5 KB under the
# 40 KB default) and has 320 rows
MCU_DEFS   += -DGFX_INDEXED=1 -DGFX_FB_BYTES=38400u -DGFX_MAX_ROWS=320u
