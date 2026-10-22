# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
# This board's host tests, included by tests/host/Makefile and run from there (paths are relative to tests/host)

# The calibration reader on a fake flash image
TESTS   += calibrationTest
calibrationTest: $(ROOT)/boards/dm1701/tests/calibrationTest.c $(ROOT)/boards/dm1701/calibration_dm1701.c \
		$(ROOT)/boards/dm1701/dm1701.h $(CORE)/radio/cal_curve.h
	$(CC) $(CFLAGS) -I$(ROOT)/hal -Wshadow -I$(ROOT)/boards/dm1701 -o $@ $(ROOT)/boards/dm1701/tests/calibrationTest.c \
		$(ROOT)/boards/dm1701/calibration_dm1701.c

run-boards:: calibrationTest
	./calibrationTest
