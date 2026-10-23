# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# MD-380 feature defaults (config/features.mk lists them all). 464 KB for code and read-only data: flash sectors 3-7;
# the codec sectors 8-11 are never linked into (boards/md380/memory.ld, docs/spec/codec-placement.md).
# 160x128 display. The case edge crowds the top row and the two left columns (seen on the radio, 2026-10-16);
# right and bottom are close but usable (hardware-md380.md 7). The fault trace, the large font and LTO follow the
# platform (platform/stm32f4/config.mk)
CONFIG_DISPLAY_INSET ?= 0
CONFIG_DISPLAY_INSET_TOP ?= 1
CONFIG_DISPLAY_INSET_LEFT ?= 2
# The FM TX tones (roger beep, 1750 Hz burst), DTMF and APRS AFSK need a TX tone path through the HR_C5000 or the PC7 PWM:
# not written
CONFIG_FM_TX_TONES ?= n
CONFIG_DTMF ?= n
CONFIG_APRS_RX ?= n
# SFR needs CRC_MCU_Control, which the HR_C5000 lacks (docs/spec/sfr.md); the split duplex glue is written for the
# HR_C6000 and HR_C7000 only
CONFIG_SFR ?= n
CONFIG_HOTSPOT_SPLIT ?= n
# Link time optimization takes the third-party code along (FreeRTOS marks what its port's assembly reaches
# portDONT_DISCARD; the vectors are referenced from the assembly start-up). The image needed it while the application
# had 275 KB, before the codec moved; it stays on for the smaller image
LTO_3P ?= 1
# What the radio's hardware can't do, one switch per line with the reason (tools/boards.py checks there is one;
# docs/build-config.md, "Unsupported, not written, unconfirmed"): ci's maximal configuration leaves these off
BOARD_UNSUPPORTED += SFR	# the HR_C5000 lacks CRC_MCU_Control for the idle bursts (docs/spec/sfr.md)
BOARD_UNSUPPORTED += FONT_XL	# the 160x128 panel is too small for the large font (it is for 240 px and wider)
BOARD_UNSUPPORTED += AM_RX	# no air band receiver (docs/spec/airband-and-broadcast.md)
BOARD_UNSUPPORTED += BCAST_FM	# no broadcast receiver (docs/spec/airband-and-broadcast.md)
BOARD_UNSUPPORTED += NAND_STORAGE	# the data flash is a SPI NOR (docs/spec/hardware-md380.md 12)
BOARD_UNSUPPORTED += VIBRATE	# no vibration motor (docs/user/23-radios.md, What the MD-380 lacks)
BOARD_UNSUPPORTED += FAULT_TRACE	# needs a host UART: the host port is USB, the one UART the G models' GPS line
# What the hardware can do but this radio's driver or glue doesn't yet, the same way: off by default, and off in ci's
# maximal configuration too
BOARD_NOT_WRITTEN += FM_TX_TONES	# the TX tone path (through the HR_C5000 or the PC7 PWM) isn't written (above)
BOARD_NOT_WRITTEN += DTMF	# the DTMF tones need the same TX tone path (above)
BOARD_NOT_WRITTEN += APRS_RX	# no APRS receive audio path yet (the HR_C5000's I2S receive audio isn't fed to it)
BOARD_NOT_WRITTEN += APRS_IGATE	# needs APRS receive (above)
BOARD_NOT_WRITTEN += HOTSPOT_SPLIT	# the split duplex glue is HR_C6000 and HR_C7000 only (docs/spec/split-duplex.md)
BOARD_NOT_WRITTEN += AUDIO_PROBE	# no board.audio_tap yet
# Supported, but the maximal configuration leaves these off too: PPP stands in for the USB network adapter on UART-only
# radios (docs/spec/ppp.md), and this one has the adapter
MAXIMAL_SKIP := NET_PPP
