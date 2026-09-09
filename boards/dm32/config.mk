# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
# DM-32UV feature defaults (config/features.mk lists them all). 912 KB of code space before the codec slot.
# No USB on the radio: the host link (CPS, hotspot, debug link) runs on the programming cable's UART0
CONFIG_USB ?= n
CONFIG_HOSTLINK ?= y
# The 240x320 panel is driven at half resolution (120x160 logical, every pixel doubled): the core's frame buffer
# is 40 KB (core/ui/gfx.h), so CONFIG_FONT_XL buys nothing yet
CONFIG_FONT_XL ?= n
CONFIG_DISPLAY_INSET ?= 0
# Air band AM receive and the RDA5807M broadcast FM receiver (docs/spec/airband-and-broadcast.md, parked): the core
# is built and host tested, the board glue isn't written yet (no CAP_AM_RX / CAP_BCAST_FM), so nothing runs yet
CONFIG_AM_RX ?= $(CONFIG_FM)
CONFIG_BCAST_FM ?= y
