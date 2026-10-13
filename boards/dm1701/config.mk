# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
# DM-1701 feature defaults (config/features.mk lists them all). 336 KB of code space: flash sectors 3-6, the
# codec sectors 7, 8, 10 and 11 are never linked into (dm1701.ld).
# 160x120 display
CONFIG_FONT_XL ?= n
# Safe area: the lens hides about 5 px left and right (bench, 2026-10-09). Top and bottom are 0: the 5 px there were
# dead space on the radio (owner, 2026-10-11). Left and right stay 5 until checked on the radio the same way
CONFIG_DISPLAY_INSET ?= 5
CONFIG_DISPLAY_INSET_TOP ?= 0
CONFIG_DISPLAY_INSET_BOTTOM ?= 0
# No receive path for APRS: FM audio goes AT1846S -> HR_C6000 -> speaker, no MCU ADC sees it (docs/spec/aprs.md 5.3);
# the demodulator's buffers would only cost RAM
CONFIG_APRS_RX ?= n
# Link time optimization on our sources (about 17 KB smaller): the default image no longer fits without it since USRP
# (CONFIG_NET_USRP, 2026-10-13). LTO=0 builds the plain way (and then needs features off). The free sector 9 (FLASH2,
# dm1701.ld) is the other way to make room, once the packaging and flashing tools write it
LTO ?= 1
MAXIMAL_VARS := LTO=1
# Supported but not needed here, so tools/ci.sh's maximal configuration leaves it off: PPP stands in for the USB
# network adapter on UART-only radios (docs/spec/ppp.md), and this one has the adapter
MAXIMAL_SKIP := NET_PPP
# No use on a 160x120 panel (the 16x32 font is for 240 px and wider), and no hardware for the air band (the AT1846S is
# FM only) or a broadcast receiver (docs/spec/airband-and-broadcast.md), so the maximal configuration leaves them off
BOARD_UNSUPPORTED := FONT_XL AM_RX BCAST_FM
