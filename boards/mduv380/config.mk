# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# MD-UV380 feature defaults (config/features.mk lists them all). 720 KB for code and read-only data: flash sectors 3-9;
# the codec sectors 10 and 11 are never linked into (boards/mduv380/memory.ld, docs/spec/codec-placement.md).
# 160x128 display, every row visible. The fault trace, the large font and LTO follow the platform
# (platform/stm32f4/config.mk)
# Safe area: the TYT housing's edge crowds the top row and the two left columns; unconfirmed on this radio
# (hardware-mduv380.md 4)
CONFIG_DISPLAY_INSET ?= 0
CONFIG_DISPLAY_INSET_TOP ?= 1
CONFIG_DISPLAY_INSET_LEFT ?= 2
# APRS receive takes the HR_C6000's FM receive audio from the I2S stream (docs/spec/aprs.md 5.3). The iGate (RF to
# APRS-IS through tools/aprs_gate.py, 1.9 KB) is off by default
CONFIG_APRS_IGATE ?= n
MAXIMAL_VARS := LTO=1
# Left out of tools/ci.sh's maximal configuration: PPP stands in for the USB network adapter on UART-only radios
# (docs/spec/ppp.md), and this one has the adapter; the APRS iGate is off by default (above)
MAXIMAL_SKIP := NET_PPP APRS_IGATE
# What the radio's hardware can't do, one switch per line with the reason (tools/boards.py checks there is one;
# docs/build-config.md, "Unsupported, not written, unconfirmed"): ci's maximal configuration leaves these off
BOARD_UNSUPPORTED += FONT_XL	# the 160x128 panel is too small for the large font (it is for 240 px and wider)
CONFIG_AM_RX ?= n
BOARD_UNSUPPORTED += AM_RX	# no air band receiver: the AT1846S is FM only (docs/spec/airband-and-broadcast.md)
CONFIG_BCAST_FM ?= n
BOARD_UNSUPPORTED += BCAST_FM	# no broadcast receiver (docs/spec/airband-and-broadcast.md)
BOARD_UNSUPPORTED += NAND_STORAGE	# the data flash is a 16 MB SPI NOR (docs/spec/hardware-mduv380.md, the overview)
BOARD_UNSUPPORTED += VIBRATE	# no vibration motor (docs/user/23-radios.md, What the MD-UV380 lacks)
BOARD_UNSUPPORTED += FAULT_TRACE	# needs a host UART: the host port is USB, and the one UART is the G models' GPS line
# What the hardware can do but this radio's driver or glue doesn't yet (docs/build-config.md, "Unsupported, not
# written, unconfirmed"): off by default, and off in ci's maximal configuration too
CONFIG_REF_TRIM ?= n
BOARD_NOT_WRITTEN += REF_TRIM	# no hal_radio_t.ref_trim yet: it would add to the calibrated HR_C6000 MOD1 offset
