# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
# MD-380 feature defaults (config/features.mk lists them all). 464 KB for code and read-only data: flash sectors 3-7;
# the codec sectors 8-11 are never linked into (boards/md380/memory.ld, docs/spec/codec-placement.md).
# 160x128 display. The case edge crowds the top row and the two left columns (seen on the radio, 2026-10-16);
# right and bottom are close but usable (hardware-md380.md 7). The fault trace, the large font and LTO follow the
# platform (platform/stm32f4/config.mk)
CONFIG_DISPLAY_INSET ?= 0
CONFIG_DISPLAY_INSET_TOP ?= 1
CONFIG_DISPLAY_INSET_LEFT ?= 2
# The FM TX tones (roger beep, 1750 Hz burst) and APRS AFSK need a TX tone path through the HR_C5000 or the PC7 PWM:
# not written
CONFIG_FM_TX_TONES ?= n
CONFIG_APRS_RX ?= n
# The SFR and split duplex glue is HR_C6000 only (docs/spec/sfr.md: the HR_C5000 lacks CRC_MCU_Control)
CONFIG_SFR ?= n
CONFIG_HOTSPOT_SPLIT ?= n
# Link time optimization takes the third-party code along (FreeRTOS marks what its port's assembly reaches
# portDONT_DISCARD; the vectors are referenced from the assembly start-up). The image needed it while the application
# had 275 KB, before the codec moved; it stays on for the smaller image
LTO_3P ?= 1
# What the board lacks or can't do yet, one switch per line with the reason (tools/boards.py checks there is
# one): ci's maximal configuration leaves these off
BOARD_UNSUPPORTED += FM_TX_TONES	# needs a TX tone path (the HR_C5000 or the PC7 PWM): not written (above)
BOARD_UNSUPPORTED += APRS_RX	# APRS AFSK needs the TX tone path, which isn't written (above)
BOARD_UNSUPPORTED += APRS_IGATE	# needs APRS receive, which isn't written for this radio
BOARD_UNSUPPORTED += SFR	# the glue is HR_C6000 only: the HR_C5000 lacks CRC_MCU_Control (docs/spec/sfr.md)
BOARD_UNSUPPORTED += HOTSPOT_SPLIT	# the split duplex glue is HR_C6000 only (above)
BOARD_UNSUPPORTED += FONT_XL	# the 160x128 panel is too small for the large font (it is for 240 px and wider)
BOARD_UNSUPPORTED += AM_RX	# no air band receiver (docs/spec/airband-and-broadcast.md)
BOARD_UNSUPPORTED += BCAST_FM	# no broadcast receiver (docs/spec/airband-and-broadcast.md)
BOARD_UNSUPPORTED += FAULT_TRACE	# needs a host UART: the host port is USB, the one UART the G models' GPS line
# Supported, but the maximal configuration leaves these off too: PPP stands in for the USB network adapter on UART-only
# radios (docs/spec/ppp.md), and this one has the adapter. NAND storage isn't used on this radio
MAXIMAL_SKIP := NET_PPP NAND_STORAGE
