# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
# DM-1801 Pro feature defaults (config/features.mk lists them all). Bring-up state (docs/spec/hardware-dm1801pro.md 12):
# the UI, keys, data flash, battery, analog FM, DMR (voice with the radio's own codec merged into the slot,
# docs/spec/codec-dm1801pro.md) and the broadcast tuner. Transmit needs the calibration to be found, so it is refused
# without it
# The host link runs on the programming cable's UART0; USB, PPP and TCP_CONNS follow the platform
# (platform/hrc7000/config.mk)
# The RDA5807-class broadcast tuner (CAP_BCAST_FM); the air band isn't wired yet
CONFIG_BCAST_FM ?= y
CONFIG_AM_RX ?= n
# The vibration motor on PTB24 (Settings > Sound > Vibrate)
CONFIG_VIBRATE ?= y
# 128x64 monochrome: no room for the large font, and no colors to set
CONFIG_FONT_XL ?= n
CONFIG_UI_COLOR ?= n
# What the board lacks or can't do yet, one switch per line with the reason (tools/boards.py checks there is
# one): ci's maximal configuration leaves these off. USB and USB_NETWORK are the platform's
BOARD_UNSUPPORTED += AM_RX	# the broadcast tuner's air band isn't wired yet
BOARD_UNSUPPORTED += AUDIO_PROBE	# a bench diagnostic for an audio path this radio doesn't have
BOARD_UNSUPPORTED += FONT_XL	# the 128x64 panel is too small for the large font
BOARD_UNSUPPORTED += UI_COLOR	# the 128x64 panel is monochrome: no colors to set
BOARD_UNSUPPORTED += NAND_STORAGE	# the data flash is SPI NOR: 4 KB erase (docs/spec/hardware-dm1801pro.md 6.1)
