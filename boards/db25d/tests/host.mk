# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
# This board's host tests, included by tests/host/Makefile and run from there (paths are relative to tests/host)

# The factory calibration page (hardware-db25d.md 6)
TESTS   += db25dCalTest
db25dCalTest: $(ROOT)/boards/db25d/tests/db25dCalTest.c $(ROOT)/boards/db25d/calibration_db25d.c \
		$(ROOT)/boards/db25d/db25d.h $(CORE)/radio/cal_curve.h
	$(CC) $(CFLAGS) -I$(ROOT)/hal -I$(ROOT)/boards/db25d -Wshadow -o $@ $(ROOT)/boards/db25d/tests/db25dCalTest.c \
		$(ROOT)/boards/db25d/calibration_db25d.c

# The first stage in front of the application (boot/db25d): the stub load, dropped frames, the window, a cold UART
TESTS   += bootDb25dTest
bootDb25dTest: $(ROOT)/boards/db25d/tests/bootDb25dTest.c $(ROOT)/boot/db25d/boot.c
	$(CC) $(CFLAGS) -Wshadow -o $@ $(ROOT)/boards/db25d/tests/bootDb25dTest.c

run-boards:: db25dCalTest bootDb25dTest
	./db25dCalTest
	./bootDb25dTest
