# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# GD-77 feature defaults (config/features.mk lists them all). Bring-up state (docs/spec/hardware-gd77.md 12): the UI on
# the 128x64 display, keys, power and FM receive. DMR: the HR_C6000 slot engine, the voice port (SPI1) and the PCM
# stream are wired; the codec is the radio's own, merged on the user's machine (docs/spec/codec-gd77.md)
# Transmit, FM and DMR: off until the PA path and the calibration are confirmed on a radio. With y the bands allow it
# and tx() keys only with a valid calibration point (docs/build-config.md, "Board build variables")
CONFIG_GD77_TX ?= n
MCU_DEFS += -DCONFIG_GD77_TX=$(if $(filter y,$(CONFIG_GD77_TX)),1,0)
# USB serial (CDC-ACM) with the host link, the debug link and the open-firmware CPS (docs/spec/hardware-gd77.md 13)
# What the radio's hardware can't do, one switch per line with the reason (tools/boards.py checks there is one;
# docs/build-config.md, "Unsupported, not written, unconfirmed"): off by default, and off in ci's maximal configuration
CONFIG_AM_RX ?= n
CONFIG_BCAST_FM ?= n
CONFIG_GPS ?= n
CONFIG_FAULT_TRACE ?= n
CONFIG_FONT_XL ?= n
CONFIG_UI_COLOR ?= n
BOARD_UNSUPPORTED += AM_RX	# no air band receiver: the AT1846S is FM only (docs/spec/airband-and-broadcast.md)
BOARD_UNSUPPORTED += BCAST_FM	# no broadcast receiver (docs/spec/hardware-gd77.md 1)
BOARD_UNSUPPORTED += GPS	# no GPS receiver (docs/spec/hardware-gd77.md 1)
BOARD_UNSUPPORTED += NAND_STORAGE	# the data flash is a SPI NOR (docs/spec/hardware-gd77.md 6.2)
BOARD_UNSUPPORTED += VIBRATE	# no vibration motor (docs/spec/hardware-gd77.md 3.1)
BOARD_UNSUPPORTED += FONT_XL	# the 128x64 panel is too small for the large font (it is for 240 px and wider)
BOARD_UNSUPPORTED += FONT_AA	# a 1 bit panel: no edge grays
BOARD_UNSUPPORTED += FONT_BIG	# the 128x64 panel is too small for the big font
BOARD_UNSUPPORTED += UI_COLOR	# monochrome panel
BOARD_UNSUPPORTED += FAULT_TRACE	# needs a host UART: the host port is USB only
# What the hardware can do but this radio's driver or glue doesn't yet, the same way. The watchdog is the platform's
# (platform/k22f/config.mk)
CONFIG_HOTSPOT ?= n
CONFIG_CRASH_REPORT ?= n
CONFIG_ZONE_SHARE ?= n
CONFIG_USB_NETWORK ?= n
CONFIG_APRS ?= n
CONFIG_DTMF ?= n
CONFIG_FM_TX_TONES ?= n
CONFIG_VOX ?= n
BOARD_NOT_WRITTEN += HOTSPOT	# with the CPS on, its buffers overflow the 64 KB SRAM_L; nothing moves them to SRAM_U yet
BOARD_NOT_WRITTEN += HOTSPOT_SPLIT	# needs the hotspot (above)
BOARD_NOT_WRITTEN += CRASH_REPORT	# its buffers overflow the 64 KB SRAM_L, as the hotspot's (above)
BOARD_NOT_WRITTEN += ZONE_SHARE	# its air queue and fragment buffers overflow the 64 KB SRAM_L (4 KB over with it on); nothing moves them to SRAM_U yet
BOARD_NOT_WRITTEN += USB_NETWORK	# the network adapter personality (CDC-NCM) isn't brought up on this radio's USB
BOARD_NOT_WRITTEN += NET_PPP	# the IP stack isn't brought up on this radio
BOARD_NOT_WRITTEN += DMR_IP_GATEWAY	# rides the USB network adapter (above)
BOARD_NOT_WRITTEN += DMR_CONTROL_STATION	# needs the IP gateway (above)
BOARD_NOT_WRITTEN += NET_TCP	# a service of the USB network adapter (above)
BOARD_NOT_WRITTEN += NET_KISS	# a service of the USB network adapter (above)
BOARD_NOT_WRITTEN += NET_HTTP	# a service of the USB network adapter (above)
BOARD_NOT_WRITTEN += NET_HTTP_GZIP	# a service of the USB network adapter (above)
BOARD_NOT_WRITTEN += NET_RIGCTL	# a service of the USB network adapter (above)
BOARD_NOT_WRITTEN += NET_APRSIS	# a service of the USB network adapter (above)
BOARD_NOT_WRITTEN += NET_USRP	# a service of the USB network adapter (above)
BOARD_NOT_WRITTEN += MDNS	# a service of the USB network adapter (above)
BOARD_NOT_WRITTEN += TFTP	# a service of the USB network adapter (above)
BOARD_NOT_WRITTEN += TFTP_IDDB	# a service of the USB network adapter (above)
BOARD_NOT_WRITTEN += SNMP	# a service of the USB network adapter (above)
BOARD_NOT_WRITTEN += APRS	# board.afsk (AFSK through the FM transmitter) isn't written for this radio
BOARD_NOT_WRITTEN += APRS_RX	# needs APRS (above)
BOARD_NOT_WRITTEN += APRS_IGATE	# needs APRS receive (above)
BOARD_NOT_WRITTEN += DTMF	# board.radio->tx_dtmf isn't written for this radio
BOARD_NOT_WRITTEN += FM_TX_TONES	# board.radio->tx_tone isn't written for this radio
BOARD_NOT_WRITTEN += VOX	# no hal_power_t.mic_level hook yet
BOARD_NOT_WRITTEN += AUDIO_PROBE	# the probe's audio tap (board.audio_tap) isn't written for this radio
# FM is the one mode this build has
BOARD_REQUIRED := FM
