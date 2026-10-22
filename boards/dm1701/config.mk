# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
# DM-1701 feature defaults (config/features.mk lists them all). 720 KB for code and read-only data: flash sectors 3-9;
# the codec sectors 10 and 11 are never linked into (boards/dm1701/memory.ld, docs/spec/codec-placement.md).
# 160x120 display. The fault trace, the large font and LTO follow the platform (platform/stm32f4/config.mk)
# Safe area: the lens hides about 5 px left and right (bench, 2026-10-09). Top and bottom are 0: the 5 px there were
# dead space on the radio (owner, 2026-10-11). Left and right stay 5 until checked on the radio the same way
CONFIG_DISPLAY_INSET ?= 5
CONFIG_DISPLAY_INSET_TOP ?= 0
CONFIG_DISPLAY_INSET_BOTTOM ?= 0
# APRS receive takes the HR_C6000's FM receive audio from the I2S stream (docs/spec/aprs.md 5.3). The iGate (RF to
# APRS-IS through tools/aprs_gate.py, 1.9 KB) is off by default
CONFIG_APRS_IGATE ?= n
MAXIMAL_VARS := LTO=1
# Left out of tools/ci.sh's maximal configuration: PPP stands in for the USB network adapter on UART-only radios
# (docs/spec/ppp.md), and this one has the adapter; the APRS iGate is off by default (above)
MAXIMAL_SKIP := NET_PPP APRS_IGATE
# What the board lacks or can't do yet, one switch per line with the reason (tools/boards.py checks there is
# one): ci's maximal configuration leaves these off
BOARD_UNSUPPORTED += FONT_XL	# the 160x120 panel is too small for the large font (it is for 240 px and wider)
BOARD_UNSUPPORTED += AM_RX	# no air band receiver: the AT1846S is FM only (docs/spec/airband-and-broadcast.md)
BOARD_UNSUPPORTED += BCAST_FM	# no broadcast receiver (docs/spec/airband-and-broadcast.md)
BOARD_UNSUPPORTED += NAND_STORAGE	# the data flash is a 16 MB SPI NOR (docs/spec/hardware-dm1701.md, the overview)
BOARD_UNSUPPORTED += VIBRATE	# no vibration motor (docs/user/23-radios.md, What the DM-1701 lacks)
BOARD_UNSUPPORTED += FAULT_TRACE	# needs a host UART: the host port is USB, and the one UART is the GPS line
CONFIG_GPS ?= n
BOARD_UNSUPPORTED += GPS	# no GPS receiver, and no room inside for one (owner, 2026-10-22)
