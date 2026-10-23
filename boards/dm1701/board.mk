# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
# Baofeng DM-1701: STM32F405VG + HR_C6000 + AT1846S
PLATFORM   := stm32f4
MCU        := stm32f405
# The 8 MHz crystal; the image starts at sector 3, where the bootloader starts it (hardware-dm1701.md 1.4)
HSE_VALUE  := 8000000U
APP_BASE   := 0x0800C000
BOARD_DIRS := boards/dm1701 platform/stm32f4/hrc platform/stm32f4/spi_flash platform/stm32f4/fsmc_keypad drivers/rf/at1846s drivers/rf/subaudio drivers/modem/hrc6000 drivers/modem/hrc_common drivers/display/st7735 drivers/flash/spi_nor \
	drivers/input/quadrature
# The HAL's I2C driver besides the platform's modules
BOARD_HAL_MODULES := hal_i2c
# The 160x120 panel in 16 colors (display_dm1701.c): the 4 bpp frame buffer is 9600 B and 120 rows, not the 40 KB / 160 row
# RGB565 default. A screen has at most 16 colors (core/ui/gfx.h); with CONFIG_FONT_AA the edge grays compete for them
MCU_DEFS   += -DGFX_INDEXED=1 -DGFX_FB_BYTES=9600u -DGFX_MAX_ROWS=120u
