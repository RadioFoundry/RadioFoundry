# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# This board's host tests, included by tests/host/Makefile and run from there. HERE is this directory, read while the
# file is included; recipes use $< (the test, first) and $^ instead, which keep the paths they were given
HERE := $(dir $(lastword $(MAKEFILE_LIST)))

# The speaker source, path pin and FD6818 AF output by radio state, with the shared and the stock-usage options
TESTS   += audioRouteTest
audioRouteTest: $(HERE)audioRouteTest.c $(HERE)../audio_route.h
	$(CC) $(CFLAGS) -I$(dir $<).. -Wshadow -o $@ $<

run-boards:: audioRouteTest
	./audioRouteTest
