# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# AT-D578UV feature defaults, Artery builds (config/features.mk lists them all). The radio's code is boards/d578uv's,
# and so are these switches and reasons, kept in step with boards/d578uv/config.mk (tools/boards.py reads each board's
# own file). Bring-up state (docs/spec/hardware-d578uv.md 9, 10): nothing has run on an Artery radio yet. The image
# gets 128 KB of SRAM (memory.ld), no CCM
# DMR runs through the SCT3258 baseband, which carries its own vocoder and plays the voice to the codec itself
# (hardware-d578uv.md 4, CAP_MODEM_VOICE). Its driver (drivers/modem/sct), the voice set-up and the RF side (receive,
# the transmit keying and the PA per burst) are written
CONFIG_DMR ?= y
# What needs the voice frames on the MCU is not written (below)
CONFIG_HOTSPOT ?= n
CONFIG_SFR ?= n
CONFIG_DMR_PRIVACY ?= n
# RAM decides what runs on the USB network adapter beside DMR and APRS receive. The IP gateway (about 8.8 KB with the
# adapter), mDNS and TFTP (0.6 KB), the control station mode and SNMP (3.4 KB) fit, unlike on the GD32 builds, and so
# does zone sharing (about 4.7 KB). The TCP services don't: TCP alone takes about 4.7 KB, and the HTTP server, the
# web control, rigctld, KISS and APRS-IS run on it
CONFIG_NET_TCP ?= n
# USRP needs PCM both ways (below)
CONFIG_NET_USRP ?= n
# 160x128 like the other 160x128 radios
CONFIG_FONT_XL ?= n
# The core's storage in RadioFoundry's own NAND block range (hardware-d578uv.md 6.3)
CONFIG_NAND_STORAGE ?= y
# Air band AM and broadcast FM receive run on the KTMicro receiver on PB6/PB7, AM behind side B's chips
# (hardware-d578uv.md 5.4). APRS receive samples the receive audio on PC0 (boards/d578uv/afsk_d578uv.c,
# hardware-d578uv.md 3.4). Off until written: VOX (BOARD_NOT_WRITTEN below)
CONFIG_VOX ?= n
# What the radio's hardware can't do, one switch per line with the reason (tools/boards.py checks there is one;
# docs/build-config.md, "Unsupported, not written, unconfirmed")
BOARD_UNSUPPORTED += FONT_XL	# the 160x128 panel is too small for the large font (it is for 240 px and wider)
BOARD_UNSUPPORTED += VIBRATE	# no vibration motor (a mobile)
# Left out of tools/ci.sh's maximal configuration: what doesn't fit the RAM with DMR on (above), TCP and the services
# on it; and PPP, which stands in for the USB network adapter on radios without one
MAXIMAL_SKIP := NET_PPP NET_TCP NET_KISS NET_HTTP NET_HTTP_GZIP NET_WEBCTL NET_RIGCTL NET_APRSIS
# What the hardware can do but this radio's driver or glue doesn't yet
BOARD_NOT_WRITTEN += HOTSPOT	# needs the voice frames on the MCU: the baseband's frame bypass isn't written
BOARD_NOT_WRITTEN += HOTSPOT_SPLIT	# needs the hotspot (above)
BOARD_NOT_WRITTEN += SFR	# needs the voice frames on the MCU (above)
BOARD_NOT_WRITTEN += SFR_DUALPATH_RX	# needs SFR (above)
BOARD_NOT_WRITTEN += SFR_CWID	# needs SFR (above)
BOARD_NOT_WRITTEN += DMR_PRIVACY	# scrambles the voice frames on the MCU (above)
BOARD_NOT_WRITTEN += VOX	# no microphone level reaches the MCU (no ADC input); the RF chips' own detector isn't known
BOARD_NOT_WRITTEN += AUDIO_PROBE	# the probe taps an audio chip's PCM path; the voice stays in the baseband here
BOARD_NOT_WRITTEN += NET_USRP	# needs an audio tap (PCM both ways): the voice stays in the baseband, only receive audio reaches the MCU
BOARD_REQUIRED := FM
