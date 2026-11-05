# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# RT10 feature defaults (config/features.mk lists them all). The host link runs on the accessory jack's UART0; USB, PPP
# and TCP_CONNS follow the platform (platform/hrc7000/config.mk)
CONFIG_DMR ?= n
CONFIG_VOX ?= n
CONFIG_FONT_XL ?= n
CONFIG_GPS ?= n
CONFIG_GPS_SKY ?= n
CONFIG_AM_RX ?= n
CONFIG_BCAST_FM ?= n
# APRS needs an AFSK path into the FD6818's transmitter and its receive audio
CONFIG_APRS ?= n
CONFIG_APRS_RX ?= n
CONFIG_APRS_IGATE ?= n
BOARD_UNSUPPORTED += FONT_XL	# no display
BOARD_UNSUPPORTED += NAND_STORAGE	# the data flash is SPI NOR, GD25Q16 class (docs/spec/hardware-rt10.md 4)
BOARD_UNSUPPORTED += VIBRATE	# no vibration motor
BOARD_UNSUPPORTED += AM_RX	# no air band receive path
BOARD_UNSUPPORTED += BCAST_FM	# no broadcast receiver
BOARD_UNSUPPORTED += GPS	# no GPS module
BOARD_UNSUPPORTED += GPS_SKY	# no GPS module
BOARD_NOT_WRITTEN += DMR	# the modem glue, the FD6818's digital set and the codec map (rt10-112) aren't written yet
BOARD_NOT_WRITTEN += APRS	# no board.afsk: the FD6818 takes the microphone itself, not the modem's audio
BOARD_NOT_WRITTEN += APRS_RX	# the same
BOARD_NOT_WRITTEN += APRS_IGATE	# needs APRS receive
BOARD_NOT_WRITTEN += VOX	# no hal_power_t.mic_level hook yet
BOARD_NOT_WRITTEN += AUDIO_PROBE	# no board.audio_tap yet
BOARD_REQUIRED := FM
