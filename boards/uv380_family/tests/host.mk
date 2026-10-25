# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# The UV380 family's host tests, included by tests/host/Makefile and run from there. HERE is this directory, read while
# the file is included; recipes use $< (the test, first) and $^ instead, which keep the paths they were given
HERE := $(dir $(lastword $(MAKEFILE_LIST)))

# The calibration reader on a fake flash image, with the DM-1701's power conversion and with the MD-UV380's
TESTS   += calibrationTest calibrationTestMduv380
calibrationTest: $(HERE)calibrationTest.c $(HERE)../calibration_uvf.c $(HERE)../uvf.h $(CORE)/radio/cal_curve.h
	$(CC) $(CFLAGS) -I$(ROOT)/hal -I$(ROOT)/platform/stm32f4 -Wshadow -I$(dir $<).. -o $@ $(filter %.c,$^)
calibrationTestMduv380: $(HERE)calibrationTest.c $(HERE)../calibration_uvf.c $(HERE)../uvf.h $(CORE)/radio/cal_curve.h
	$(CC) $(CFLAGS) -DTEST_MDUV380=1 -I$(ROOT)/hal -I$(ROOT)/platform/stm32f4 -Wshadow -I$(dir $<).. -o $@ $(filter %.c,$^)

run-boards:: calibrationTest calibrationTestMduv380
	./calibrationTest
	./calibrationTestMduv380
