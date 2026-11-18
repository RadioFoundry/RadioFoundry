# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# AnyTone AT-D578UV (mobile), Artery builds (from mid-2022): AT32F435 + SCT3258 DMR baseband + four QA4818N RF
# chips (BK4819-class) + ST7735S 160x128 on an 8080 bus. The wiring is the GD32F303
# builds', so the radio's code is boards/d578uv's, built here on the AT32F43x platform; this directory holds what
# differs: the platform, the memory layout and the feature defaults
PLATFORM   := at32f43x
MCU        := at32f435
# The image starts where the vendor bootloader starts the application, as on the GD32 builds
APP_BASE   := 0x08006000
# Bench knobs, e.g. D578UV_DEFS="-DD578UV_LEVEL_IC=0" to keep SWD on
# board_t .name (boards/d578uv/board.c): not the GD32 build's, so the tools tell the two apart
BOARD_T_NAME := AT-D578UV-AT32
D578UV_DEFS ?=
MCU_DEFS   += $(D578UV_DEFS)
# The deepest chains here end in a NAND read's lock, under which a kernel assert would capture the crash
# (crash_assert: its 88 bytes and the kernel's 136): the radio task's (a TFTP ID database write through the NAND
# sectors) is 2960 bytes, the host task's (a debug link key store) 2712, the UI's (a message row's contact name) 1824
# and the start task's (a Tier III system's codeplug records read at boot) 2320.
# With make stack-check's overhead and margin that's 824, 762, 540 and 664 words, 0 to 8 bytes to spare
MCU_DEFS   += -DRADIO_TASK_STACK=824 -DHOSTLINK_TASK_STACK=762 -DUI_TASK_STACK_WORDS=540 -DSTART_TASK_STACK=664
BOARD_DIRS := boards/d578uva boards/d578uv drivers/display/st7735 drivers/flash/spi_nand drivers/input/quadrature \
              drivers/modem/sct drivers/rf/rfbus drivers/rf/subaudio drivers/rf/qa4818 drivers/gps/nmea_line \
              drivers/gps/gps_rx platform/at32f43x/gps_uart drivers/bcast/ktamfm drivers/bcast/rda5807m drivers/bus/i2c_gpio
# The core's storage on the NAND, with CONFIG_NAND_STORAGE
BOARD_NAND_DIRS := drivers/flash/nand_sectors drivers/flash/nand_blocks
# The 160x128 panel in 16 colors, as on the GD32 builds (boards/d578uv/board.mk)
MCU_DEFS   += -DGFX_INDEXED=1 -DGFX_FB_BYTES=10240u -DGFX_MAX_ROWS=128u
