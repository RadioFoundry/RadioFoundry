# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# This board's host tests, included by tests/host/Makefile and run from there. HERE is this directory, read while the
# file is included; recipes use $< (the test, first) and $^ instead, which keep the paths they were given
HERE := $(dir $(lastword $(MAKEFILE_LIST)))

# The PA switching per DMR burst (pa_gate.h, hardware-d578uv.md 5.1)
TESTS   += d578uvPaGateTest
d578uvPaGateTest: $(HERE)d578uvPaGateTest.c $(HERE)../pa_gate.h
	$(CC) $(CFLAGS) -I$(dir $<).. -Wshadow -o $@ $(filter %.c,$^)

run-boards:: d578uvPaGateTest
	./d578uvPaGateTest

# The plain microphone's key lines and the backlight levels (front_io.h, hardware-d578uv.md 3.1, 3.2)
TESTS   += d578uvFrontIoTest
d578uvFrontIoTest: $(HERE)d578uvFrontIoTest.c $(HERE)../front_io.h
	$(CC) $(CFLAGS) -I$(dir $<).. -Wshadow -o $@ $(filter %.c,$^)

run-boards:: d578uvFrontIoTest
	./d578uvFrontIoTest

# The frequency plans outside the RF chips (rf_plan.h, hardware-d578uv.md 5.4)
TESTS   += d578uvRfPlanTest
d578uvRfPlanTest: $(HERE)d578uvRfPlanTest.c $(HERE)../rf_plan.h
	$(CC) $(CFLAGS) -I$(dir $<).. -Wshadow -o $@ $(filter %.c,$^)

run-boards:: d578uvRfPlanTest
	./d578uvRfPlanTest

# PB9's samples: APRS tones through the core's demodulator (tone_synth.h, hardware-d578uv.md 5.3)
TESTS   += d578uvToneSynthTest
d578uvToneSynthTest: $(HERE)d578uvToneSynthTest.c $(HERE)../tone_synth.h $(CORE)/aprs/hdlc.c $(CORE)/aprs/afsk.c $(CORE)/aprs/ax25.c
	$(CC) $(CFLAGS) -I$(dir $<).. -Wshadow -o $@ $(filter %.c,$^)

run-boards:: d578uvToneSynthTest
	./d578uvToneSynthTest
