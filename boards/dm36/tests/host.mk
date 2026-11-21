# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
HERE := $(dir $(lastword $(MAKEFILE_LIST)))
TESTS += txBandsTest
txBandsTest: $(HERE)txBandsTest.c $(HERE)../bands.h $(HERE)../pins.h $(HERE)../../dm32/pins.h $(ROOT)/hal/board.h fuzz.h
	$(CC) $(CFLAGS) -I$(ROOT)/hal -I. -o $@ $<
run-boards:: txBandsTest
	./txBandsTest

TESTS += preflashTest
preflashTest: $(HERE)preflashTest.c $(HERE)../io_init.h $(HERE)../volume.h $(HERE)../pins.h $(ROOT)/drivers/input/quadrature/quadrature.c fuzz.h
	$(CC) $(CFLAGS) -I$(dir $<).. -I. -I$(ROOT)/hal -o $@ $< $(ROOT)/drivers/input/quadrature/quadrature.c
run-boards:: preflashTest
	./preflashTest
