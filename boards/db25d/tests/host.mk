# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# This board's host tests, included by tests/host/Makefile and run from there. HERE is this directory, read while the
# file is included; recipes use $< (the test, first) and $^ instead, which keep the paths they were given
HERE := $(dir $(lastword $(MAKEFILE_LIST)))

# The factory calibration page (hardware-db25d.md 6)
TESTS   += db25dCalTest
db25dCalTest: $(HERE)db25dCalTest.c $(HERE)../calibration_db25d.c $(HERE)../db25d.h $(CORE)/radio/cal_curve.h
	$(CC) $(CFLAGS) -I$(ROOT)/hal -I$(dir $<).. -Wshadow -o $@ $(filter %.c,$^)

# The first stage in front of the application (boot/db25d): the stub load, dropped frames, the window, a cold UART.
# The test includes boot.c, which includes the shared protocol (boot/hrc7000/stage.c)
TESTS   += bootDb25dTest
bootDb25dTest: $(HERE)bootDb25dTest.c $(ROOT)/boot/db25d/boot.c $(ROOT)/boot/hrc7000/stage.c
	$(CC) $(CFLAGS) -Wshadow -o $@ $<

run-boards:: db25dCalTest bootDb25dTest
	./db25dCalTest
	./bootDb25dTest
