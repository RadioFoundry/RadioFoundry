# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# Radioddity GD-88, SoC B: the second receiver (docs/spec/hardware-gd88.md 10). The same HR_C7000 SoC and BK4819 pair as
# SoC A, no display, keys or PC connector: it receives what A asks for over the link on UART3 (core/rx2) and plays it
# into A's audio routing. It runs none of the radio core: its start-up is app_gd88b.c. Receive only. The radio, DMR and
# calibration code is A's (boards/gd88/common)
PLATFORM   := hrc7000
MCU_DEFS   := -DHRC_HOST_UART=1 -DHRC_LOADER_CPU_HZ=72000000u
BOARD_DIRS := boards/gd88b boards/gd88/common drivers/rf/bk4819 drivers/rf/subaudio drivers/modem/hrc7000
BOARD_INC  := boards/gd88/common
CORE_BASE_DIRS :=
BOARD_CORE_DIRS := core/rx2
# The debug link's text helpers: B's diagnostic lines and the crash log's summary use them
BOARD_CORE_FILES := core/debuglink/dl_fmt.c
# Its own debug link server, answering through A (core/rx2/rx2_dl.c, docs/spec/rx2-link.md 2.3): the log ring, the
# chip checks and the crash log's view sections, without the debug link itself. B has no host port, so this is its
# debug link; GD88B_DEBUG=0 builds it without (B then answers every forwarded command "unknown")
ifneq ($(GD88B_DEBUG),0)
BOARD_CORE_FILES += core/debuglink/dlog.c core/debuglink/dl_chip.c core/debuglink/dl_crash_view.c
MCU_DEFS += -DCONFIG_RX2_DL_SERVER=1
endif
# The image starts where B's IAP loader jumps, with the first stage (boot/gd88b): it offers the boot ROM's handshake on
# the link (UART3), for a minute when the application asked for it, then runs the application right after it
APP_BASE   := 0x0300C000
BOOT_STAGE := gd88b
BOOT_SIZE  := 0x1000
BOOT_DEFS  := -DBOOT_WINDOW=10
BOOT_SIGNATURE := 2c1101c0
