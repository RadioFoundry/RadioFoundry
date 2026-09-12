# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
# Baofeng DM-1701: STM32F405VG + HR_C6000 + AT1846S
PLATFORM   := stm32f4
MCU_DEFS   := -DSTM32F405xx -DHSE_VALUE=8000000U -DUSER_VECT_TAB_ADDRESS -DVECT_TAB_OFFSET=0xC000U
LDSCRIPT   := boards/dm1701/dm1701.ld
# The linker script's CPU-only RAM (CCM: no DMA): zeroed at start-up for ODR_CPU_RAM, left alone for _NOINIT
MCU_DEFS   += -DODR_CPU_RAM_SECTION='".ccmbss"' -DODR_CPU_RAM_NOINIT_SECTION='".ccmram"'
BOARD_DIRS := boards/dm1701 drivers/rf/at1846s drivers/modem/hrc6000 drivers/display/st7735 drivers/flash/spi_nor
# Built only with CONFIG_USB
BOARD_USB_DIRS := drivers/usb
HAL_MODULES := hal hal_cortex hal_rcc hal_rcc_ex hal_gpio hal_pwr hal_pwr_ex hal_flash hal_flash_ex hal_dma hal_i2c \
	hal_spi hal_tim hal_tim_ex hal_adc hal_adc_ex hal_dac hal_dac_ex hal_sram ll_fsmc
