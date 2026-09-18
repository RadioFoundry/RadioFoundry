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

RTOS := third_party/FreeRTOS-Kernel

PLATFORM_DIRS := platform/hrc7000 platform/hrc7000/freertos
PLATFORM_INC := -I$(RTOS)/include -Iplatform/hrc7000/freertos

PLATFORM_3P_SRC := $(RTOS)/tasks.c $(RTOS)/queue.c $(RTOS)/list.c $(RTOS)/timers.c $(RTOS)/event_groups.c \
	$(RTOS)/portable/MemMang/heap_4.c

# Our own start-up (crt0.S) and syscalls; newlib for memcpy / snprintf and friends
LDSPECS := -nostartfiles --specs=nosys.specs -Wl,--no-warn-rwx-segments
BIN_SECTIONS := .text .rodata .ramfunc .data
