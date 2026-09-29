# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
# DM-1701 feature defaults (config/features.mk lists them all). 336 KB of code space: flash sectors 3-6, the
# codec sectors 7, 8, 10 and 11 are never linked into (dm1701.ld).
# 160x120 display
CONFIG_FONT_XL ?= n
# The lens over the panel hides about 5 px at each edge (bench, 2026-10-09)
CONFIG_DISPLAY_INSET ?= 5
# No receive path for APRS: FM audio goes AT1846S -> HR_C6000 -> speaker, no MCU ADC sees it (docs/spec/aprs.md 5.3);
# the demodulator's buffers would only cost RAM
CONFIG_APRS_RX ?= n
# Link time optimization stays off in the image until it has been tested on the radio (LTO=1 to try it: about 20 KB
# smaller); tools/ci.sh's maximal configuration (every switch on) only fits with it
MAXIMAL_VARS := LTO=1
