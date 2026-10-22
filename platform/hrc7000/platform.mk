# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
# HR_C7000 (C-SKY CK803S): csky-elf GCC 14 with newlib built for ck803s (tools/build-csky-toolchain.sh), FreeRTOS on
# our port of the CDK CK802 port (platform/hrc7000/freertos)
# ~/toolchains/csky-elf (the build script's default), else csky-elf-gcc on PATH (the CI image), else /usr/bin
CSKY_GCC_DIR ?= $(firstword $(wildcard $(HOME)/toolchains/csky-elf/bin/) \
	$(dir $(shell command -v csky-elf-gcc 2>/dev/null)) /usr/bin/)
CROSS ?= $(CSKY_GCC_DIR)csky-elf-
CC := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
SIZE := $(CROSS)size

# Little endian, no FPU, no DSP, no high registers (r16-r31): the context switch saves r0-r13 and r15 only
CPUFLAGS := -mcpu=ck803s -mlittle-endian -mno-high-registers -msoft-float
# Bytes a task's stack holds beyond its call chain (make stack-check): an interrupt or a yield saves 17 words there
# (freertos/portasm.S), then a handler runs on the interrupt stack with interrupts off, so frames never nest
STACK_OVERHEAD := 68

RTOS := third_party/FreeRTOS-Kernel

$(if $(APP_BASE),,$(error boards/$(BOARD)/board.mk: APP_BASE, the image address, is missing))
# The low SRAM below the codec window (common.ld .ram_lo, zeroed by crt0) holds the state the core marks ODR_CPU_RAM. A
# board whose linker script has no such section sets CPU_RAM_SECTION empty
CPU_RAM_SECTION ?= .ram_lo
MCU_DEFS += $(if $(CPU_RAM_SECTION),-DODR_CPU_RAM_SECTION='"$(CPU_RAM_SECTION)"')
LDSCRIPT ?= boards/$(BOARD)/memory.ld

# drivers/gps/*: the shared GPS receive ring under gps_hrc.c (unreferenced, so linked out, on a radio without a GPS)
PLATFORM_DIRS := platform/hrc7000 platform/hrc7000/freertos drivers/gps/nmea_line drivers/gps/gps_rx
PLATFORM_INC := -I$(RTOS)/include -Iplatform/hrc7000/freertos

PLATFORM_3P_SRC := $(RTOS)/tasks.c $(RTOS)/queue.c $(RTOS)/list.c $(RTOS)/timers.c $(RTOS)/event_groups.c \
	$(RTOS)/portable/MemMang/heap_4.c

# Our own start-up (crt0.S) and syscalls; newlib for memcpy / snprintf and friends
LDSPECS := -nostartfiles --specs=nosys.specs -Wl,--no-warn-rwx-segments
BIN_SECTIONS := .text .rodata .ramfunc .data
