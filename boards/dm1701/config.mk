# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
# DM-1701 feature defaults (config/features.mk lists them all). 336 KB of code space: flash sectors 3-6, the
# codec sectors 7, 8, 10 and 11 are never linked into (dm1701.ld).
# 160x120 display
CONFIG_FONT_XL ?= n
# The lens over the panel hides about 5 px at each edge (bench, 2026-10-09)
CONFIG_DISPLAY_INSET ?= 5
