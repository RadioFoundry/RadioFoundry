# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# This board's host tests, and those of the GD-88's second SoC (boards/gd88b), included by tests/host/Makefile and run
# from there. HERE is this directory, read while the file is included; recipes use $< (the test, first) and $^ instead,
# which keep the paths they were given
HERE := $(dir $(lastword $(MAKEFILE_LIST)))

# The factory calibration page, the layout of hardware-db25d.md 6 on each SoC (hardware-gd88.md 7), in the code both
# SoCs share
TESTS   += gd88CalTest
gd88CalTest: $(HERE)gd88CalTest.c $(HERE)../common/calibration_gd88.c $(HERE)../common/gd88_cal.h $(CORE)/radio/cal_curve.h
	$(CC) $(CFLAGS) -I$(ROOT)/hal -I$(dir $<)../common -Wshadow -o $@ $(filter %.c,$^)

# The two first stages (boot/gd88, boot/gd88b): the RF chain and UART1 on A, UART3 and the long window on B. One test,
# built for each SoC; it includes the stage's boot.c
TESTS   += bootGd88Test bootGd88bTest
bootGd88Test: $(HERE)bootGd88Test.c $(ROOT)/boot/gd88/boot.c $(ROOT)/boot/hrc7000/stage.c
	$(CC) $(CFLAGS) -Wshadow -DSOC_B=0 -o $@ $<
bootGd88bTest: $(HERE)bootGd88Test.c $(ROOT)/boot/gd88b/boot.c $(ROOT)/boot/hrc7000/stage.c
	$(CC) $(CFLAGS) -Wshadow -DSOC_B=1 -o $@ $<

# SoC B's descriptor: receive-only bands (its tx() refuses every frequency), or transmitting ones as A's second DMR
# transmitter (Tx), and the transmitter-off hook, built against a host stand-in for the platform header. The
# calibration page's flash address is an integer cast to a pointer
TESTS   += gd88bBoardTest gd88bBoardTestTx
gd88bBoardTest: $(HERE)gd88bBoardTest.c $(ROOT)/boards/gd88b/board.c $(HERE)hostplat/platform.h
	$(CC) $(CFLAGS) -I$(dir $<)hostplat -I$(ROOT)/hal -I$(ROOT)/boards/gd88b -I$(dir $<)../common $(FMONLY) \
		-DGD88_TX=0 -DGD88_DMR_TX=0 -Wshadow -Wno-int-to-pointer-cast -o $@ $<
gd88bBoardTestTx: $(HERE)gd88bBoardTest.c $(ROOT)/boards/gd88b/board.c $(HERE)hostplat/platform.h
	$(CC) $(CFLAGS) -I$(dir $<)hostplat -I$(ROOT)/hal -I$(ROOT)/boards/gd88b -I$(dir $<)../common $(FMONLY) \
		-DGD88_TX=0 -DGD88_DMR_TX=1 -Wshadow -Wno-int-to-pointer-cast -o $@ $<

run-boards:: gd88CalTest bootGd88Test bootGd88bTest gd88bBoardTest gd88bBoardTestTx
	./gd88CalTest
	./bootGd88Test
	./bootGd88bTest
	./gd88bBoardTest
	./gd88bBoardTestTx
