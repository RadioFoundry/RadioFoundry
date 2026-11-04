# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# Retevis RT10: HR_C7000 SoC (C-SKY CK803S + DMR modem) + FD6818 on a two-wire bus, no display, a 16-position channel
# knob (docs/spec/hardware-rt10.md). The PC link is UART0 on the accessory jack
PLATFORM   := hrc7000
# No vendor loader: the boot ROM starts the image its flag at flash 0 names (hardware-rt10.md 2). RadioFoundry's flag
# names 0x0300C000, so the image sits where it does on the other HR_C7000 radios
APP_BASE   := 0x0300C000
# 2 MB program flash (GD25Q16): the debug link's READ_MEM window over it (platform/hrc7000/mem_window.h)
MCU_DEFS   := -DHRC_XIP_FLASH_SIZE=0x200000u
BOARD_DIRS := boards/rt10 drivers/flash/spi_nor drivers/input/sharedptt
# No display: the core still draws into a 128x64 1 bpp frame buffer (display_rt10.c), read only by the debug link
MCU_DEFS   += -DGFX_FB_BYTES=1024u -DGFX_MAX_ROWS=64u
