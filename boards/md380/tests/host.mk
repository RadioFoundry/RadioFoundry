# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# This board's host tests, included by tests/host/Makefile and run from there. HERE is this directory, read while the
# file is included; recipes use $< (the test, first) and $^ instead, which keep the paths they were given
HERE := $(dir $(lastword $(MAKEFILE_LIST)))

# The bit-banged RF bus (rfbus_md380.c, with the real pins.h) against fake chips on the documented wiring; stub/ holds
# the stand-in platform.h and FreeRTOS.h the board code includes
TESTS   += md380RfbusTest
md380RfbusTest: $(HERE)md380RfbusTest.c $(HERE)../rfbus_md380.c $(ROOT)/drivers/modem/hrc5000/hrc5000.c \
                $(ROOT)/drivers/modem/hrc_common/hrc.c $(ROOT)/core/dmr/ambe/ambeFec.c \
                $(HERE)../rfbus_md380.h $(HERE)../pins.h $(wildcard $(HERE)stub/*.h) $(ROOT)/hal/hal_dmr_modem.h \
                $(wildcard $(ROOT)/drivers/modem/hrc5000/*.h $(ROOT)/drivers/modem/hrc_common/*.h)
	$(CC) $(CFLAGS) -I$(dir $<)stub -I$(dir $<).. -I$(ROOT)/hal -Wshadow -o $@ $(filter %.c,$^)

# The panel type from the controller's ID and the factory byte (panel_md380.c)
TESTS   += md380PanelTest
md380PanelTest: $(HERE)md380PanelTest.c $(HERE)../panel_md380.c $(HERE)../panel_md380.h
	$(CC) $(CFLAGS) -I$(dir $<).. -o $@ $(filter %.c,$^)

# The calibration reader on a synthetic table (calibration_md380.c, hardware-md380.md 11)
TESTS   += md380CalTest
md380CalTest: $(HERE)md380CalTest.c $(HERE)../calibration_md380.c $(HERE)../md380.h $(CORE)/radio/cal_curve.h
	$(CC) $(CFLAGS) -I$(ROOT)/hal -I$(ROOT)/platform/stm32f4 -I$(dir $<).. -Wshadow -o $@ $(filter %.c,$^)

run-boards:: md380RfbusTest md380PanelTest md380CalTest
	./md380RfbusTest
	./md380PanelTest
	./md380CalTest
