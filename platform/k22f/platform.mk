# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
# NXP Kinetis K22F (Cortex-M4F): arm-none-eabi GCC, hard float, CMSIS core headers, FreeRTOS CM4F port. No vendor SDK:
# the registers are in k22f.h, start-up and vector table in startup.c
ARM_GCC_DIR ?= $(firstword $(wildcard $(HOME)/opt/arm-gnu-toolchain-*-arm-none-eabi/bin/) /usr/bin/)
CROSS ?= $(ARM_GCC_DIR)arm-none-eabi-
CC := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
SIZE := $(CROSS)size

CPUFLAGS := -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard
# Bytes a task's stack holds beyond its call chain (make stack-check): the same Cortex-M4F port as the STM32 radios,
# 26 words of exception frame with the FPU's lazy space plus 1 for alignment, and 25 words of context switch
STACK_OVERHEAD := 208

CMSIS_CORE := third_party/cmsis-core
RTOS := third_party/FreeRTOS-Kernel

PLATFORM_DIRS := platform/k22f
PLATFORM_INC := -I$(CMSIS_CORE)/Include -I$(RTOS)/include -I$(RTOS)/portable/GCC/ARM_CM4F

PLATFORM_3P_SRC := $(RTOS)/tasks.c $(RTOS)/queue.c $(RTOS)/list.c $(RTOS)/timers.c $(RTOS)/event_groups.c \
	$(RTOS)/portable/GCC/ARM_CM4F/port.c $(RTOS)/portable/MemMang/heap_4.c

# The image starts with the vector table (startup.c, .isr_vector)
LDSPECS := --specs=nano.specs --specs=nosys.specs -Wl,--no-warn-rwx-segments
