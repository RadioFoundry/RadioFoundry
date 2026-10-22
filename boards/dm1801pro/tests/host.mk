# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
# This board's host tests, included by tests/host/Makefile and run from there (paths are relative to tests/host)

# The start-up pin levels and the audio route table (hardware-dm1801pro.md 3.1)
TESTS   += dm1801proPinsTest
dm1801proPinsTest: $(ROOT)/boards/dm1801pro/tests/dm1801proPinsTest.c $(ROOT)/boards/dm1801pro/boot_pins.h \
		$(ROOT)/boards/dm1801pro/pins.h $(ROOT)/boards/dm1801pro/audio_route.h
	$(CC) $(CFLAGS) -I$(ROOT)/boards/dm1801pro -Wshadow -o $@ $(ROOT)/boards/dm1801pro/tests/dm1801proPinsTest.c

run-boards:: dm1801proPinsTest
	./dm1801proPinsTest
