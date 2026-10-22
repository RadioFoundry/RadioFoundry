# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
# This board's host tests, included by tests/host/Makefile and run from there. HERE is this directory, read while the
# file is included; recipes use $< (the test, first) and $^ instead, which keep the paths they were given
HERE := $(dir $(lastword $(MAKEFILE_LIST)))

# The calibration reader on a fake flash image
TESTS   += calibrationTest
calibrationTest: $(HERE)calibrationTest.c $(HERE)../calibration_dm1701.c $(HERE)../dm1701.h $(CORE)/radio/cal_curve.h
	$(CC) $(CFLAGS) -I$(ROOT)/hal -Wshadow -I$(dir $<).. -o $@ $(filter %.c,$^)

run-boards:: calibrationTest
	./calibrationTest
