# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
# TYT MD-380 / MD-390, Retevis RT3 (with or without GPS): STM32F405VG + HR_C5000 + SKY72310 synthesizer + GT3136 FM
# receiver, one band per radio (docs/spec/hardware-md380.md). The band is a build option: BAND=uhf (default,
# 400-480 MHz) or BAND=vhf (136-174 MHz); C sees ODR_BAND_UHF / ODR_BAND_VHF (the Makefile, "Hardware build options")
PLATFORM   := stm32f4
BUILD_OPTIONS := BAND
BAND_VALUES   := uhf vhf
MCU        := stm32f405
# The 8 MHz crystal; the image starts at sector 3, where the Tytera bootloader starts it (hardware-md380.md 1.3)
HSE_VALUE  := 8000000U
APP_BASE   := 0x0800C000
# Bench knobs (docs/testing/md380-bringup.md), e.g. MD380_DEFS="-DMD380_PANEL_TYPE=1 -DMD380_KEEP_SWD"
MD380_DEFS ?=
MCU_DEFS   += $(MD380_DEFS)
BOARD_DIRS := boards/md380 platform/stm32f4/hrc platform/stm32f4/spi_flash platform/stm32f4/fsmc_keypad drivers/rf/sky72310 drivers/rf/subaudio drivers/modem/hrc5000 drivers/modem/hrc_common drivers/display/st7735 drivers/flash/spi_nor \
	drivers/gps/nmea_line drivers/gps/gps_rx platform/stm32f4/gps_uart
# The 160x128 panel in 16 colors (display_md380.c): the 4 bpp frame buffer is 10240 B and 128 rows, not the 40 KB / 160 row
# RGB565 default. A screen has at most 16 colors (core/ui/gfx.h); with CONFIG_FONT_AA the edge grays compete for them
MCU_DEFS   += -DGFX_INDEXED=1 -DGFX_FB_BYTES=10240u -DGFX_MAX_ROWS=128u
