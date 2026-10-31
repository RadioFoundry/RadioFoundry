# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# TYT MD-2017 / Retevis RT82 (with or without GPS): STM32F405VG + HR_C6000 + two AT1846S, the UV380 board family
# (boards/uv380_family has the code; this directory the radio's own data; docs/spec/hardware-md2017.md). The board's
# own wiring is the family's UVF_PINS_MD2017 pin map; the second AT1846S, a second receiver, is set up and left at rest
PLATFORM   := stm32f4
MCU        := stm32f405
# The 8 MHz crystal; the image starts at sector 3, where the bootloader starts it (hardware-md2017.md 1)
HSE_VALUE  := 8000000U
APP_BASE   := 0x0800C000
BOARD_DIRS := boards/md2017 boards/uv380_family platform/stm32f4/hrc platform/stm32f4/spi_flash platform/stm32f4/fsmc_keypad \
	drivers/rf/at1846s drivers/rf/subaudio drivers/modem/hrc6000 drivers/modem/hrc_common drivers/display/st7735 \
	drivers/flash/spi_nor drivers/input/steplines drivers/gps/nmea_line drivers/gps/gps_rx platform/stm32f4/gps_uart
# The HAL's I2C driver besides the platform's modules
BOARD_HAL_MODULES := hal_i2c
MCU_DEFS   += -DUVF_PINS_MD2017=1 -DUVF_RF_CHIPS=2
# The 160x128 panel in 16 colors, every row visible (hardware-md2017.md 4): the 4 bpp frame buffer is 10240 B and 128
# rows. A screen has at most 16 colors (core/ui/gfx.h); with CONFIG_FONT_AA the edge grays compete for them
MCU_DEFS   += -DGFX_INDEXED=1 -DGFX_FB_BYTES=10240u -DGFX_MAX_ROWS=128u -DUVF_LCD_HIDDEN_ROWS=0
# The TYT image starts the HSE as a crystal (hardware-md2017.md 1); the bypass mode stays the fallback
MCU_DEFS   += -DUVF_HSE_XTAL_FIRST=1
