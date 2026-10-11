# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
# Radioddity DB25-D PRO (mobile): HR_C7000 SoC (C-SKY CK803S + DMR modem) + two BK4819 (receiver / transmitter and the
# superhet LO) + ST7735S 160x128 on a bit-banged serial bus (docs/spec/hardware-db25d.md). The PC link is UART1
PLATFORM   := hrc7000
MCU_DEFS   := -DHRC_HOST_UART=1
LDSCRIPT   := boards/db25d/db25d.ld
BOARD_DIRS := boards/db25d drivers/rf/bk4819 drivers/display/st7735 drivers/input/keymatrix drivers/input/quadrature \
	drivers/flash/spi_nand
