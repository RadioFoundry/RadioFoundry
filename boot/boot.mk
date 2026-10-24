# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
# A first stage in front of the application (boot/README.md), for a board whose board.mk sets BOOT_STAGE (the
# directory under boot/), BOOT_SIZE (the stage's flash sector; the application starts right after it), BOOT_DEFS and
# BOOT_SIGNATURE (the bytes its loader checks for, hex). The stage sits at APP_BASE: its linker script gets the address
# as __app_base and its size as __boot_size, its code the application's first byte as APP_RESUME. The HR_C7000 entry,
# linker script and protocol are shared (boot/hrc7000); boot/<stage>/boot.c holds the radio's pins. Builds
# $(BUILD)/boot/boot.bin, padded to BOOT_SIZE, and
# $(TARGET)_install.bin: the stage followed by the application, written from the stage's address by the loader's
# update tool or stubtool --write-app.
BOOT_DIR := boot/$(BOOT_STAGE)
BOOT_LD  := boot/hrc7000/stage.ld
BOOT_SRC := boot/hrc7000/entry.S $(BOOT_DIR)/boot.c
BOOT_ELF := $(BUILD)/boot/boot.elf
BOOT_BIN := $(BUILD)/boot/boot.bin

$(BOOT_ELF): $(BOOT_SRC) boot/hrc7000/stage.c $(BOOT_LD) $(BUILD)/flags
	@mkdir -p $(dir $@)
	$(CC) $(CPUFLAGS) -Os -ffreestanding -fno-builtin -nostdlib -Wall -Wextra $(BOOT_DEFS) -DAPP_RESUME=$(APP_START) \
		-Wl,-T,$(BOOT_LD),--no-warn-rwx-segments,--defsym=__app_base=$(APP_BASE),--defsym=__boot_size=$(BOOT_SIZE) \
		-o $@ $(BOOT_SRC)

# The loader checks a signature before it starts an application on a normal power-on: pad.py fails the build when the
# stage lacks it (the radio would stay in update mode) or outgrows its sector
$(BOOT_BIN): $(BOOT_ELF) boot/pad.py
	$(OBJCOPY) -O binary $< $@.raw
	python3 boot/pad.py $@.raw $@ $(BOOT_SIZE) $(BOOT_SIGNATURE)

$(TARGET)_install.bin: $(BOOT_BIN) $(TARGET).bin
	cat $(BOOT_BIN) $(TARGET).bin > $@
	@echo "image $@: $$(wc -c < $@ | tr -d ' ') bytes (first stage + application)"
