# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
# This board's host tests, included by tests/host/Makefile and run from there. HERE is this directory, read while the
# file is included; recipes use $< (the test, first) and $^ instead, which keep the paths they were given
HERE := $(dir $(lastword $(MAKEFILE_LIST)))

# The start-up pin levels and the audio route table (hardware-dm1801pro.md 3.1)
TESTS   += dm1801proPinsTest
dm1801proPinsTest: $(HERE)dm1801proPinsTest.c $(HERE)../boot_pins.h $(HERE)../pins.h $(HERE)../audio_route.h
	$(CC) $(CFLAGS) -I$(dir $<).. -Wshadow -o $@ $<

run-boards:: dm1801proPinsTest
	./dm1801proPinsTest
