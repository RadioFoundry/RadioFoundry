# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
# This board's host tests, included by tests/host/Makefile and run from there. HERE is this directory, read while the
# file is included; recipes use $< (the test, first) and $^ instead, which keep the paths they were given
HERE := $(dir $(lastword $(MAKEFILE_LIST)))
GD77 := $(HERE)..
GD77_K22F := $(ROOT)/platform/k22f

# The K22F platform's clock, divider and pin arithmetic with this board's clock plans; its flash window, reset causes,
# HSRUN step and time base, I2C / DSPI / PWM arithmetic and pin alternates; the board's pin table, key map and bus
# choices (docs/spec/hardware-gd77.md)
TESTS   += k22fPlatformTest k22fSystemTest gd77BoardTest
k22fPlatformTest: $(HERE)k22fPlatformTest.c $(GD77_K22F)/clock_math.h $(GD77_K22F)/k22f_pin.h $(GD77)/clock_gd77.h
	$(CC) $(CFLAGS) -I$(GD77_K22F) -I$(dir $<).. -Wshadow -o $@ $<
k22fSystemTest: $(HERE)k22fSystemTest.c $(GD77_K22F)/clock_math.h $(GD77_K22F)/ftfa_math.h \
                $(GD77_K22F)/periph_math.h $(GD77_K22F)/k22f_mux.h $(GD77_K22F)/k22f_pin.h $(GD77)/clock_gd77.h \
                $(GD77)/pin_table.h $(GD77)/bus_cfg.h $(GD77)/pins.h
	$(CC) $(CFLAGS) -I$(GD77_K22F) -I$(dir $<).. -Wshadow -o $@ $<
gd77BoardTest: $(HERE)gd77BoardTest.c $(ROOT)/drivers/input/keymatrix/keymatrix.c $(GD77)/pin_table.h $(GD77)/pins.h \
               $(GD77)/keys.h $(GD77)/bus_cfg.h $(GD77_K22F)/k22f_pin.h $(GD77_K22F)/k22f_mux.h
	$(CC) $(CFLAGS) -I$(GD77_K22F) -I$(dir $<).. -Wshadow -o $@ $(filter %.c,$^)

# The key map's rules (information-architecture.md 2), with and without DMR emergency on the top key
TESTS   += gd77KeymapTest gd77KeymapEmTest
gd77KeymapTest: $(HERE)gd77KeymapTest.c $(GD77)/keymap_gd77.h $(GD77)/keys.h $(ROOT)/hal/hal_input.h
	$(CC) $(CFLAGS) -I$(ROOT)/hal -I$(dir $<).. -Wshadow -DCONFIG_DMR_EMERGENCY=0 -o $@ $<
gd77KeymapEmTest: $(HERE)gd77KeymapTest.c $(GD77)/keymap_gd77.h $(GD77)/keys.h $(ROOT)/hal/hal_input.h
	$(CC) $(CFLAGS) -I$(ROOT)/hal -I$(dir $<).. -Wshadow -DCONFIG_DMR_EMERGENCY=1 -o $@ $<

# The audio and RF side: PCM framing between the I2S rings and 20 ms blocks; the AT1846S with the radio's profile, the
# path line bands, the switch sequences, the DAC0 ramp, the HR_C6000 board values, the timeslot edge rule
TESTS   += gd77PcmTest gd77RfTest
gd77PcmTest: $(HERE)gd77PcmTest.c $(GD77)/pcm_gd77.c $(GD77)/pcm_gd77.h
	$(CC) $(CFLAGS) -I$(dir $<).. -Wshadow -o $@ $(filter %.c,$^)
gd77RfTest: $(HERE)gd77RfTest.c $(GD77)/rf_gd77.c $(GD77)/c6k_gd77.c $(ROOT)/drivers/rf/at1846s/at1846s.c \
            $(ROOT)/drivers/rf/subaudio/subaudio.c $(ROOT)/drivers/modem/hrc6000/hrc6000.c \
            $(ROOT)/drivers/modem/hrc_common/hrc.c $(GD77)/rf_gd77.h $(GD77)/c6k_gd77.h $(GD77)/dmr_gd77.h \
            $(ROOT)/drivers/rf/at1846s/at1846s.h $(ROOT)/drivers/modem/hrc6000/hrc6000.h \
            $(ROOT)/drivers/modem/hrc_common/hrc.h
	$(CC) $(CFLAGS) -I$(dir $<).. -I$(ROOT)/hal -Wshadow -o $@ $(filter %.c,$^)

# Storage and calibration (hardware-gd77.md 6.2-6.4): the board's NOR map and write guard with core/storage and
# core/settings on it under power cuts, the logical space split, the calibration blocks (drivers/cal/cal_block and this
# board's layout) and the calibration point: node layouts, interpolation, every destination value and its fallback, the
# band split and invalid blocks, on synthetic data
TESTS   += gd77StorageTest gd77CalTest
gd77StorageTest: $(HERE)gd77StorageTest.c $(GD77)/storage_gd77.c $(GD77)/cal_blocks_gd77.c $(GD77)/cal_gd77.c \
                 $(ROOT)/drivers/cal/cal_block.c $(ROOT)/core/storage/storage.c $(ROOT)/core/settings/settings.c \
                 sim/sim_flash.c $(GD77)/storage_gd77.h $(GD77)/cal_blocks_gd77.h $(GD77)/cal_gd77.h \
                 $(ROOT)/drivers/cal/cal_block.h $(wildcard $(ROOT)/core/storage/*.h $(ROOT)/core/settings/*.h)
	$(CC) $(CFLAGS) -I$(ROOT)/hal -I$(dir $<).. -I. -Wshadow -o $@ $(filter %.c,$^)
gd77CalTest: $(HERE)gd77CalTest.c $(GD77)/cal_gd77.c $(GD77)/cal_blocks_gd77.c $(ROOT)/drivers/cal/cal_block.c \
             $(GD77)/cal_gd77.h $(GD77)/cal_blocks_gd77.h $(ROOT)/drivers/cal/cal_block.h
	$(CC) $(CFLAGS) -I$(dir $<).. -Wshadow -o $@ $(filter %.c,$^)

run-boards:: k22fPlatformTest k22fSystemTest gd77BoardTest gd77KeymapTest gd77KeymapEmTest gd77PcmTest gd77RfTest \
             gd77StorageTest gd77CalTest
	./k22fPlatformTest
	./k22fSystemTest
	./gd77BoardTest
	./gd77KeymapTest
	./gd77KeymapEmTest
	./gd77PcmTest
	./gd77RfTest
	./gd77StorageTest
	./gd77CalTest
