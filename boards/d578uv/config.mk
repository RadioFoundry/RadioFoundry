# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# AT-D578UV feature defaults (config/features.mk lists them all). Bring-up state (docs/spec/hardware-d578uv.md 9): the
# display, keys, USB host link, FM on the RF chips and the GPS module are written from the stock firmware's analysis and
# untested on a radio. RAM is the limit: 96 KB, no CCM
# DMR runs through the SCT3258 baseband, which carries its own vocoder and plays the voice to the codec itself
# (hardware-d578uv.md 4, CAP_MODEM_VOICE). Its driver (drivers/modem/sct), the voice set-up and the RF side (receive,
# the transmit keying and the PA per burst) are written
CONFIG_DMR ?= y
# What needs the voice frames on the MCU is not written (below); with DMR on, what needs RAM this radio doesn't have
# is off: the USB network adapter and the IP gateway behind it take about 6 KB, zone sharing about 4.7 KB
CONFIG_HOTSPOT ?= n
CONFIG_SFR ?= n
CONFIG_DMR_PRIVACY ?= n
ifeq ($(CONFIG_DMR),y)
CONFIG_USB_NETWORK ?= n
CONFIG_ZONE_SHARE ?= n
endif
# 160x128 like the other 160x128 radios
CONFIG_FONT_XL ?= n
# The core's storage in RadioFoundry's own NAND block range (hardware-d578uv.md 6.3)
CONFIG_NAND_STORAGE ?= y
# PTT and keys from the Bluetooth module on UART4 (PC10 / PC11; hardware-d578uv.md 3.7). Found by asking it at start,
# so a radio without the module (not every model has it) only shows it absent
CONFIG_BT_PTT ?= y
# Air band AM and broadcast FM receive run on the KTMicro receiver on PB6/PB7, AM behind side B's chips
# (hardware-d578uv.md 5.4)
# APRS receive samples the receive audio on PC0 (afsk_d578uv.c, hardware-d578uv.md 3.4). It doesn't fit with DMR on:
# it takes about 11.6 KB (the demodulator, the station list and the 2 KB sample ring) and about 6.4 KB is free. With
# DMR off it fits with about 3.9 KB to spare, so it stays off there too unless asked for
CONFIG_APRS_RX ?= n
# USRP needs PCM both ways (below)
CONFIG_NET_USRP ?= n
# What the radio's hardware can't do, one switch per line with the reason (tools/boards.py checks there is one;
# docs/build-config.md, "Unsupported, not written, unconfirmed")
BOARD_UNSUPPORTED += FONT_XL	# the 160x128 panel is too small for the large font (it is for 240 px and wider)
BOARD_UNSUPPORTED += VIBRATE	# no vibration motor (a mobile)
# Left out of tools/ci.sh's maximal configuration: what doesn't fit the RAM with DMR on (above). The USB network adapter
# and everything that runs on it (about 30 KB together), and zone sharing (about 6 KB there)
MAXIMAL_SKIP := USB_NETWORK NET_PPP DMR_IP_GATEWAY NET_TCP NET_KISS NET_HTTP NET_HTTP_GZIP NET_WEBCTL NET_RIGCTL \
	NET_APRSIS MDNS TFTP TFTP_IDDB SNMP DMR_CONTROL_STATION ZONE_SHARE APRS_RX APRS_IGATE
# What the hardware can do but this radio's driver or glue doesn't yet
BOARD_NOT_WRITTEN += HOTSPOT	# needs the voice frames on the MCU: the baseband's frame bypass isn't written
BOARD_NOT_WRITTEN += HOTSPOT_SPLIT	# needs the hotspot (above)
BOARD_NOT_WRITTEN += SFR	# needs the voice frames on the MCU (above)
BOARD_NOT_WRITTEN += SFR_DUALPATH_RX	# needs SFR (above)
BOARD_NOT_WRITTEN += SFR_CWID	# needs SFR (above)
BOARD_NOT_WRITTEN += DMR_PRIVACY	# scrambles the voice frames on the MCU (above)
BOARD_NOT_WRITTEN += AUDIO_PROBE	# the probe taps an audio chip's PCM path; the voice stays in the baseband here
BOARD_NOT_WRITTEN += NET_USRP	# needs an audio tap (PCM both ways): the voice stays in the baseband, only receive audio reaches the MCU
BOARD_REQUIRED := FM
