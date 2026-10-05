# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
## Firmware build, one tree for every radio:
##
##   make BOARD=dm1701          -> build/dm1701/odr_dm1701.{elf,bin,map}, plus odr_dm1701_codec.bin when the
##                                 radio's stock firmware is in codecs/ (codecs/README.md)
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
ifeq ($(CONFIG_BCAST_FM),y)
CORE_DIRS += core/bcast
endif
ifeq ($(CONFIG_APRS),y)
CORE_DIRS += core/aprs
endif
ifeq ($(CONFIG_SNMP),y)
CORE_DIRS += core/snmp
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
# The split duplex repeater: the continuous BS outbound and the RX half's slot labels (the hotspot's extension is in
# core/hotspot/hotspotSplit.c, the CACH coding in core/dmr/fec)
ifeq ($(CONFIG_HOTSPOT_SPLIT),y)
APP_SRC += core/dmr/bsOutbound.c core/dmr/slotEpoch.c
endif
# The IP gateway (the data service above carries its packets: CONFIG_DMR_DATA_CORE follows it)
ifeq ($(CONFIG_DMR_IP_GATEWAY),y)
APP_SRC += core/dmr/ipGateway.c
endif
# PPP on the serial host port (its host link glue is core/hostlink/hostlink_ppp.c)
ifeq ($(CONFIG_NET_PPP),y)
APP_SRC += core/net/ppp.c
endif
# The radio's own services on the USB network link (core/net): net.c starts and polls them all, net_services.c is the
# UDP services' glue to the gateway; each service's file only with its switch
NET_SRC-$(CONFIG_NET_TCP) += core/net/net.c core/net/tcp.c core/net/http.c core/net/kiss_tcp.c core/net/rigctld.c \
	core/net/aprsis.c
NET_SRC-$(CONFIG_MDNS) += core/net/net.c core/net/net_services.c core/net/mdns.c
NET_SRC-$(CONFIG_TFTP) += core/net/net.c core/net/net_services.c core/net/tftp.c core/net/tftp_codeplug.c
APP_SRC += $(sort $(NET_SRC-y))
# Their text formatting (key=value views, JSON, protocol lines) is the debug link's dl_fmt.c, built here without it
ifneq ($(NET_SRC-y),)
ifneq ($(CONFIG_DEBUGLINK),y)
APP_SRC += core/debuglink/dl_fmt.c
endif
endif

INCLUDES := -Ihal -Icore -Idrivers -Iboards/$(BOARD) -Iplatform/$(PLATFORM) -I$(BUILD)/gen $(PLATFORM_INC)

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
# Link time optimization: LTO=1 compiles our sources to GCC's IR and optimizes them together at the link (inlining and
# dead code across files: about 17 KB smaller on the STM32 radios, 20 KB with LTO_3P). A board's config.mk may default
# it (the MD-380 does: without it the image doesn't hold its features); the DM-1701 keeps LTO=0 until it has been
# tested on the radio, but its maximal CI build uses it (MAXIMAL_VARS). LTO_3P=1 takes the third-party code (HAL,
# CMSIS, FreeRTOS, TinyUSB) along: FreeRTOS marks what its port's inline assembly reaches portDONT_DISCARD, and the
# vectors and SystemInit are referenced from the assembly start-up, which the linker sees. Files with file-scope
# assembly are never in it (LTO_EXCLUDE). The optimization flags go to the link too, where the code is generated
LTO ?= 0
LTO_3P ?= 0
LTO_EXCLUDE ?= platform/stm32f4/codec_call.c
ifeq ($(LTO),1)
CFLAGS_APP += -flto=auto
LDFLAGS += $(OPT) -flto=auto
ifeq ($(LTO_3P),1)
CFLAGS_3P += -flto=auto
endif
endif
# Sections copied into the .bin (one contiguous image from the start of the application)
BIN_SECTIONS ?= .isr_vector .text .rodata .ARM.extab .ARM .preinit_array .init_array .fini_array .data

APP_OBJ := $(APP_SRC:%.c=$(BUILD)/%.o) $(APP_ASM:%.S=$(BUILD)/%.o)
TP_OBJ := $(patsubst %.c,$(BUILD)/%.o,$(filter %.c,$(PLATFORM_3P_SRC))) $(patsubst %.s,$(BUILD)/%.o,$(filter %.s,$(PLATFORM_3P_SRC)))
OBJ := $(APP_OBJ) $(TP_OBJ)

