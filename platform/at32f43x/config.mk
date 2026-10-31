# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# AT32F43x feature defaults, read after the board's config.mk (a board's ?= wins) and before config/features.mk.
# The 16x32 font is for panels 240 px and wider
CONFIG_FONT_XL ?= n
# Link time optimization on our sources
LTO ?= 1
# The fault trace needs a host UART the fault handler can drive: these radios' host port is USB
CONFIG_FAULT_TRACE ?= n
