# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
# TYT MD-9600 / Retevis RT90 (mobile): STM32F405VG + HR_C6000 + two RF chains (VHF, UHF), each with its own synthesizer
# (SKY72310 on V1-V3, "PLL1590" on V4, LMX2571 on V4A / V5) and IF receiver (AK2365A on V1-V4) + ST7567 128x64 LCD and
# 25Q128 flash on SPI2 (docs/spec/hardware-md9600.md). One image for every board revision: the revision is chosen at
# run time (rfplan_md9600.h); the per-revision drivers are added in config.mk with their switches
PLATFORM   := stm32f4
MCU_DEFS   := -DSTM32F405xx -DHSE_VALUE=8000000U -DUSER_VECT_TAB_ADDRESS -DVECT_TAB_OFFSET=0xC000U
# The revision driven when none is stored and the probe has no verdict: v5 (V4A / V5, the current boards), v4 or v1v3
MD9600_REV_DEFAULT ?= v5
MCU_DEFS   += -DMD9600_REV_DEFAULT=MD9600_REV_$(shell echo $(MD9600_REV_DEFAULT) | tr a-z A-Z)
# The receiver probe at start-up (rev_probe in radio_md9600.c): off until its thresholds are measured on radios
MD9600_REV_PROBE ?= 0
MCU_DEFS   += -DMD9600_REV_PROBE=$(MD9600_REV_PROBE)
LDSCRIPT   := boards/md9600/md9600.ld
# The linker script's CPU-only RAM (CCM: no DMA): zeroed at start-up for ODR_CPU_RAM, left alone for _NOINIT
MCU_DEFS   += -DODR_CPU_RAM_SECTION='".ccmbss"' -DODR_CPU_RAM_NOINIT_SECTION='".ccmram"'
BOARD_DIRS := boards/md9600 drivers/display/st7567 drivers/flash/spi_nor drivers/input/keymatrix drivers/input/quadrature
# Built only with CONFIG_USB
BOARD_USB_DIRS := drivers/usb
HAL_MODULES := hal hal_cortex hal_rcc hal_rcc_ex hal_gpio hal_pwr hal_pwr_ex hal_flash hal_flash_ex hal_dma
