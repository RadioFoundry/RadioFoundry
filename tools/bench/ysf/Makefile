# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# The YSF bench tool (ysftool.c) on the host, from the firmware's own sources. tools/bench/ysftool.py runs this.
#   make -C tools/bench/ysf            -> build/bench/ysftool
HERE    := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
ROOT    := $(abspath $(HERE)/../../..)
CORE    := $(ROOT)/core
OUT     ?= $(ROOT)/build/bench
CC      ?= cc
# Blanking __attribute__ also strips glibc's always_inline from its fortified printf wrappers (as nxdn/Makefile)
CFLAGS  := -std=gnu99 -O2 -Wall -Wextra -Wshadow -I$(CORE) -I$(ROOT)/hal '-D__attribute__(x)=' -U_FORTIFY_SOURCE \
           -DCONFIG_AIR=1 -DCONFIG_YSF=1
SRC     := $(HERE)/ysftool.c $(CORE)/ysf/ysf_frame.c $(CORE)/ysf/ysf_rx.c $(CORE)/ysf/ysf_call.c $(CORE)/ysf/ysf_wx.c \
           $(CORE)/air/air_framer.c $(CORE)/fec/conv.c $(CORE)/fec/golay.c $(CORE)/fec/crc16.c \
           $(CORE)/dmr/ambe/ambeFec.c
HDRS    := $(wildcard $(CORE)/ysf/*.h $(CORE)/air/*.h $(CORE)/fec/*.h) $(ROOT)/hal/hal_air.h

$(OUT)/ysftool: $(SRC) $(HDRS)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -o $@ $(SRC) -lm

.PHONY: clean
clean:
	rm -f $(OUT)/ysftool
