# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# K22F feature defaults, read after the board's config.mk (a board's ?= wins) and before config/features.mk.
# The hardware watchdog: the platform has no hal_watchdog yet (startup.c switches the WDOG off)
CONFIG_WATCHDOG ?= n
# What the platform's boards could do but no code does yet, one switch per line with the reason: ci's maximal
# configuration leaves these off
BOARD_NOT_WRITTEN += WATCHDOG	# the platform has no hal_watchdog yet (startup.c switches the WDOG off)
# Link time optimization on our sources and the third-party code (about 30 KB smaller): the image no longer
# holds its features without it. LTO=0 builds the plain way. Two files stay out of it: the codec call veneer (assembly
# at file scope) and the start-up code, whose reset handler loads the stack top from a literal pool that the
# assembler places at the end of its unit, out of the load's reach in a merged LTO partition
LTO ?= 1
LTO_3P ?= 1
LTO_EXCLUDE ?= platform/k22f/codec_call.c platform/k22f/startup.c
