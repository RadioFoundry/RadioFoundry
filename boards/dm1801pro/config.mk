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
# What the radio's hardware can't do, one switch per line with the reason (tools/boards.py checks there is one;
# docs/build-config.md, "Unsupported, not written, unconfirmed"): ci's maximal configuration leaves these off. USB and USB_NETWORK are the platform's
BOARD_UNSUPPORTED += FONT_XL	# the 128x64 panel is too small for the large font
BOARD_UNSUPPORTED += UI_COLOR	# the 128x64 panel is monochrome: no colors to set
BOARD_UNSUPPORTED += NAND_STORAGE	# the data flash is SPI NOR: 4 KB erase (docs/spec/hardware-dm1801pro.md 6.1)
CONFIG_GPS ?= n
BOARD_UNSUPPORTED += GPS	# no GPS receiver, and no room inside for one (owner, 2026-10-22)
# What the hardware can do but this radio's driver or glue doesn't yet, the same way: off by default, and off in ci's
# maximal configuration too
BOARD_NOT_WRITTEN += AM_RX	# the RF chip's 108-136 MHz window isn't used yet (docs/spec/hardware-dm1801pro.md 5)
BOARD_NOT_WRITTEN += AUDIO_PROBE	# the probe's audio tap (board.audio_tap) isn't written for this radio
