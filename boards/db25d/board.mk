# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
# Radioddity DB25-D PRO (mobile): HR_C7000 SoC (C-SKY CK803S + DMR modem) + two BK4819 (receiver / transmitter and the
# superhet LO) + ST7735S 160x128 on a bit-banged serial bus (docs/spec/hardware-db25d.md). The PC link is UART1
PLATFORM   := hrc7000
# The IAP loader leaves the PLL at 72 MHz (hardware-db25d.md 2). With DB25D_FULL_CLOCK=1 (default) ODR re-programs it to
# the DM-32's 168 MHz CPU / 84 MHz APB at start-up (platform/hrc7000/clock.c) and puts the loader's clocks back before
# it hands over to the runtime stub (built for them) or resets. DB25D_FULL_CLOCK=0 keeps the loader's 72 / 36 MHz.
# The runtime stub's RAM window (HRC_STUB_BASE) and, with DMR, the linker script and the codec host are set in config.mk:
# the stub gets 128 KB above the bss without DMR (db25d.ld keeps it free), and the stock codec's RAM window with DMR
# (db25d_dmr.ld)
DB25D_FULL_CLOCK ?= 1
MCU_DEFS   := -DHRC_HOST_UART=1 -DHRC_LOADER_CPU_HZ=72000000u
ifeq ($(DB25D_FULL_CLOCK),1)
MCU_DEFS   += -DHRC_CPU_HZ=168000000u
else
MCU_DEFS   += -DHRC_CPU_HZ=72000000u
endif
LDSCRIPT   := boards/db25d/db25d.ld
BOARD_DIRS := boards/db25d drivers/rf/bk4819 drivers/display/st7735 drivers/input/keymatrix drivers/input/quadrature drivers/input/sharedptt drivers/input/serialmic \
	drivers/flash/spi_nand drivers/modem/hrc7000
# The core's storage on the NAND, with CONFIG_NAND_STORAGE (hardware-db25d.md 6.6)
BOARD_NAND_DIRS := drivers/flash/nand_sectors
# The first stage at 0x0300C000 (boot/db25d): the stub window on the PC cable at power-on, then the application at
# 0x0300D000 (db25d.ld). It starts with the stock entry the IAP loader checks for
BOOT_STAGE := db25d
BOOT_SIZE  := 0x1000
BOOT_DEFS  := -DBOOT_WINDOW=10 -DAPP_RESUME=0x0300D000
BOOT_SIGNATURE := 2c1101c0
