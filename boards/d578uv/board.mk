# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# AnyTone AT-D578UV (mobile), GD32F303 builds: GD32F303VK + SCT3258 DMR baseband + four QA4818N RF chips (BK4819-class) +
# ST7735S 160x128 on an 8080 bus (docs/spec/hardware-d578uv.md). Radios built from mid-2022 carry an Artery AT32F43x
# instead, which this image doesn't run on (hardware-d578uv.md 1)
PLATFORM   := gd32f30x
MCU        := gd32f303
# The image starts where the vendor bootloader starts the application (hardware-d578uv.md 2)
APP_BASE   := 0x08006000
# Bench knobs (docs/testing/d578uv-bringup.md), e.g. D578UV_DEFS="-DD578UV_LEVEL_IC=0" to keep SWD on
D578UV_DEFS ?=
MCU_DEFS   += $(D578UV_DEFS)
BOARD_DIRS := boards/d578uv drivers/display/st7735 drivers/flash/spi_nand drivers/input/quadrature drivers/modem/sct \
              drivers/rf/rfbus drivers/rf/subaudio drivers/rf/qa4818 drivers/gps/nmea_line drivers/gps/gps_rx \
              platform/gd32f30x/gps_uart drivers/bcast/ktamfm drivers/bus/i2c_gpio
# The core's storage on the NAND, with CONFIG_NAND_STORAGE (hardware-d578uv.md 6.3)
BOARD_NAND_DIRS := drivers/flash/nand_sectors
# The 160x128 panel in 16 colors (display_d578uv.c): the 4 bpp frame buffer is 10240 B and 128 rows, not the 40 KB / 160 row
# RGB565 default. A screen has at most 16 colors (core/ui/gfx.h); with CONFIG_FONT_AA the edge grays compete for them
MCU_DEFS   += -DGFX_INDEXED=1 -DGFX_FB_BYTES=10240u -DGFX_MAX_ROWS=128u
