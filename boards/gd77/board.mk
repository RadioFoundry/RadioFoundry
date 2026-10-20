# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
# Radioddity GD-77 / TYT MD-760: NXP MK22FN512 (Kinetis K22F, Cortex-M4F) + HR_C6000 + AT1846S + 128x64 monochrome
# UC1701 / ST7567-class LCD on a bit-banged serial bus (docs/spec/hardware-gd77.md). Bring-up: the image is the raw
# application for flash 0x4000; the radio's update path needs a packaging step this repository doesn't have yet (10)
PLATFORM   := k22f
LDSCRIPT   := boards/gd77/gd77.ld
# The upper SRAM half: the core's ODR_CPU_RAM state (zeroed at start-up) and the RTOS heap (gd77.ld)
MCU_DEFS   := -DODR_CPU_RAM_SECTION='".ramu_bss"' -DODR_CPU_RAM_NOINIT_SECTION='".ramu_noinit"'
BOARD_DIRS := boards/gd77 drivers/rf/at1846s drivers/rf/subaudio drivers/modem/hrc6000 drivers/modem/hrc_common \
	drivers/display/st7567 drivers/flash/spi_nor drivers/input/keymatrix drivers/bus/i2c_gpio drivers/bus/spi_gpio
# The 128x64 panel at 1 bit a pixel: a 1 KB frame buffer of 64 rows instead of the 40 KB default
MCU_DEFS   += -DGFX_FB_BYTES=1024u -DGFX_MAX_ROWS=64u
