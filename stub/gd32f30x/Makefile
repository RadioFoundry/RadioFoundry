# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# The GD32F30x flash stub (docs/spec/stub-gd32f30x.md): one image for every GD32F30x radio behind a vendor bootloader.
# No board code: it keeps the pin levels RadioFoundry left, and the host sends the bootloader's end in a CONFIG record.
# The protocol engine is shared with the other stubs (stub/common).
#
#   make -C stub/gd32f30x                build/stub-gd32f30x/gd32stub.bin
#   make -C stub/gd32f30x BUILD=DIR      elsewhere
#   make -C stub/gd32f30x check          build, then fail if the copy embedded in stub/f4stub.py differs (ci.sh runs it)
ROOT := ../..
ARM_GCC_DIR ?= $(firstword $(wildcard $(HOME)/opt/arm-gnu-toolchain-*-arm-none-eabi/bin/) /usr/bin/)
CROSS ?= $(ARM_GCC_DIR)arm-none-eabi-
CC := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
BUILD ?= $(ROOT)/build/stub-gd32f30x

# Soft float: the stub has no floating point, and RadioFoundry may hand over with the FPU context flag in any state.
# Link-time optimization across the engine's ops and the port's callbacks, as the STM32F4 stub
CFLAGS := -mcpu=cortex-m4 -mthumb -mfloat-abi=soft -Os -flto -std=gnu11 -ffreestanding -fno-builtin -ffunction-sections \
	-fdata-sections -fno-common -Wall -Wextra -Wshadow -Wstrict-prototypes \
	-I../common -I$(ROOT)/platform/gd32f30x -I$(ROOT)/third_party/cmsis-core/Include
LDFLAGS := -mcpu=cortex-m4 -mthumb -mfloat-abi=soft -Os -flto -nostdlib -nostartfiles -Wl,--gc-sections -T gd32stub.ld \
	-Wl,-Map=$(BUILD)/gd32stub.map -Wl,--print-memory-usage -Wl,--no-warn-rwx-segments

SRC := gd32stub_main.c stubeng.c gd32stub_board.c gd32stub_usb.c gd32stub_flash.c
OBJ := $(patsubst %.c,$(BUILD)/%.o,$(SRC))
vpath stubeng.c ../common

all: $(BUILD)/gd32stub.bin

$(BUILD)/%.o: %.c ../common/stubeng.h gd32stub.h gd32stub_header.h gd32stub_usb.h gd32stub_flash.h Makefile
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/gd32stub.elf: $(OBJ) gd32stub.ld
	$(CC) $(LDFLAGS) -o $@ $(OBJ) -lgcc

$(BUILD)/gd32stub.bin: $(BUILD)/gd32stub.elf
	$(OBJCOPY) -O binary -j .image $< $@
	@echo "stub image $@: $$(wc -c < $@ | tr -d ' ') bytes (at most 8192)"

check: $(BUILD)/gd32stub.bin
	@python3 ../f4stub.py --check-embedded $< > /dev/null || \
		{ echo "the copy embedded in stub/f4stub.py differs from $<: python3 stub/f4stub.py --embed $<"; exit 1; }

clean:
	rm -rf $(BUILD)

.PHONY: all check clean
