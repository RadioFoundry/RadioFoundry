# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# Baofeng DM-1701 / Retevis RT84: STM32F405VG + HR_C6000 + AT1846S, the UV380 board family (boards/uv380_family has the
# code; this directory the radio's own data)
PLATFORM   := stm32f4
MCU        := stm32f405
# The 8 MHz crystal; the image starts at sector 3, where the bootloader starts it (hardware-dm1701.md 1.4)
HSE_VALUE  := 8000000U
APP_BASE   := 0x0800C000
BOARD_DIRS := boards/dm1701 boards/uv380_family platform/stm32f4/hrc platform/stm32f4/spi_flash platform/stm32f4/fsmc_keypad drivers/rf/at1846s drivers/rf/subaudio drivers/modem/hrc6000 drivers/modem/hrc_common drivers/display/st7735 drivers/flash/spi_nor \
	drivers/input/quadrature drivers/gps/nmea_line drivers/gps/gps_rx platform/stm32f4/gps_uart
# The HAL's I2C driver besides the platform's modules
BOARD_HAL_MODULES := hal_i2c
# The 160x120 panel in 16 colors (boards/uv380_family/display_uvf.c): the 4 bpp frame buffer is 9600 B and 120 rows, not the 40 KB / 160 row
# RGB565 default. A screen has at most 16 colors (core/ui/gfx.h); with CONFIG_FONT_AA the edge grays compete for them
MCU_DEFS   += -DGFX_INDEXED=1 -DGFX_FB_BYTES=9600u -DGFX_MAX_ROWS=120u
# The lens hides the panel's last 8 rows (boards/uv380_family/display_uvf.c)
MCU_DEFS   += -DUVF_LCD_HIDDEN_ROWS=8
