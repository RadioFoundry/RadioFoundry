# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# This board's host tests, included by tests/host/Makefile and run from there. HERE is this directory, read while the
# file is included; recipes use $< (the test, first) and $^ instead, which keep the paths they were given
HERE := $(dir $(lastword $(MAKEFILE_LIST)))

# The factory calibration sector (hardware-rt10.md 4)
TESTS   += rt10CalTest
rt10CalTest: $(HERE)rt10CalTest.c $(HERE)../calibration_rt10.c $(HERE)../rt10.h $(ROOT)/platform/hrc7000/dmr_rfburst_timing.h
	$(CC) $(CFLAGS) -I$(ROOT)/hal -I$(ROOT)/platform/hrc7000 -I$(dir $<).. -Wshadow -o $@ $(filter %.c,$^)

run-boards:: rt10CalTest
	./rt10CalTest
