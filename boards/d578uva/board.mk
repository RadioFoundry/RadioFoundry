# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# AnyTone AT-D578UV (mobile), Artery builds (from mid-2022): AT32F435 + SCT3258 DMR baseband + four QA4818N RF
# chips (BK4819-class) + ST7735S 160x128 on an 8080 bus (docs/spec/hardware-d578uv.md 10). The wiring is the GD32F303
# builds', so the radio's code is boards/d578uv's, built here on the AT32F43x platform; this directory holds what
# differs: the platform, the memory layout and the feature defaults
PLATFORM   := at32f43x
MCU        := at32f435
# The image starts where the vendor bootloader starts the application, as on the GD32 builds (hardware-d578uv.md 10)
APP_BASE   := 0x08006000
# Bench knobs (docs/testing/d578uv-bringup.md), e.g. D578UV_DEFS="-DD578UV_LEVEL_IC=0" to keep SWD on
D578UV_DEFS ?=
MCU_DEFS   += $(D578UV_DEFS)
# The radio task's deepest chain (a TFTP ID database write through the NAND sectors, with VOX on) is 2740 bytes here,
# past the core's 768 words: 800 words leaves about 120 bytes (make stack-check)
MCU_DEFS   += -DRADIO_TASK_STACK=800
BOARD_DIRS := boards/d578uva boards/d578uv drivers/display/st7735 drivers/flash/spi_nand drivers/input/quadrature \
              drivers/modem/sct drivers/rf/rfbus drivers/rf/subaudio drivers/rf/qa4818 drivers/gps/nmea_line \
              drivers/gps/gps_rx platform/at32f43x/gps_uart drivers/bcast/ktamfm drivers/bus/i2c_gpio
# The core's storage on the NAND, with CONFIG_NAND_STORAGE (hardware-d578uv.md 6.3)
BOARD_NAND_DIRS := drivers/flash/nand_sectors
# The 160x128 panel in 16 colors, as on the GD32 builds (boards/d578uv/board.mk)
MCU_DEFS   += -DGFX_INDEXED=1 -DGFX_FB_BYTES=10240u -DGFX_MAX_ROWS=128u
