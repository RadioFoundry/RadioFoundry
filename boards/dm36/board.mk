# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# Board wiring and layout: docs/spec/hardware-dm36.md.
include boards/dm32/board.mk
BOARD_DIRS := boards/dm36 $(BOARD_DIRS)
MCU_DEFS += -DBOARD_EXTERNAL_DESCRIPTOR -Iboards/dm36 -Iboards/dm32
LDSCRIPT := boards/dm36/memory.ld
# Preserve the 48 KiB stock bootloader (hardware-dm36.md 2).
APP_BASE := 0x0300C000
# The data flash map is the shared one of these sources: RadioFoundry writes only from 0x700000 up, above every stock area
# (hardware-dm36.md 10.3)
