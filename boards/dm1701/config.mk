# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
# DM-1701 feature defaults (config/features.mk lists them all). 418 KB of code space before the codec window.
# 160x120 display
CONFIG_FONT_XL ?= n
# The lens over the panel hides about 5 px at each edge (bench, 2026-10-09)
CONFIG_DISPLAY_INSET ?= 5
# No receive path for APRS: FM audio goes AT1846S -> HR_C6000 -> speaker, no MCU ADC sees it (docs/spec/aprs.md 5.3);
# the demodulator's buffers would only cost RAM
CONFIG_APRS_RX ?= n
