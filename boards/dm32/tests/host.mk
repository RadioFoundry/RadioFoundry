# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# This board's host tests, included by tests/host/Makefile and run from there. HERE is this directory, read while the
# file is included; recipes use $< (the test, first) and $^ instead, which keep the paths they were given
HERE := $(dir $(lastword $(MAKEFILE_LIST)))

# The power levels on the radio's calibration rows (hardware-dm32.md 11.4)
TESTS   += dm32PowerTest
dm32PowerTest: $(HERE)dm32PowerTest.c $(HERE)../power_levels.h $(ROOT)/drivers/cal/baofeng/baofeng_cal.c \
		$(ROOT)/drivers/cal/baofeng/baofeng_cal.h $(ROOT)/core/radio/apc_ramp.h
	$(CC) $(CFLAGS) -I$(ROOT)/hal -I$(dir $<).. -Wshadow -o $@ $(filter %.c,$^)

run-boards:: dm32PowerTest
	./dm32PowerTest
