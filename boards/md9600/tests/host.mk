# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
# This board's host tests, included by tests/host/Makefile and run from there (paths are relative to tests/host)

MD9600   := $(ROOT)/boards/md9600
MD9600_T := $(MD9600)/tests

# The synthesizer and IF receiver drivers and the board's revision and frequency plan, with every revision built in,
# and again with only V4A / V5 as a single-revision image would have
MD9600_RF := $(MD9600_T)/md9600RfTest.c $(MD9600)/rev_md9600.c $(MD9600)/rfplan_md9600.c \
	$(ROOT)/drivers/rf/sky72310/sky72310.c $(ROOT)/drivers/rf/pll1590/pll1590.c $(ROOT)/drivers/rf/lmx2571/lmx2571.c \
	$(ROOT)/drivers/rf/ak2365a/ak2365a.c
MD9600_RF_H := $(MD9600)/rfplan_md9600.h $(ROOT)/drivers/rf/sky72310/sky72310.h $(ROOT)/drivers/rf/pll1590/pll1590.h \
	$(ROOT)/drivers/rf/lmx2571/lmx2571.h $(ROOT)/drivers/rf/ak2365a/ak2365a.h $(ROOT)/drivers/bus/wire3.h
MD9600_ALL := -DCONFIG_MD9600_V1V3=1 -DCONFIG_MD9600_V4=1 -DCONFIG_MD9600_V5=1
TESTS   += md9600RfTest md9600RfTestV5

md9600RfTest: $(MD9600_RF) $(MD9600_RF_H)
	$(CC) $(CFLAGS) -I$(MD9600) -Wshadow $(MD9600_ALL) -o $@ $(MD9600_RF)

md9600RfTestV5: $(MD9600_RF) $(MD9600_RF_H)
	$(CC) $(CFLAGS) -I$(MD9600) -Wshadow -DCONFIG_MD9600_V5=1 -o $@ $(MD9600_RF)

# The RF sequencing and the calibration rows against a fake board, with transmit in the build and without it
MD9600_SEQ := $(MD9600_T)/md9600RfSeqTest.c $(MD9600)/rfseq_md9600.c $(MD9600)/cal_md9600.c $(MD9600)/rfcal_md9600.c \
	$(MD9600)/hwopt_md9600.c $(ROOT)/drivers/cal/calblock/calblock.c $(filter-out $(MD9600_T)/md9600RfTest.c,$(MD9600_RF))
MD9600_SEQ_H := $(MD9600_RF_H) $(MD9600)/rfseq_md9600.h $(MD9600)/md9600.h
TESTS   += md9600RfSeqTest md9600RfSeqTestNoTx md9600RfSeqTestV5

md9600RfSeqTest: $(MD9600_SEQ) $(MD9600_SEQ_H)
	$(CC) $(CFLAGS) -I$(MD9600) -I$(ROOT)/hal -Wshadow $(MD9600_ALL) -DCONFIG_MD9600_TX=1 -o $@ $(MD9600_SEQ)

md9600RfSeqTestNoTx: $(MD9600_SEQ) $(MD9600_SEQ_H)
	$(CC) $(CFLAGS) -I$(MD9600) -I$(ROOT)/hal -Wshadow $(MD9600_ALL) -DCONFIG_MD9600_TX=0 -o $@ $(MD9600_SEQ)

md9600RfSeqTestV5: $(MD9600_SEQ) $(MD9600_SEQ_H)
	$(CC) $(CFLAGS) -I$(MD9600) -I$(ROOT)/hal -Wshadow -DCONFIG_MD9600_V5=1 -DCONFIG_MD9600_TX=1 -o $@ $(MD9600_SEQ)

# The calibration and option block reader on the generic calibration block (synthetic blocks only)
MD9600_CAL := $(MD9600_T)/md9600CalTest.c $(MD9600)/cal_md9600.c $(ROOT)/drivers/cal/calblock/calblock.c
TESTS   += md9600CalTest

md9600CalTest: $(MD9600_CAL) $(MD9600)/cal_md9600.h $(ROOT)/drivers/cal/calblock/calblock.h $(CORE)/radio/cal_curve.h
	$(CC) $(CFLAGS) -I$(MD9600) -I$(ROOT)/hal -Wshadow -o $@ $(MD9600_CAL)

# The speaker routing's pin levels (speaker_md9600.h)
TESTS   += md9600AudioTest

md9600AudioTest: $(MD9600_T)/md9600AudioTest.c $(MD9600)/speaker_md9600.h
	$(CC) $(CFLAGS) -I$(MD9600) -Wshadow -o $@ $(MD9600_T)/md9600AudioTest.c

# The data flash map under core/storage and core/settings and the board record sector, on spiNorTest.c's chip model
MD9600_STORE := $(MD9600)/store_md9600.c $(MD9600)/rev_md9600.c
TESTS   += md9600StoreTest

md9600StoreTest: $(MD9600_T)/md9600StoreTest.c spiNorTest.c $(ROOT)/drivers/flash/spi_nor/spi_nor.c \
                 $(ROOT)/drivers/flash/spi_nor/spi_nor.h $(STORE) $(wildcard $(CORE)/storage/*.h $(CORE)/settings/*.h) \
                 $(MD9600_STORE) $(MD9600)/store_md9600.h $(MD9600)/rfplan_md9600.h
	$(CC) $(CFLAGS) -I$(ROOT)/hal -I. -I$(MD9600) -Wshadow $(MD9600_ALL) -o $@ $(MD9600_T)/md9600StoreTest.c \
		$(ROOT)/drivers/flash/spi_nor/spi_nor.c $(CORE)/storage/storage.c $(CORE)/settings/settings.c $(MD9600_STORE)

run-boards:: md9600RfTest md9600RfTestV5 md9600RfSeqTest md9600RfSeqTestNoTx md9600RfSeqTestV5 md9600CalTest \
             md9600AudioTest md9600StoreTest
	./md9600RfTest
	./md9600RfTestV5
	./md9600CalTest
	./md9600RfSeqTest
	./md9600RfSeqTestNoTx
	./md9600RfSeqTestV5
	./md9600AudioTest
	./md9600StoreTest
