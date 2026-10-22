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
BOARD_DIRS := boards/md380 platform/stm32f4/hrc drivers/rf/sky72310 drivers/rf/subaudio drivers/modem/hrc5000 drivers/modem/hrc_common drivers/display/st7735 drivers/flash/spi_nor \
	drivers/gps/nmea_line drivers/gps/gps_rx platform/stm32f4/gps_uart
