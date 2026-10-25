# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# TYT MD-UV380 / MD-UV390 / Retevis RT3S (with or without GPS), and the MD-UV390 Plus 10 W: STM32F405VG + HR_C6000 + one
# AT1846S for both bands, the UV380 board family (boards/uv380_family has the code; this directory the radio's own data;
# docs/spec/hardware-mduv380.md). The Plus 10 W swaps two pins and has its own power tables: a build option, POWER=5w
# (default) or POWER=10w; C sees RFOUNDRY_POWER_5W / RFOUNDRY_POWER_10W (the Makefile, "Hardware build options")
PLATFORM   := stm32f4
BUILD_OPTIONS := POWER
POWER_VALUES  := 5w 10w
MCU        := stm32f405
# The 8 MHz reference; the image starts at sector 3, where the bootloader starts it (hardware-mduv380.md 11)
HSE_VALUE  := 8000000U
APP_BASE   := 0x0800C000
# Bench knobs (docs/testing/mduv380-bringup.md), e.g. MDUV380_DEFS="-DMDUV380_10W_CHECK=0"
MDUV380_DEFS ?=
MCU_DEFS   += $(MDUV380_DEFS)
BOARD_DIRS := boards/mduv380 boards/uv380_family platform/stm32f4/hrc platform/stm32f4/spi_flash platform/stm32f4/fsmc_keypad \
	drivers/rf/at1846s drivers/rf/subaudio drivers/modem/hrc6000 drivers/modem/hrc_common drivers/display/st7735 \
	drivers/flash/spi_nor drivers/input/quadrature drivers/gps/nmea_line drivers/gps/gps_rx platform/stm32f4/gps_uart
# The HAL's I2C driver besides the platform's modules
BOARD_HAL_MODULES := hal_i2c
# The 160x128 panel in 16 colors, every row visible (hardware-mduv380.md 4): the 4 bpp frame buffer is 10240 B and 128
# rows. A screen has at most 16 colors (core/ui/gfx.h); with CONFIG_FONT_AA the edge grays compete for them
MCU_DEFS   += -DGFX_INDEXED=1 -DGFX_FB_BYTES=10240u -DGFX_MAX_ROWS=128u -DUVF_LCD_HIDDEN_ROWS=0
# The TYT images start the HSE as a crystal (hardware-mduv380.md 2); the bypass mode stays the fallback
MCU_DEFS   += -DUVF_HSE_XTAL_FIRST=1
