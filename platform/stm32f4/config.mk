# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# STM32F4 feature defaults, read after the board's config.mk (a board's ?= wins) and before config/features.mk.
# The fault trace needs a host UART: these radios' host port is USB, and their UART is taken (the GPS receive line)
CONFIG_FAULT_TRACE ?= n
# The 16x32 font is for panels 240 px and wider; these radios have 160 px panels
CONFIG_FONT_XL ?= n
# Link time optimization on our sources (about 17 KB smaller): the images no longer hold their features without it.
# LTO=0 builds the plain way
LTO ?= 1
# Not fitted to the platform yet, one switch per line with the reason (docs/build-config.md, "Unsupported, not
# written, unconfirmed"): ci's maximal configuration leaves these off
BOARD_NOT_WRITTEN += AIR_RX_ADAPT	# the soft modem's adaptive options: only on HR_C7000 so far (air-interface.md 6.1)
