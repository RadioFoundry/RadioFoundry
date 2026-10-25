# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# The Kinetis K22F flash stub (docs/spec/stub-k22f.md): one image for K22F radios behind their own loader. No board
# code: it keeps the pin levels RadioFoundry left, and the host sends the board facts (SPI flash wiring, the
# writable flash range, the SPI flash's write floor) in a CONFIG record. The protocol engine and the runtime are shared
# with the STM32F4 stub (stub/common); the register names, the FTFA command object and the software SPI master are
# RadioFoundry's own (platform/k22f, drivers/bus/spi_gpio).
#
#   make -C stub/k22f                 build/stub-k22f/k22stub.bin (loaded by stub/stubtool.py --rfoundry-enter)
#   make -C stub/k22f BUILD=DIR       elsewhere
#   make -C stub/k22f check           build, then fail if the copy embedded in stub/k22stub.py differs (ci.sh runs it)
ROOT := ../..
ARM_GCC_DIR ?= $(firstword $(wildcard $(HOME)/opt/arm-gnu-toolchain-*-arm-none-eabi/bin/) /usr/bin/)
CROSS ?= $(ARM_GCC_DIR)arm-none-eabi-
CC := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
BUILD ?= $(ROOT)/build/stub-k22f

# Soft float: the stub has no floating point, and RadioFoundry hands over with the FPU context flag cleared. Link time
# optimization inlines the shared pieces across files
CFLAGS := -mcpu=cortex-m4 -mthumb -mfloat-abi=soft -Os -flto -std=gnu11 -ffreestanding -fno-builtin -ffunction-sections \
	-fdata-sections -fno-common -Wall -Wextra -Wshadow -Wstrict-prototypes \
	-I../common -I$(ROOT)/platform/k22f -I$(ROOT)/drivers/bus/spi_gpio -I$(ROOT)/third_party/cmsis-core/Include
LDFLAGS := -mcpu=cortex-m4 -mthumb -mfloat-abi=soft -flto -Os -nostdlib -nostartfiles -Wl,--gc-sections -T k22stub.ld \
	-Wl,-Map=$(BUILD)/k22stub.map -Wl,--print-memory-usage -Wl,--no-warn-rwx-segments

COMMON := stubeng.c stubrt.c stubcdc.c stubnor.c
SRC := k22stub_main.c k22stub_board.c k22stub_usb.c k22stub_flash.c k22stub_spi.c spi_gpio.c $(COMMON)
OBJ := $(patsubst %.c,$(BUILD)/%.o,$(SRC))
vpath %.c ../common $(ROOT)/drivers/bus/spi_gpio
HDR := $(wildcard ../common/*.h *.h) $(ROOT)/platform/k22f/k22f.h $(ROOT)/platform/k22f/k22f_usb_regs.h \
	$(ROOT)/platform/k22f/ftfa_math.h $(ROOT)/platform/k22f/stub_handoff.h

all: $(BUILD)/k22stub.bin

$(BUILD)/%.o: %.c $(HDR) Makefile
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/k22stub.elf: $(OBJ) k22stub.ld
	$(CC) $(LDFLAGS) -o $@ $(OBJ) -lgcc

$(BUILD)/k22stub.bin: $(BUILD)/k22stub.elf
	$(OBJCOPY) -O binary -j .image $< $@
	@echo "stub image $@: $$(wc -c < $@ | tr -d ' ') bytes (at most 8192)"

check: $(BUILD)/k22stub.bin
	@python3 ../k22stub.py --check-embedded $< > /dev/null || \
		{ echo "the copy embedded in stub/k22stub.py differs from $<: python3 stub/k22stub.py --embed $<"; exit 1; }

clean:
	rm -rf $(BUILD)

.PHONY: all check clean
