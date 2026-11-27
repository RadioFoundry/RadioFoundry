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

# LTO drops unused board hardware before the host sanitizers register its globals.
# Linux Clang can use its own linker when the system linker lacks the LLVM plugin.
DM36_TEST_LD := $(if $(and $(findstring clang,$(CC)),$(filter Linux,$(shell uname -s)),$(shell command -v ld.lld)),-fuse-ld=lld)
TESTS += receiveControlTest
receiveControlTest: $(HERE)receiveControlTest.c $(HERE)../../dm32/power_dm32.c $(HERE)../../dm32/radio_dm32.c \
		$(HERE)../../dm32/audio_route.h $(HERE)../pins.h $(HERE)../volume.h
	$(CC) $(CFLAGS) -U__attribute__ -O2 -flto $(DM36_TEST_LD) \
		-I$(dir $<).. -I$(ROOT)/boards/dm32 -I$(ROOT)/hal -I$(ROOT)/platform/hrc7000 -o $@ $(wordlist 1,2,$^)
run-boards:: receiveControlTest
	./receiveControlTest
