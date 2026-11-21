# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# USRP's 8 kHz 16-bit PCM (128 kbit/s plus headers) doesn't fit the 115200 baud host port, the radio's only IP link
# (PPP, ppp.md 6): on the bench 81 of 180 frames of a call got out
CONFIG_NET_USRP ?= n
include boards/dm32/config.mk
CONFIG_VIBRATE ?= n
BOARD_NOT_WRITTEN += VIBRATE # the motor is held off at boot
CONFIG_GPS ?= n
BOARD_UNSUPPORTED += GPS # no GPS module in the photos or manual
BOARD_UNSUPPORTED += GPS_SKY # no GPS receiver
CONFIG_DMR_CONTROL_STATION ?= n
BOARD_UNSUPPORTED += DMR_CONTROL_STATION # control station mode is the USB network adapter's; no USB data port
BOARD_UNSUPPORTED += NET_USRP # USRP needs 155 kbit/s (mu-law 91); the 115200 baud PPP link carried 72 (hardware-dm36.md 3)
BOARD_UNSUPPORTED += NAND_STORAGE # 16 MiB SPI NOR data flash
BOARD_NOT_WRITTEN += VOX # the board has no microphone level hook
BOARD_NOT_WRITTEN += AUDIO_PROBE # the board has no audio tap hook
