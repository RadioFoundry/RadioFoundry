# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# Radioddity DB25-D (mobile): HR_C7000 SoC (C-SKY CK803S + DMR modem) + the superhet's main transceiver and LO (the PRO's
# two BK4819, or an older batch's FD6818 with an FD6818, 0x7813 or AT1141 LO; one image detects which) + ST7735S 160x128
# on a bit-banged serial bus (docs/spec/hardware-db25d.md). The PC link is UART1
PLATFORM   := hrc7000
# The IAP loader leaves the PLL at 72 MHz (hardware-db25d.md 2). With DB25D_FULL_CLOCK=1 (default) RadioFoundry re-programs it to
# the platform's 168 MHz CPU / 84 MHz APB at start-up (platform/hrc7000/clock.c) and puts the loader's clocks back before
# it hands over to the runtime stub (built for them) or resets. DB25D_FULL_CLOCK=0 keeps the loader's 72 / 36 MHz.
# The runtime stub's RAM window (HRC_STUB_BASE) and, without DMR, the linker script are set in config.mk: the stub gets
# the stock codec's RAM window with DMR (memory.ld), and 128 KB above the bss without it (fm_only.ld keeps it free)
DB25D_FULL_CLOCK ?= 1
MCU_DEFS   := -DHRC_HOST_UART=1 -DHRC_LOADER_CPU_HZ=72000000u
ifeq ($(DB25D_FULL_CLOCK),1)
MCU_DEFS   += -DHRC_CPU_HZ=168000000u
else
MCU_DEFS   += -DHRC_CPU_HZ=72000000u
endif
BOARD_DIRS := boards/db25d drivers/rf/bk4819 drivers/rf/fd6818 drivers/rf/at1141 drivers/rf/subaudio drivers/display/st7735 drivers/input/keymatrix drivers/input/quadrature drivers/input/sharedptt drivers/input/plugptt drivers/input/serialmic \
	drivers/flash/spi_nand drivers/modem/hrc7000
# The core's storage on the NAND, with CONFIG_NAND_STORAGE (hardware-db25d.md 6.6)
BOARD_NAND_DIRS := drivers/flash/nand_sectors
# The image starts where the IAP loader jumps (hardware-db25d.md 2) with the first stage (boot/db25d): the stub window
# on the PC cable at power-on, then the application right after it. It starts with the stock entry the IAP loader checks
# for
APP_BASE   := 0x0300C000
BOOT_STAGE := db25d
BOOT_SIZE  := 0x1000
BOOT_DEFS  := -DBOOT_WINDOW=10
BOOT_SIGNATURE := 2c1101c0
# The 160x128 panel in 16 colors (display_db25d.c): the 4 bpp frame buffer is 10240 B and 128 rows, not the 40 KB / 160 row
# RGB565 default. A screen has at most 16 colors (core/ui/gfx.h); with CONFIG_FONT_AA the edge grays compete for them
MCU_DEFS   += -DGFX_INDEXED=1 -DGFX_FB_BYTES=10240u -DGFX_MAX_ROWS=128u
