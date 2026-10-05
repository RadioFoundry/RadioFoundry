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
# Link time optimization stays off in the image until it has been tested on the radio (LTO=1 to try it: about 17 KB
# smaller); tools/ci.sh's maximal configuration (every switch on) only fits with it
MAXIMAL_VARS := LTO=1
# Supported but not needed here, so tools/ci.sh's maximal configuration leaves it off: PPP stands in for the USB
# network adapter on UART-only radios (docs/spec/ppp.md), and this one has the adapter
MAXIMAL_SKIP := NET_PPP
