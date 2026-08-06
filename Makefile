# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
## Firmware build, one tree for every radio:
##
##   make BOARD=dm1701          -> build/dm1701/odr_dm1701.{elf,bin,map}
##   make BOARD=dm1701 size     -> section sizes and the space left before the codec
##   make test                  -> host tests
##
## Each board says its platform, linker script and source directories in boards/<board>/board.mk.
## Every *.c in the listed directories is built, so adding a file needs no Makefile change.

BOARD ?= dm1701
include boards/$(BOARD)/board.mk
-include boards/$(BOARD)/config.mk
include config/features.mk
include platform/$(PLATFORM)/platform.mk

BUILD := build/$(BOARD)
TARGET := $(BUILD)/odr_$(BOARD)

CORE_DIRS := core/app core/radio core/ui core/input core/codeplug
ifeq ($(CONFIG_DMR),y)
CORE_DIRS += core/dmr/call core/dmr/ambe core/dmr/fec
endif
ifeq ($(CONFIG_USB),y)
CORE_DIRS += core/hostlink $(BOARD_USB_DIRS)
endif
ifeq ($(CONFIG_HOTSPOT),y)
CORE_DIRS += core/hotspot
endif
ifeq ($(CONFIG_DEBUGLINK),y)
CORE_DIRS += core/debuglink
endif
APP_DIRS := $(CORE_DIRS) $(PLATFORM_DIRS) $(BOARD_DIRS)
APP_SRC := $(foreach d,$(APP_DIRS),$(wildcard $(d)/*.c))
# Single files of core/dmr (the directory also holds the data service, built by its own switches)
ifeq ($(CONFIG_DMR_PRIVACY),y)
APP_SRC += core/dmr/dmrPrivacy.c
endif

INCLUDES := -Ihal -Icore -Idrivers -Iboards/$(BOARD) -Iplatform/$(PLATFORM) $(PLATFORM_INC)

GITVERSION := $(shell git rev-parse --short=8 HEAD 2>/dev/null || echo unknown)
DEFS := $(MCU_DEFS) $(FEATURE_DEFS) -DODR_BOARD_NAME=\"$(BOARD)\" -DODR_GIT=\"$(GITVERSION)\"

OPT ?= -Os
COMMON := $(CPUFLAGS) $(OPT) -g3 -ffunction-sections -fdata-sections -fno-common -MMD -MP
CFLAGS_APP := $(COMMON) -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wshadow -Wdouble-promotion $(DEFS) $(INCLUDES)
CFLAGS_3P := $(COMMON) -std=gnu11 -w $(DEFS) $(INCLUDES)
LDFLAGS := $(CPUFLAGS) -T$(LDSCRIPT) -Wl,--gc-sections -Wl,-Map=$(TARGET).map -Wl,--print-memory-usage \
	--specs=nano.specs --specs=nosys.specs -Wl,--no-warn-rwx-segments

APP_OBJ := $(APP_SRC:%.c=$(BUILD)/%.o)
TP_OBJ := $(patsubst %.c,$(BUILD)/%.o,$(filter %.c,$(PLATFORM_3P_SRC))) $(patsubst %.s,$(BUILD)/%.o,$(filter %.s,$(PLATFORM_3P_SRC)))
OBJ := $(APP_OBJ) $(TP_OBJ)

.PHONY: all clean size test FORCE
all: $(TARGET).bin

# Rebuild everything when the compile flags change (feature switches and build values)
$(BUILD)/flags: FORCE
	@mkdir -p $(BUILD)
	@echo '$(CFLAGS_APP)' | cmp -s - $@ || echo '$(CFLAGS_APP)' > $@
$(APP_OBJ) $(TP_OBJ): $(BUILD)/flags

$(TARGET).elf: $(OBJ) $(LDSCRIPT)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) -lm
$(TARGET).bin: $(TARGET).elf
	$(OBJCOPY) -O binary -j .isr_vector -j .text -j .rodata -j .ARM.extab -j .ARM -j .preinit_array -j .init_array -j .fini_array -j .data $< $@
	@echo "image $@: $$(wc -c < $@ | tr -d ' ') bytes"

$(APP_OBJ): $(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_APP) -c $< -o $@
$(BUILD)/third_party/%.o: third_party/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_3P) -c $< -o $@
$(BUILD)/third_party/%.o: third_party/%.s
	@mkdir -p $(dir $@)
	$(CC) $(CPUFLAGS) -c $< -o $@

size: $(TARGET).elf
	$(SIZE) -A $< | grep -E '^\.(isr_vector|text|rodata|rodata2|data|bss|ccmram)'

test:
	$(MAKE) -C tests/host

clean:
	rm -rf $(BUILD)

-include $(OBJ:.o=.d)
