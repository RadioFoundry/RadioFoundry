# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# This board's host tests, included by tests/host/Makefile and run from there. HERE is this directory, read while the
# file is included; recipes use $< (the test, first) and $^ instead, which keep the paths they were given
HERE := $(dir $(lastword $(MAKEFILE_LIST)))

# The band table (bands_dm32.h): receive to 520 MHz, transmit to 480, the switched extension past them. Built as the
# default build has it, with the air band, broadcast FM and the extension
TESTS   += dm32BandTest
dm32BandTest: $(HERE)dm32BandTest.c $(HERE)../bands_dm32.h $(ROOT)/hal/board.h
	$(CC) $(CFLAGS) -I$(ROOT)/hal -I$(dir $<).. -DCONFIG_AM_RX=1 -DCONFIG_BCAST_FM=1 -DCONFIG_RX_EXT=1 -Wshadow -o $@ $<

run-boards:: dm32BandTest
	./dm32BandTest
