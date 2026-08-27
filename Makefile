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

CORE_DIRS := core/app core/radio core/ui core/input core/codeplug core/settings core/storage
ifeq ($(CONFIG_DMR),y)
CORE_DIRS += core/dmr/call core/dmr/ambe core/dmr/fec
endif
ifeq ($(CONFIG_HOSTLINK),y)
CORE_DIRS += core/hostlink
endif
ifeq ($(CONFIG_USB),y)
CORE_DIRS += $(BOARD_USB_DIRS)
endif
ifeq ($(CONFIG_HOTSPOT),y)
CORE_DIRS += core/hotspot
endif
ifeq ($(CONFIG_CPS_LEGACY),y)
CORE_DIRS += core/cps
endif
ifeq ($(CONFIG_DEBUGLINK),y)
CORE_DIRS += core/debuglink
endif
APP_DIRS := $(CORE_DIRS) $(PLATFORM_DIRS) $(BOARD_DIRS)
APP_SRC := $(foreach d,$(APP_DIRS),$(wildcard $(d)/*.c))
APP_ASM := $(foreach d,$(APP_DIRS),$(wildcard $(d)/*.S))
# Single files of core/dmr, whose other files belong to other features
ifeq ($(CONFIG_DMR_PRIVACY),y)
APP_SRC += core/dmr/dmrPrivacy.c
endif
# DMR data (SMS, ARS, LRRP, CSBK signaling)
ifeq ($(CONFIG_DMR_DATA_CORE),y)
APP_SRC += core/dmr/dmrData.c core/dmr/dmrDataService.c core/dmr/dmrLrrp.c
endif
# DMR emergency (the alarm goes through the data service above)
ifeq ($(CONFIG_DMR_EMERGENCY),y)
APP_SRC += core/dmr/dmr_emergency.c
endif
# The single frequency repeater: the generic logic and its runner on the DMR modem (the hotspot's SFR mode is in
# core/hotspot/hotspotSfr.c)
ifeq ($(CONFIG_SFR),y)
APP_SRC += core/dmr/sfr.c core/dmr/sfrRunner.c
endif
# The IP gateway (the data service above carries its packets: CONFIG_DMR_DATA_CORE follows it)
ifeq ($(CONFIG_DMR_IP_GATEWAY),y)
APP_SRC += core/dmr/ipGateway.c
endif

INCLUDES := -Ihal -Icore -Idrivers -Iboards/$(BOARD) -Iplatform/$(PLATFORM) $(PLATFORM_INC)

# Firmware version: the commit, with -dirty when tracked files differ from it or untracked sources sit in the built
# directories (both end up in the image). Only the files that use ODR_GIT get it, through their own stamp, so a new
# commit or an edit rebuilds those few objects, not everything.
GITVERSION := $(shell git rev-parse --short=8 HEAD 2>/dev/null || echo unknown)
ifneq ($(GITVERSION),unknown)
GITDIRTY := $(shell git update-index -q --refresh >/dev/null 2>&1; git diff-index --quiet --ignore-submodules=all HEAD -- \
	2>/dev/null || echo -dirty)
ifeq ($(GITDIRTY),)
GITDIRTY := $(if $(shell git ls-files --others --exclude-standard -- $(APP_DIRS) config Makefile 2>/dev/null),-dirty)
endif
endif
ODR_VERSION := $(GITVERSION)$(GITDIRTY)
DEFS := $(MCU_DEFS) $(FEATURE_DEFS) -DODR_BOARD_NAME=\"$(BOARD)\"

OPT ?= -Os
COMMON := $(CPUFLAGS) $(OPT) -g3 -ffunction-sections -fdata-sections -fno-common -MMD -MP
CFLAGS_APP := $(COMMON) -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wshadow -Wdouble-promotion $(DEFS) $(INCLUDES)
CFLAGS_3P := $(COMMON) -std=gnu11 -w $(DEFS) $(INCLUDES)
# The C library flavor and linker extras come from the platform (platform.mk); these are the arm-none-eabi ones
LDSPECS ?= --specs=nano.specs --specs=nosys.specs -Wl,--no-warn-rwx-segments
LDFLAGS := $(CPUFLAGS) -T$(LDSCRIPT) -Wl,--gc-sections -Wl,-Map=$(TARGET).map -Wl,--print-memory-usage $(LDSPECS)
# Sections copied into the .bin (one contiguous image from the start of the application)
BIN_SECTIONS ?= .isr_vector .text .rodata .ARM.extab .ARM .preinit_array .init_array .fini_array .data

APP_OBJ := $(APP_SRC:%.c=$(BUILD)/%.o) $(APP_ASM:%.S=$(BUILD)/%.o)
TP_OBJ := $(patsubst %.c,$(BUILD)/%.o,$(filter %.c,$(PLATFORM_3P_SRC))) $(patsubst %.s,$(BUILD)/%.o,$(filter %.s,$(PLATFORM_3P_SRC)))
OBJ := $(APP_OBJ) $(TP_OBJ)

# No built-in suffix rules (the .S -> .s rule deletes sources on case-insensitive file systems, gotchas 13.3)
.SUFFIXES:
.PHONY: all clean size test print-config print-cc
all: $(TARGET).bin

# Rebuild everything when the compile flags change (feature switches and build values), and the objects that carry
# the version when it changes. Checked while the Makefile is read, before make looks at any object: the stale objects
# are removed outright, because make's 1 s timestamps can't order a stamp written in the same second as an object
# (and a file removed by a recipe would still be in make's cache)
VERSION_OBJ := $(patsubst %.c,$(BUILD)/%.o,$(shell grep -l ODR_GIT $(APP_SRC) 2>/dev/null))
ifeq ($(filter clean test print-config print-cc,$(MAKECMDGOALS)),)
$(shell mkdir -p $(BUILD); echo '$(CFLAGS_APP)' | cmp -s - $(BUILD)/flags || { echo '$(CFLAGS_APP)' > $(BUILD)/flags; \
	rm -f $(OBJ); })
$(shell echo '$(ODR_VERSION)' | cmp -s - $(BUILD)/version || { echo '$(ODR_VERSION)' > $(BUILD)/version; \
	rm -f $(VERSION_OBJ); })
endif
$(APP_OBJ) $(TP_OBJ): $(BUILD)/flags
$(VERSION_OBJ): $(BUILD)/version
$(VERSION_OBJ): CFLAGS_APP += -DODR_GIT=\"$(ODR_VERSION)\"

$(TARGET).elf: $(OBJ) $(LDSCRIPT)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) -lm
$(TARGET).bin: $(TARGET).elf
	$(OBJCOPY) -O binary $(addprefix -j ,$(BIN_SECTIONS)) $< $@
	@echo "image $@: $$(wc -c < $@ | tr -d ' ') bytes, version $(ODR_VERSION)"

$(filter %.o,$(APP_SRC:%.c=$(BUILD)/%.o)): $(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_APP) -c $< -o $@
$(BUILD)/third_party/%.o: third_party/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_3P) -c $< -o $@
$(BUILD)/third_party/%.o: third_party/%.s
	@mkdir -p $(dir $@)
	$(CC) $(CPUFLAGS) -c $< -o $@
# Platform assembly (start-up, RTOS port), preprocessed (.S)
$(BUILD)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(CPUFLAGS) $(DEFS) $(INCLUDES) -MMD -MP -c $< -o $@

size: $(TARGET).elf
	$(SIZE) -A $< | grep -E '^\.(isr_vector|text|rodata|rodata2|data|bss|ccmram)'

test:
	$(MAKE) -C tests/host

# For tools/ci.sh: the resolved feature switches (NAME=y|n, one per line) and the compiler this board uses
print-config:
	@$(foreach f,$(FEATURES),echo '$(f)=$(CONFIG_$(f))';)
print-cc:
	@echo '$(CC)'

clean:
	rm -rf $(BUILD)

-include $(OBJ:.o=.d)
