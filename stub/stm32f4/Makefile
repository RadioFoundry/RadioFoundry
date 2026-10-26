# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# The STM32F4 flash stub (docs/spec/stub-stm32f4.md): one image for every STM32F405 radio behind the AnyRoad / TYT
# bootloader. No board code: it keeps the pin levels RadioFoundry left, and the host sends the board
# facts (SPI flash wiring, crystal, bootloader end) in a CONFIG record. The protocol engine is shared with other stubs
# (stub/common).
#
#   make -C stub/stm32f4                 build/stub-stm32f4/f4stub.bin (loaded by stub/stubtool.py --rfoundry-enter)
#   make -C stub/stm32f4 BUILD=DIR       elsewhere
#   make -C stub/stm32f4 check           build, then fail if the copy embedded in stub/f4stub.py differs (ci.sh runs it)
ROOT := ../..
ARM_GCC_DIR ?= $(firstword $(wildcard $(HOME)/opt/arm-gnu-toolchain-*-arm-none-eabi/bin/) /usr/bin/)
CROSS ?= $(ARM_GCC_DIR)arm-none-eabi-
CC := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
SIZE := $(CROSS)size
BUILD ?= $(ROOT)/build/stub-stm32f4

# Soft float: the stub has no floating point, and RadioFoundry may hand over with the FPU context flag in any state.
# Link-time optimization: the image must fit RadioFoundry's 8 KB staging buffer, and across files (the engine's ops
# through the port's callbacks) it saves about 400 bytes
CFLAGS := -mcpu=cortex-m4 -mthumb -mfloat-abi=soft -Os -flto -std=gnu11 -ffreestanding -fno-builtin -ffunction-sections \
	-fdata-sections -fno-common -Wall -Wextra -Wshadow -Wstrict-prototypes -DSTM32F405xx \
	-I../common -I$(ROOT)/third_party/cmsis-device-f4/Include -I$(ROOT)/third_party/cmsis-core/Include
LDFLAGS := -mcpu=cortex-m4 -mthumb -mfloat-abi=soft -Os -flto -nostdlib -nostartfiles -Wl,--gc-sections -T f4stub.ld \
	-Wl,-Map=$(BUILD)/f4stub.map -Wl,--print-memory-usage -Wl,--no-warn-rwx-segments

SRC := f4stub_main.c stubeng.c f4stub_board.c f4stub_usb.c f4stub_flash.c f4stub_spi.c
OBJ := $(patsubst %.c,$(BUILD)/%.o,$(SRC))
vpath stubeng.c ../common

all: $(BUILD)/f4stub.bin

$(BUILD)/%.o: %.c ../common/stubeng.h f4stub.h f4stub_usb.h f4stub_flash.h f4stub_spi.h Makefile
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/f4stub.elf: $(OBJ) f4stub.ld
	$(CC) $(LDFLAGS) -o $@ $(OBJ) -lgcc

$(BUILD)/f4stub.bin: $(BUILD)/f4stub.elf
	$(OBJCOPY) -O binary -j .image $< $@
	@echo "stub image $@: $$(wc -c < $@ | tr -d ' ') bytes (at most 8192)"

check: $(BUILD)/f4stub.bin
	@python3 ../f4stub.py --check-embedded $< > /dev/null || \
		{ echo "the copy embedded in stub/f4stub.py differs from $<: python3 stub/f4stub.py --embed $<"; exit 1; }

clean:
	rm -rf $(BUILD)

.PHONY: all check clean
