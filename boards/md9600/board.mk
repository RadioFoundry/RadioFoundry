# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
# TYT MD-9600 / Retevis RT90 (mobile): STM32F405VG + HR_C6000 + two RF chains (VHF, UHF), each with its own synthesizer
# (SKY72310 on V1-V3, "PLL1590" on V4, LMX2571 on V4A / V5) and IF receiver (AK2365A on V1-V4) + ST7567 128x64 LCD and
# 25Q128 flash on SPI2 (docs/spec/hardware-md9600.md). One image for every board revision: the revision is chosen at
# run time (rfplan_md9600.h); the per-revision drivers are added in config.mk with their switches
PLATFORM   := stm32f4
MCU        := stm32f405
# The 8 MHz crystal; the image starts at sector 3, behind the 48 KB TYT bootloader (hardware-md9600.md 1.3)
HSE_VALUE  := 8000000U
APP_BASE   := 0x0800C000
# The 128x64 panel is 1 bit per pixel (display_md9600.c): 1 KB of frame buffer and 64 rows, not the 40 KB / 160 row
# default
MCU_DEFS   += -DGFX_FB_BYTES=1024u -DGFX_MAX_ROWS=64u
# The revision driven when none is stored and the probe has no verdict: v5 (V4A / V5, the current boards), v4 or v1v3
MD9600_REV_DEFAULT ?= v5
MCU_DEFS   += -DMD9600_REV_DEFAULT=MD9600_REV_$(shell echo $(MD9600_REV_DEFAULT) | tr a-z A-Z)
# The receiver probe at start-up (rev_probe in radio_md9600.c): off until its thresholds are measured on radios
MD9600_REV_PROBE ?= 0
MCU_DEFS   += -DMD9600_REV_PROBE=$(MD9600_REV_PROBE)
BOARD_DIRS := boards/md9600 drivers/display/st7567 drivers/flash/spi_nor drivers/input/keymatrix drivers/input/quadrature \
	drivers/cal/calblock drivers/input/adckeys platform/stm32f4/hrc platform/stm32f4/spi_flash drivers/modem/hrc6000 drivers/modem/hrc_common \
	drivers/gps/nmea_line drivers/gps/gps_rx platform/stm32f4/gps_uart
