# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# Radioddity GD-88, SoC A: HR_C7000 SoC (C-SKY CK803S + DMR modem) + two BK4819 (receiver / transmitter and the
# superhet LO) + a 320x240 ILI9341-class panel on the i8080 LCD controller (docs/spec/hardware-gd88.md). The PC link is
# UART1; SoC B, the second receiver, is on UART3 (boards/gd88b). Code both SoCs share is in boards/gd88/common
PLATFORM   := hrc7000
# The IAP loader leaves the PLL at 72 MHz; RadioFoundry re-programs it to 168 MHz CPU / 84 MHz APB at start-up and puts the
# loader's clocks back before it hands over to the runtime stub or resets (platform/hrc7000/clock.c), as on the other
# radios with a first stage (boot/README.md)
MCU_DEFS   := -DHRC_HOST_UART=1 -DHRC_LOADER_CPU_HZ=72000000u
BOARD_DIRS := boards/gd88 boards/gd88/common drivers/rf/bk4819 drivers/rf/at1141 drivers/rf/subaudio drivers/display/ili9341 drivers/input/keymatrix \
	drivers/input/quadrature drivers/input/sharedptt drivers/flash/spi_nand drivers/audio/afsk_tonegen drivers/modem/hrc7000 \
	drivers/bcast/rda5807m drivers/bus/i2c_gpio
# The link to SoC B (core/rx2): the main side runs here
BOARD_CORE_DIRS := core/rx2
# The core's storage on the NAND, with CONFIG_NAND_STORAGE
BOARD_NAND_DIRS := drivers/flash/nand_sectors
# The 320x240 panel at full resolution in 16 colors (display_gd88.c): the frame buffer is 37.5 KB with 240 rows
MCU_DEFS   += -DGFX_INDEXED=1 -DGFX_FB_BYTES=38400u -DGFX_MAX_ROWS=240u
# The image starts where the IAP loader jumps (hardware-gd88.md 2) with the first stage (boot/gd88): the stub window
# on the PC cable at power-on, then the application right after it. It starts with the entry word the IAP loader
# checks for
APP_BASE   := 0x0300C000
BOOT_STAGE := gd88
BOOT_SIZE  := 0x1000
BOOT_DEFS  := -DBOOT_WINDOW=10
BOOT_SIGNATURE := 2c1101c0
# Headers next to the board's own: the shared code's
BOARD_INC  := boards/gd88/common