# No built-in suffix rules (the .S -> .s rule deletes sources on case-insensitive file systems, gotchas 13.3)
.SUFFIXES:
.PHONY: all codec clean size test print-config print-cc print-unsupported print-maximal-skip print-maximal-vars
all: $(TARGET).bin codec

# The codec: merged into $(TARGET)_codec.bin when the radio's stock firmware is in codecs/ (codecs/README.md)
codec: $(TARGET).bin
	@python3 tools/codec_auto.py --board $(BOARD) --image $<

# Rebuild everything when the compile flags change (feature switches and build values), and the objects that carry
# the version when it changes. Checked while the Makefile is read, before make looks at any object: the stale objects
# are removed outright, because make's 1 s timestamps can't order a stamp written in the same second as an object
# (and a file removed by a recipe would still be in make's cache)
VERSION_OBJ := $(patsubst %.c,$(BUILD)/%.o,$(shell grep -l ODR_GIT $(APP_SRC) 2>/dev/null))
ifeq ($(filter clean test print-config print-cc print-unsupported print-maximal-skip print-maximal-vars,$(MAKECMDGOALS)),)
$(shell mkdir -p $(BUILD); echo '$(CFLAGS_APP) $(CFLAGS_3P) $(LDFLAGS)' | cmp -s - $(BUILD)/flags || \
	{ echo '$(CFLAGS_APP) $(CFLAGS_3P) $(LDFLAGS)' > $(BUILD)/flags; \
	rm -f $(OBJ); })
$(shell echo '$(ODR_VERSION)' | cmp -s - $(BUILD)/version || { echo '$(ODR_VERSION)' > $(BUILD)/version; \
	rm -f $(VERSION_OBJ); })
# The About page's authors and third-party list (tools/gen_about.py), and the objects that include it when it changed
ABOUT_OBJ := $(patsubst %.c,$(BUILD)/%.o,$(shell grep -l odr_about.h $(APP_SRC) 2>/dev/null))
$(shell python3 tools/gen_about.py AUTHORS platform/$(PLATFORM)/THIRD_PARTY $(BUILD)/gen/odr_about.h | grep -q changed \
	&& rm -f $(ABOUT_OBJ))
# The HTTP status server's static files (core/net/http_assets/*, tools/gen_http_assets.py): a table in the gen
# directory, gzip compressed with CONFIG_NET_HTTP_GZIP; http.o is rebuilt when it changed
ifeq ($(CONFIG_NET_HTTP),y)
HTTP_ASSETS := $(sort $(wildcard core/net/http_assets/*))
$(shell python3 tools/gen_http_assets.py $(if $(filter y,$(CONFIG_NET_HTTP_GZIP)),--gzip) $(BUILD)/gen/http_assets_gen.h \
	$(HTTP_ASSETS) | grep -q changed && rm -f $(BUILD)/core/net/http.o)
endif
endif
$(APP_OBJ) $(TP_OBJ): $(BUILD)/flags
$(VERSION_OBJ): $(BUILD)/version
$(VERSION_OBJ): CFLAGS_APP += -DODR_GIT=\"$(ODR_VERSION)\"
ifeq ($(LTO),1)
$(patsubst %.c,$(BUILD)/%.o,$(filter $(LTO_EXCLUDE),$(APP_SRC))): CFLAGS_APP += -fno-lto
endif

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
# Switches the board's hardware support doesn't cover (boards/<board>/config.mk BOARD_UNSUPPORTED): ci's maximal
# configuration leaves them off
print-unsupported:
	@echo '$(BOARD_UNSUPPORTED)'
# Switches the board supports but has no need of, which ci's maximal configuration also leaves off
# (boards/<board>/config.mk MAXIMAL_SKIP)
print-maximal-skip:
	@echo '$(MAXIMAL_SKIP)'
# Make variables ci's maximal configuration adds for this board (boards/<board>/config.mk MAXIMAL_VARS)
print-maximal-vars:
	@echo '$(MAXIMAL_VARS)'

clean:
	rm -rf $(BUILD)

-include $(OBJ:.o=.d)
