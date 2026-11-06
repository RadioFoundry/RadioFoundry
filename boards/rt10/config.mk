# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# RT10 feature defaults (config/features.mk lists them all). The host link runs on the accessory jack's UART0; USB, PPP
# and TCP_CONNS follow the platform (platform/hrc7000/config.mk)
# DMR (hardware-rt10.md 7.1): the stock scheme on the shared RF-burst task; voice needs the codec merged (8)
CONFIG_DMR ?= y
CONFIG_HOTSPOT_SPLIT ?= n
CONFIG_NET_USRP ?= n
CONFIG_VOX ?= n
CONFIG_FONT_XL ?= n
CONFIG_UI_COLOR ?= n
CONFIG_GPS ?= n
CONFIG_GPS_SKY ?= n
CONFIG_AM_RX ?= n
CONFIG_BCAST_FM ?= n
# APRS needs an AFSK path into the FD6818's transmitter and its receive audio
CONFIG_APRS ?= n
CONFIG_APRS_RX ?= n
CONFIG_APRS_IGATE ?= n
BOARD_UNSUPPORTED += FONT_XL	# no display
BOARD_UNSUPPORTED += UI_COLOR	# no display: no colors to set
BOARD_UNSUPPORTED += NAND_STORAGE	# the data flash is SPI NOR, GD25Q16 class (docs/spec/hardware-rt10.md 4)
BOARD_UNSUPPORTED += VIBRATE	# no vibration motor
BOARD_UNSUPPORTED += AM_RX	# no air band receive path
BOARD_UNSUPPORTED += BCAST_FM	# no broadcast receiver
BOARD_UNSUPPORTED += GPS	# no GPS module
BOARD_UNSUPPORTED += GPS_SKY	# no GPS module
BOARD_NOT_WRITTEN += APRS	# no board.afsk: the FD6818 takes the microphone itself, not the modem's audio
BOARD_NOT_WRITTEN += APRS_RX	# the same
BOARD_NOT_WRITTEN += APRS_IGATE	# needs APRS receive
BOARD_NOT_WRITTEN += VOX	# no hal_power_t.mic_level hook yet
BOARD_NOT_WRITTEN += AUDIO_PROBE	# no board.audio_tap yet
BOARD_NOT_WRITTEN += HOTSPOT_SPLIT	# the per-burst TX path has no continuous base-station outbound
BOARD_NOT_WRITTEN += NET_USRP	# USRP's 8 kHz PCM both ways needs more than the host link's 115200 baud
# DMR needs the codec merged for voice, so FM stays the one mode every configuration needs
BOARD_REQUIRED := FM
# Bench knobs (dmr_rt10.c, dmr_rt10.h): RT10_DMR_RF_MODE (the modem's RF_MODE word), RT10_DMR_MIC_DB (its ADC gain in
# DMR), RT10_DMR_DAC_C (the AF ADC's bias in DMR)
ifneq ($(RT10_DMR_DAC_C),)
MCU_DEFS += -DRT10_DMR_DAC_C=$(RT10_DMR_DAC_C)u
endif
ifneq ($(RT10_DMR_RF_MODE),)
MCU_DEFS += -DRT10_DMR_RF_MODE=$(RT10_DMR_RF_MODE)
endif
ifneq ($(RT10_DMR_MIC_DB),)
MCU_DEFS += -DRT10_DMR_MIC_DB=$(RT10_DMR_MIC_DB)u
endif
