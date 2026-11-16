# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# The P25 bench tool (p25tool.c) on the host, from the firmware's own sources. tools/bench/p25tool.py runs this.
#   make -C tools/bench/p25            -> build/bench/p25tool
HERE    := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
ROOT    := $(abspath $(HERE)/../../..)
CORE    := $(ROOT)/core
OUT     ?= $(ROOT)/build/bench
CC      ?= cc
CFLAGS  := -std=gnu99 -O2 -Wall -Wextra -Wshadow -I$(CORE) -I$(ROOT)/hal '-D__attribute__(x)=' \
           -DCONFIG_AIR=1 -DCONFIG_P25=1 -DCONFIG_P25_TRUNK=1
SRC     := $(HERE)/p25tool.c $(CORE)/p25/p25_unit.c $(CORE)/p25/p25_rx.c $(CORE)/p25/p25_call.c $(CORE)/p25/p25_tsbk.c \
           $(CORE)/air/air_framer.c $(CORE)/air/air_softmodem.c $(wildcard $(CORE)/fec/*.c)
HDRS    := $(wildcard $(CORE)/p25/*.h $(CORE)/air/*.h $(CORE)/fec/*.h) $(ROOT)/hal/hal_air.h $(ROOT)/hal/hal_baseband.h

$(OUT)/p25tool: $(SRC) $(HDRS)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -o $@ $(SRC) -lm

.PHONY: clean
clean:
	rm -f $(OUT)/p25tool
