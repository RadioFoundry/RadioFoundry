# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
# TYT MD-380 / MD-390, Retevis RT3 (with or without GPS): STM32F405VG + HR_C5000 + SKY72310 synthesizer + GT3136 FM
# receiver, one band per radio (docs/spec/hardware-md380.md). The band is a build choice: MD380_BAND=uhf (default,
# 400-480 MHz) or MD380_BAND=vhf (136-174 MHz)
PLATFORM   := stm32f4
MD380_BAND ?= uhf
MCU_DEFS   := -DSTM32F405xx -DHSE_VALUE=8000000U -DUSER_VECT_TAB_ADDRESS -DVECT_TAB_OFFSET=0xC000U
MCU_DEFS   += -DMD380_VHF=$(if $(filter vhf,$(MD380_BAND)),1,0)
# Bench knobs (docs/testing/md380-bringup.md), e.g. MD380_DEFS="-DMD380_PANEL_TYPE=1 -DMD380_KEEP_SWD"
MD380_DEFS ?=
MCU_DEFS   += $(MD380_DEFS)
LDSCRIPT   := boards/md380/md380.ld
# The linker script's CPU-only RAM (CCM: no DMA, not zeroed at start-up) for core state marked ODR_CPU_RAM
MCU_DEFS   += -DODR_CPU_RAM_SECTION='".ccmram"'
BOARD_DIRS := boards/md380 drivers/rf/sky72310 drivers/modem/hrc6000 drivers/display/st7735 drivers/flash/spi_nor \
	drivers/gps/nmea_line
# Built only with CONFIG_USB
BOARD_USB_DIRS := drivers/usb
HAL_MODULES := hal hal_cortex hal_rcc hal_rcc_ex hal_gpio hal_pwr hal_pwr_ex hal_flash hal_flash_ex hal_dma \
	hal_spi hal_tim hal_tim_ex hal_adc hal_adc_ex hal_dac hal_dac_ex hal_sram ll_fsmc
