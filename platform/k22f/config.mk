# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# K22F feature defaults, read after the board's config.mk (a board's ?= wins) and before config/features.mk.
# The hardware watchdog: the platform has no hal_watchdog yet (startup.c switches the WDOG off)
CONFIG_WATCHDOG ?= n
# What the platform's boards could do but no code does yet, one switch per line with the reason: ci's maximal
# configuration leaves these off
BOARD_NOT_WRITTEN += WATCHDOG	# the platform has no hal_watchdog yet (startup.c switches the WDOG off)
