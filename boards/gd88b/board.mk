# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
# Radioddity GD-88, SoC B: the second receiver (docs/spec/hardware-gd88.md 10). The same HR_C7000 SoC and BK4819 pair as
# SoC A, no display, keys or PC connector: it receives what A asks for over the link on UART3 (core/rx2) and plays it
# into A's audio routing. It runs none of the radio core: its start-up is app_gd88b.c. Receive only. The radio, DMR and
# calibration code is A's (boards/gd88/common)
PLATFORM   := hrc7000
MCU_DEFS   := -DHRC_HOST_UART=1 -DHRC_LOADER_CPU_HZ=72000000u -DHRC_CPU_HZ=168000000u
LDSCRIPT   := boards/gd88/gd88.ld
BOARD_DIRS := boards/gd88b boards/gd88/common drivers/rf/bk4819 drivers/modem/hrc7000
BOARD_INC  := boards/gd88/common
CORE_BASE_DIRS :=
BOARD_CORE_DIRS := core/rx2
# The first stage at 0x0300C000 (boot/gd88b): offers the boot ROM's handshake on the link (UART3), for a minute when
# the application asked for it, then the application at 0x0300D000
BOOT_STAGE := gd88b
BOOT_SIZE  := 0x1000
BOOT_DEFS  := -DBOOT_WINDOW=10 -DAPP_RESUME=0x0300D000
BOOT_SIGNATURE := 2c1101c0
