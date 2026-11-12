# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# DM-1701 feature defaults (config/features.mk lists them all). 336 KB for code and read-only data: flash sectors 3-9,
# 0x0800C000 to 0x080BFFFF; the codec sectors 10 and 11 are never linked into (boards/dm1701/memory.ld,
# docs/spec/codec-placement.md). docs/feature-matrix.md has the image's size
# 160x120 display. The fault trace, the large font and LTO follow the platform (platform/stm32f4/config.mk)
# Safe area: the lens hides about 5 px left and right (bench, 2026-10-09). Top and bottom are 0: the 5 px there were
# dead space on the radio (owner, 2026-10-11). Left and right stay 5 until checked on the radio the same way
CONFIG_DISPLAY_INSET ?= 5
CONFIG_DISPLAY_INSET_TOP ?= 0
CONFIG_DISPLAY_INSET_BOTTOM ?= 0
# APRS receive takes the HR_C6000's FM receive audio from the I2S stream (docs/spec/aprs.md 5.3). The iGate (RF to
# APRS-IS through tools/aprs_gate.py, 1.9 KB) is off by default
CONFIG_APRS_IGATE ?= n
MAXIMAL_VARS := LTO=1
# Left out of tools/ci.sh's maximal configuration: PPP stands in for the USB network adapter on UART-only radios
# (docs/spec/ppp.md), and this one has the adapter; the APRS iGate is off by default (above). Chinese is left out too:
# every switch on doesn't fit the 336 KB region (below) with two translations, so maximal keeps Spanish, as the default
# build does, and Chinese builds when it is asked for alone (docs/feature-matrix.md)
MAXIMAL_SKIP := NET_PPP APRS_IGATE LANG_ZH LANG_DEFAULT_ZH
# What the radio's hardware can't do, one switch per line with the reason (tools/boards.py checks there is one;
# docs/build-config.md, "Unsupported, not written, unconfirmed"): ci's maximal configuration leaves these off
BOARD_UNSUPPORTED += FONT_XL	# the 160x120 panel is too small for the large font (it is for 240 px and wider)
CONFIG_AM_RX ?= n
BOARD_UNSUPPORTED += AM_RX	# no air band receiver: the AT1846S is FM only (docs/spec/airband-and-broadcast.md)
CONFIG_BCAST_FM ?= n
BOARD_UNSUPPORTED += BCAST_FM	# no broadcast receiver (docs/spec/airband-and-broadcast.md)
BOARD_UNSUPPORTED += NAND_STORAGE	# the data flash is a 16 MB SPI NOR (docs/spec/hardware-dm1701.md, the overview)
BOARD_UNSUPPORTED += VIBRATE	# no vibration motor (docs/user/23-radios.md, What the DM-1701 lacks)
BOARD_UNSUPPORTED += FAULT_TRACE	# needs a host UART: the host port is USB, and the one UART is the GPS line
# GPS stays on: no module from the factory, but owners fit one on PA10 / PA9 (hardware-dm1701.md 14.2). Detected at run
# time in the background: a radio without one stops listening after 5 s of silence

# Chinese (CONFIG_LANG_ZH, docs/build-config.md): a line of it needs 16 px, and this class draws its words in the 6x12
# font, so core/ui/ui_screens.c moves lists, labels, the strip, the hints and the text pages up to the 8x16 font while a
# wide language is in use; Home then has no room for the zone line and a list shows 5 rows instead of 7 on the 160x120
# panel (docs/spec/ui.md 2.5). The language is off by default like on every radio, and the region (below) holds it only
# in Spanish's place: Chinese needs 32,856 B on top of the lookup Spanish already brings (docs/feature-matrix.md) while
# Spanish itself is 28,456 B, so an image with both is about 20 KB over the region; with Spanish off, Chinese is 11,900 B
# more than the default image, which is 368 B over one measurement of it and 1,340 B under the other. Turn the debug
# link off (32,376 B, tools and HIL only) unless the linker says it fits
#
#	make BOARD=dm1701 CONFIG_LANG_ZH=y CONFIG_LANG_DEFAULT_ZH=y CONFIG_LANG_ES=n
#
# board.toml's "chinese" ci extra builds that with CONFIG_DEBUGLINK=n, which is the image with a margin. The web flasher
# offers Chinese to a browser that asks for it (docs/user/02-installing.md); the build then reports that it doesn't fit,
# and Spanish comes off in Custom (docs/user/23-radios.md, this radio's language note)
