# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
# This board's host tests, included by tests/host/Makefile and run from there (paths are relative to tests/host)

# The bit-banged RF bus (rfbus_md380.c, with the real pins.h) against fake chips on the documented wiring; stub/ holds
# the stand-in platform.h and FreeRTOS.h the board code includes
TESTS   += md380RfbusTest
md380RfbusTest: $(ROOT)/boards/md380/tests/md380RfbusTest.c $(ROOT)/boards/md380/rfbus_md380.c \
                $(ROOT)/boards/md380/rfbus_md380.h $(ROOT)/boards/md380/pins.h $(wildcard $(ROOT)/boards/md380/tests/stub/*.h) \
                $(ROOT)/drivers/modem/hrc5000/hrc5000.c $(ROOT)/drivers/modem/hrc_common/hrc.c \
                $(wildcard $(ROOT)/drivers/modem/hrc5000/*.h $(ROOT)/drivers/modem/hrc_common/*.h) $(ROOT)/hal/hal_dmr_modem.h
	$(CC) $(CFLAGS) -I$(ROOT)/boards/md380/tests/stub -I$(ROOT)/boards/md380 -I$(ROOT)/hal -Wshadow -o $@ \
		$(ROOT)/boards/md380/tests/md380RfbusTest.c $(ROOT)/boards/md380/rfbus_md380.c \
		$(ROOT)/drivers/modem/hrc5000/hrc5000.c $(ROOT)/drivers/modem/hrc_common/hrc.c $(ROOT)/core/dmr/ambe/ambeFec.c

run-boards:: md380RfbusTest
	./md380RfbusTest
