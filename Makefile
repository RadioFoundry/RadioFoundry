# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
## Firmware build, one tree for every radio:
##
##   make BOARD=<radio>         -> build/<radio>/rfoundry_<radio>.{elf,bin,map}, plus rfoundry_<radio>_codec.bin when the
##                                 radio's stock firmware is in codecs/ (codecs/README.md)
##   make BOARD=<radio> size    -> section sizes and the space left before the codec
##   make test                  -> host tests (no BOARD needed)
##   make                       -> lists the boards
##
## Each board says its platform, image address and source directories in boards/<board>/board.mk, and how it
## differs from its platform's defaults (platform/<platform>/platform.mk, config.mk, common.ld, FreeRTOSConfig.h).
## Every *.c in the listed directories is built, so adding a file needs no Makefile change.

# The flashing tools' view of the board registry (tools/boards.py), regenerated when one of its sources changes. Every
# build and the host tests depend on it, so a board.toml edit reaches rfoundry without a separate step. Without a
# Python 3.11 (tools/rfoundry_python.sh) it stays as it is, with a note; tools/boards.py check fails on a stale copy
REGISTRY_JSON := tools/rfoundry/rfoundry/registry.json
REGISTRY_SRC := $(wildcard boards/*/board.toml boards/*/board.mk boards/*/config.mk boards/*/*.ld boards/*/*.c \
	boards/*/pins.h platform/*/platform.toml platform/*/platform.mk platform/*/config.mk) tools/boards.py
RFOUNDRY_PYTHON = $(shell tools/rfoundry_python.sh 2>/dev/null)
$(REGISTRY_JSON): $(REGISTRY_SRC)
	@py="$(RFOUNDRY_PYTHON)"; if [ -n "$$py" ]; then "$$py" tools/boards.py json --for rfoundry --out $@ && touch $@; \
	else echo "note: $@ is older than the board registry; no Python 3.11 to regenerate it (tools/rfoundry_python.sh)"; fi
registry: $(REGISTRY_JSON)
.PHONY: registry
# the rule above is the first in the file: plain make still means all
.DEFAULT_GOAL := all

# No default radio: an image for the wrong one is easy to miss. Without BOARD, only the host tests run; anything else
# lists the boards
ifeq ($(BOARD),)
test: $(REGISTRY_JSON)
	$(MAKE) -C tests/host
.PHONY: test
ifneq ($(filter-out test registry,$(or $(MAKECMDGOALS),all)),)
$(error say which radio: make BOARD=<board>, one of: $(sort $(patsubst boards/%/board.mk,%,$(wildcard boards/*/board.mk))))
endif
else
include boards/$(BOARD)/board.mk
# A release variant (config/variants/<name>.mk): a preset that turns features off. Read before the board's defaults,
# so it wins over them and the command line wins over it. Empty: the full default build (docs/build-config.md)
VARIANT ?=
ifneq ($(VARIANT),)
ifeq ($(wildcard config/variants/$(VARIANT).mk),)
$(error unknown VARIANT=$(VARIANT): the variants are $(basename $(notdir $(wildcard config/variants/*.mk))))
endif
include config/variants/$(VARIANT).mk
endif
-include boards/$(BOARD)/config.mk
# The platform's defaults, after the board's: ?= keeps the first value, so the board's wins
-include platform/$(PLATFORM)/config.mk
include config/features.mk
include platform/$(PLATFORM)/platform.mk

# Hardware build options: hardware versions one image can't serve (a radio sold with UHF and VHF boards). board.mk names
# them in BUILD_OPTIONS and lists each one's values in <NAME>_VALUES, the default first; board.toml [[build_options]]
# only adds the words people read (tools/boards.py check holds the two to the same values). Each value reaches C as
# RFOUNDRY_<NAME>_<VALUE> = 0 or 1 (RFOUNDRY_BAND_VHF). A non-default value names the build directory and the image
# (build/md380-vhf/rfoundry_md380-vhf.bin), the names tools/release.py and rfoundry look for
upper = $(shell echo '$1' | tr a-z- A-Z_)
$(foreach o,$(BUILD_OPTIONS),$(eval $(o) ?= $(firstword $($(o)_VALUES))))
$(foreach o,$(BUILD_OPTIONS),$(if $(filter $($(o)),$($(o)_VALUES)),,$(error $(o)=$($(o)) is not a $(BOARD) build: \
	the values are $($(o)_VALUES))))
OPTION_SUFFIX := $(subst $() ,,$(foreach o,$(BUILD_OPTIONS),$(if $(filter-out $(firstword $($(o)_VALUES)),$($(o))),-$($(o)))))
OPTION_DEFS := $(foreach o,$(BUILD_OPTIONS),$(foreach x,$($(o)_VALUES),-DRFOUNDRY_$(call upper,$(o)_$(x))=$(if $(filter $(x),$($(o))),1,0)))
MCU_DEFS += $(OPTION_DEFS)

BUILD := build/$(BOARD)$(if $(VARIANT),-$(VARIANT))$(OPTION_SUFFIX)
TARGET := $(BUILD)/rfoundry_$(BOARD)$(OPTION_SUFFIX)

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
ifeq ($(CONFIG_CPS_OPENFW),y)
CORE_DIRS += core/cps
endif
ifeq ($(CONFIG_DEBUGLINK),y)
CORE_DIRS += core/debuglink
endif
ifeq ($(CONFIG_STUB_ENTRY),y)
CORE_DIRS += core/stubentry
endif
ifeq ($(CONFIG_APRS),y)
CORE_DIRS += core/aprs
endif
ifeq ($(CONFIG_SNMP),y)
CORE_DIRS += core/snmp
endif
ifeq ($(CONFIG_CRASH_LOG),y)
CORE_DIRS += core/crash
endif
# The storage layer over SPI NAND, for the boards that list it (BOARD_NAND_DIRS in their board.mk)
ifeq ($(CONFIG_NAND_STORAGE),y)
BOARD_DIRS += $(BOARD_NAND_DIRS)
endif
APP_DIRS := $(CORE_DIRS) $(PLATFORM_DIRS) $(BOARD_DIRS)
APP_SRC := $(foreach d,$(APP_DIRS),$(wildcard $(d)/*.c))
APP_ASM := $(foreach d,$(APP_DIRS),$(wildcard $(d)/*.S))
# The stub entry without the debug link: its listener uses the debug link's framing (dl_frame.c, no RTOS or board)
ifeq ($(CONFIG_STUB_ENTRY)$(CONFIG_DEBUGLINK),yn)
APP_SRC += core/debuglink/dl_frame.c
endif
# The fast codeplug protocol (core/cps is globbed with CPS_OPENFW): only with its own switch
ifneq ($(CONFIG_CPS_FAST),y)
APP_SRC := $(filter-out core/cps/cps_fast.c core/cps/cps_lz4.c,$(APP_SRC))
endif
# The task table behind the debug link's tasks view (core/app is always globbed): only with its switch, which also
# turns on the kernel's trace hooks in the platforms' FreeRTOSConfig.h
ifneq ($(CONFIG_TASK_STACKS),y)
APP_SRC := $(filter-out core/app/task_stack.c,$(APP_SRC))
endif
# The idle task's watchdog feed (core/app is always globbed): only with its switch, which also turns on the idle hook in
# the platforms' FreeRTOSConfig.h
ifneq ($(CONFIG_WATCHDOG),y)
APP_SRC := $(filter-out core/app/watchdog.c,$(APP_SRC))
endif
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
# The fault trace line a fault handler prints before the restart (core/crash is globbed with CRASH_LOG): only with its
# own switch, with or without the crash log
APP_SRC := $(filter-out core/crash/crash_trace.c,$(APP_SRC))
ifeq ($(CONFIG_FAULT_TRACE),y)
APP_SRC += core/crash/crash_trace.c
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
NET_SRC-$(CONFIG_NET_WEBCTL) += core/net/ws.c
NET_SRC-$(CONFIG_MDNS) += core/net/net.c core/net/net_services.c core/net/mdns.c
NET_SRC-$(CONFIG_TFTP) += core/net/net.c core/net/net_services.c core/net/tftp.c core/net/tftp_codeplug.c
NET_SRC-$(CONFIG_TFTP_IDDB) += core/net/tftp_iddb.c
NET_SRC-$(CONFIG_NET_USRP) += core/net/net.c core/usbaudio/usrp_net.c core/usbaudio/usrp.c
APP_SRC += $(sort $(NET_SRC-y))
# The PCM stream shared between the radio's users, a host audio session (USRP) and APRS receive
MUX_SRC-$(CONFIG_NET_USRP) += core/usbaudio/audio_mux.c core/usbaudio/pcm_slip.c
MUX_SRC-$(CONFIG_APRS_RX) += core/usbaudio/audio_mux.c core/usbaudio/pcm_slip.c
APP_SRC += $(sort $(MUX_SRC-y))
# Their text formatting (key=value views, JSON, protocol lines) is the debug link's dl_fmt.c, built here without it.
# The crash log's summary line uses it too
ifneq ($(NET_SRC-y)$(filter y,$(CONFIG_CRASH_LOG)),)
ifneq ($(CONFIG_DEBUGLINK),y)
APP_SRC += core/debuglink/dl_fmt.c
endif
endif

INCLUDES := -Ihal -Icore -Idrivers -Iboards/$(BOARD) -Iplatform/$(PLATFORM) -I$(BUILD)/gen $(PLATFORM_INC)

# Firmware version: the commit, with -dirty when tracked files differ from it or untracked sources sit in the built
# directories (both end up in the image). Only the files that use RFOUNDRY_GIT get it, through their own stamp, so a new
# commit or an edit rebuilds those few objects, not everything.
GITVERSION := $(shell git rev-parse --short=8 HEAD 2>/dev/null || echo unknown)
ifneq ($(GITVERSION),unknown)
GITDIRTY := $(shell git update-index -q --refresh >/dev/null 2>&1; git diff-index --quiet --ignore-submodules=all HEAD -- \
	2>/dev/null || echo -dirty)
ifeq ($(GITDIRTY),)
GITDIRTY := $(if $(shell git ls-files --others --exclude-standard -- $(APP_DIRS) config Makefile 2>/dev/null),-dirty)
endif
endif
RFOUNDRY_VERSION := $(GITVERSION)$(GITDIRTY)
# The same as the crash record's build number and dirty flag (core/crash/crash.c): the version's leading hex digits
# (at most 8, then its end or "-") as a number, 0 when it isn't a commit. Worked out here, not in the image
RFOUNDRY_GIT_HEAD := $(firstword $(subst -, ,$(RFOUNDRY_VERSION)))
RFOUNDRY_GIT_ID := $(if $(shell printf '%s' '$(RFOUNDRY_GIT_HEAD)' | grep -Eqx '[0-9a-fA-F]{1,8}' && echo y),0x$(RFOUNDRY_GIT_HEAD)u,0u)
RFOUNDRY_GIT_DIRTY := $(if $(findstring -dirty,$(RFOUNDRY_VERSION)),1,0)
# RFOUNDRY_BOARD_ID: the board's id (boards/<id>), for platform code that names things after the board (the HR_C7000 codec
# host's profile ids, "<id>-<stock tag>"); core/ never uses it
DEFS := $(MCU_DEFS) $(FEATURE_DEFS) -DRFOUNDRY_BOARD_ID=\"$(BOARD)\"

OPT ?= -Os
# Every function's frame size and calls, next to each object (or each LTO partition, next to the .elf), for the stack
# check (make stack-check, tools/stack_check.py). Output files only: the image is the same with or without it
STACK_INFO := -fcallgraph-info=su
COMMON := $(CPUFLAGS) $(OPT) -g3 -ffunction-sections -fdata-sections -fno-common -MMD -MP $(STACK_INFO)
CFLAGS_APP := $(COMMON) -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wshadow -Wdouble-promotion $(DEFS) $(INCLUDES)
CFLAGS_3P := $(COMMON) -std=gnu11 -w $(DEFS) $(INCLUDES)
# The C library flavor and linker extras come from the platform (platform.mk); these are the arm-none-eabi ones
LDSPECS ?= --specs=nano.specs --specs=nosys.specs -Wl,--no-warn-rwx-segments
# The image address (board.mk APP_BASE) reaches the linker scripts as __app_base, and the application's first byte, after
# a first stage when there is one, as __app_start. A board's memory.ld INCLUDEs the platform's common.ld (-L)
APP_START := $(APP_BASE)$(if $(BOOT_STAGE),+$(BOOT_SIZE))
LDFLAGS := $(CPUFLAGS) -T$(LDSCRIPT) -Lplatform/$(PLATFORM) -Wl,--defsym=__app_base=$(APP_BASE) \
	-Wl,--defsym=__app_start=$(APP_START) -Wl,--gc-sections -Wl,-Map=$(TARGET).map -Wl,--print-memory-usage $(LDSPECS)
# Link time optimization: LTO=1 compiles our sources to GCC's IR and optimizes them together at the link (inlining and
# dead code across files: about 17 KB smaller on the STM32 radios, 20 KB with LTO_3P). A board's config.mk may default
# it (some STM32 radios do: without it their images don't hold their features). LTO_3P=1 takes the
# third-party code (HAL, CMSIS, FreeRTOS, TinyUSB) along: FreeRTOS marks what its port's inline assembly reaches portDONT_DISCARD, and the
# vectors and SystemInit are referenced from the assembly start-up, which the linker sees. Files with file-scope
# assembly are never in it (LTO_EXCLUDE). The optimization flags go to the link too, where the code is generated
LTO ?= 0
LTO_3P ?= 0
LTO_EXCLUDE ?= platform/stm32f4/codec_call.c
ifeq ($(LTO),1)
CFLAGS_APP += -flto=auto
LDFLAGS += $(OPT) -flto=auto $(STACK_INFO)
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
.PHONY: all codec clean size stack-check test print-config print-cc print-unsupported print-not-written print-required print-maximal-skip print-maximal-vars
all: $(REGISTRY_JSON) $(TARGET).bin $(if $(BOOT_STAGE),$(TARGET)_install.bin) codec

# The codec: merged into $(TARGET)_codec.bin when the radio's stock firmware is in codecs/ (codecs/README.md)
# A board with a first stage (BOOT_STAGE) merges the codec into the install image, so the merged file starts with the stage
codec: $(TARGET).bin $(if $(BOOT_STAGE),$(TARGET)_install.bin)
	@python3 tools/codec_auto.py --board $(BOARD) --image $(if $(BOOT_STAGE),$(TARGET)_install.bin,$(TARGET).bin) \
		$(if $(BOOT_STAGE),-o $(TARGET)_codec.bin)

# Rebuild everything when the compile flags change (feature switches and build values), and the objects that carry
# the version when it changes. Checked while the Makefile is read, before make looks at any object: the stale objects
# are removed outright, because make's 1 s timestamps can't order a stamp written in the same second as an object
# (and a file removed by a recipe would still be in make's cache)
VERSION_OBJ := $(patsubst %.c,$(BUILD)/%.o,$(shell grep -l RFOUNDRY_GIT $(APP_SRC) 2>/dev/null))
ifeq ($(filter clean test print-config print-cc print-unsupported print-not-written print-required print-maximal-skip print-maximal-vars \
	print-vars,$(MAKECMDGOALS)),)
$(shell mkdir -p $(BUILD); echo '$(CFLAGS_APP) $(CFLAGS_3P) $(LDFLAGS)' | cmp -s - $(BUILD)/flags || \
	{ echo '$(CFLAGS_APP) $(CFLAGS_3P) $(LDFLAGS)' > $(BUILD)/flags; \
	rm -f $(OBJ); })
$(shell echo '$(RFOUNDRY_VERSION)' | cmp -s - $(BUILD)/version || { echo '$(RFOUNDRY_VERSION)' > $(BUILD)/version; \
	rm -f $(VERSION_OBJ); })
# The About page's authors and third-party list (tools/gen_about.py), and the objects that include it when it changed
ABOUT_OBJ := $(patsubst %.c,$(BUILD)/%.o,$(shell grep -l rfoundry_about.h $(APP_SRC) 2>/dev/null))
$(shell python3 tools/gen_about.py AUTHORS platform/$(PLATFORM)/THIRD_PARTY $(BUILD)/gen/rfoundry_about.h | grep -q changed \
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
$(VERSION_OBJ): CFLAGS_APP += -DRFOUNDRY_GIT=\"$(RFOUNDRY_VERSION)\" -DRFOUNDRY_GIT_ID=$(RFOUNDRY_GIT_ID) -DRFOUNDRY_GIT_DIRTY=$(RFOUNDRY_GIT_DIRTY)
ifeq ($(LTO),1)
$(patsubst %.c,$(BUILD)/%.o,$(filter $(LTO_EXCLUDE),$(APP_SRC))): CFLAGS_APP += -fno-lto
endif

$(TARGET).elf: $(OBJ) $(LDSCRIPT) $(wildcard platform/$(PLATFORM)/*.ld)
	@rm -f $@.ltrans*.ci	# LTO's call graphs, one per partition: none left from a link with more partitions
	$(CC) $(LDFLAGS) -o $@ $(OBJ) -lm
$(TARGET).bin: $(TARGET).elf
	$(OBJCOPY) -O binary $(addprefix -j ,$(BIN_SECTIONS)) $< $@
	@echo "image $@: $$(wc -c < $@ | tr -d ' ') bytes, version $(RFOUNDRY_VERSION)"

# A first stage in front of the application (boot/), for a board that sets BOOT_STAGE
ifneq ($(BOOT_STAGE),)
include boot/boot.mk
endif

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
	$(SIZE) -A $< | grep -E '^\.(isr_vector|text|rodata|data|bss|ccmram)'

test: $(REGISTRY_JSON)
	$(MAKE) -C tests/host

# Static worst-case stack depth of every task against its stack (docs/dev/stack-check.md). The platform gives the bytes
# its RTOS port saves on a task stack (STACK_OVERHEAD); STACK_MARGIN bytes must stay free; a board still bringing up its
# call graph may set STACK_CHECK_REPORT_ONLY=1 in its config.mk
STACK_MARGIN ?= 128
STACK_CHECK_REPORT_ONLY ?= 0
stack-check: $(TARGET).elf
	@python3 tools/stack_check.py --build $(BUILD) --label $(BOARD) --cc '$(CC)' --cflags '$(CFLAGS_APP)' \
		--objdump $(CROSS)objdump --elf $(TARGET).elf \
		--overhead $(STACK_OVERHEAD) --margin $(STACK_MARGIN) $(if $(filter 1,$(STACK_CHECK_REPORT_ONLY)),--report-only) \
		$(STACK_CHECK_ARGS)

# For tools/ci.sh: the resolved feature switches (NAME=y|n, one per line) and the compiler this board uses
print-config:
	@$(foreach f,$(FEATURES),echo '$(f)=$(CONFIG_$(f))';)
print-cc:
	@echo '$(CC)'
# Switches the radio's hardware can't do (boards/<board>/config.mk BOARD_UNSUPPORTED, one a line with its reason;
# strip drops the spaces before each comment) and those its hardware can do but whose driver or glue isn't written
# yet (BOARD_NOT_WRITTEN, the same form; docs/build-config.md): ci's maximal configuration leaves both off
print-unsupported:
	@echo '$(strip $(BOARD_UNSUPPORTED))'
print-not-written:
	@echo '$(strip $(BOARD_NOT_WRITTEN))'
# Switches the board can't build without (boards/<board>/config.mk BOARD_REQUIRED): ci builds no "no-" configuration
# for them
print-required:
	@echo '$(BOARD_REQUIRED)'
# Switches the board supports but has no need of, which ci's maximal configuration also leaves off
# (boards/<board>/config.mk MAXIMAL_SKIP)
print-maximal-skip:
	@echo '$(MAXIMAL_SKIP)'
# Make variables ci's maximal configuration adds for this board (boards/<board>/config.mk MAXIMAL_VARS)
print-maximal-vars:
	@echo '$(MAXIMAL_VARS)'
# NAME=value for each make variable in VARS, as this build resolves it (tools/boards.py reads board.mk through it, so
# conditionals and config.mk count)
print-build-options:
	@:$(foreach o,$(BUILD_OPTIONS),$(info $(o)=$($(o)_VALUES)))
print-vars:
	@:$(foreach v,$(VARS),$(info $(v)=$($(v))))

clean:
	rm -rf $(BUILD)

-include $(OBJ:.o=.d)
endif
