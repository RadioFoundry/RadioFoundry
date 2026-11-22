# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# The NXDN bench tool (nxdntool.c) on the host, from the firmware's own sources. tools/bench/nxdntool.py runs this.
#   make -C tools/bench/nxdn           -> build/bench/nxdntool
HERE    := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
ROOT    := $(abspath $(HERE)/../../..)
CORE    := $(ROOT)/core
OUT     ?= $(ROOT)/build/bench
CC      ?= cc
# Blanking __attribute__ also strips glibc's always_inline from its fortified printf wrappers, which then fail to
# build where the compiler turns _FORTIFY_SOURCE on by default
CFLAGS  := -std=gnu99 -O2 -Wall -Wextra -Wshadow -I$(CORE) -I$(ROOT)/hal '-D__attribute__(x)=' -U_FORTIFY_SOURCE \
           -DCONFIG_AIR=1 -DCONFIG_NXDN=1 -DCONFIG_NXDN_OTA_ALIAS=1
SRC     := $(HERE)/nxdntool.c $(CORE)/nxdn/nxdn_frame.c $(CORE)/nxdn/nxdn_rx.c $(CORE)/nxdn/nxdn_call.c \
           $(CORE)/air/air_framer.c $(CORE)/air/air_softmodem.c $(CORE)/fec/conv.c
HDRS    := $(wildcard $(CORE)/nxdn/*.h $(CORE)/air/*.h $(CORE)/fec/*.h) $(ROOT)/hal/hal_air.h $(ROOT)/hal/hal_baseband.h

$(OUT)/nxdntool: $(SRC) $(HDRS)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -o $@ $(SRC) -lm

.PHONY: clean
clean:
	rm -f $(OUT)/nxdntool
