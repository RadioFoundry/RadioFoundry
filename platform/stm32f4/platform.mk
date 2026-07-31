# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
# STM32F4: arm-none-eabi GCC, Cortex-M4F hard float, ST HAL + CMSIS, FreeRTOS CM4F port
ARM_GCC_DIR ?= $(firstword $(wildcard $(HOME)/opt/arm-gnu-toolchain-*-arm-none-eabi/bin/) /usr/bin/)
CROSS ?= $(ARM_GCC_DIR)arm-none-eabi-
CC := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
SIZE := $(CROSS)size

CPUFLAGS := -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard

HALDIR := third_party/stm32f4xx-hal-driver
CMSIS_DEV := third_party/cmsis-device-f4
CMSIS_CORE := third_party/cmsis-core
RTOS := third_party/FreeRTOS-Kernel

PLATFORM_DIRS := platform/stm32f4
PLATFORM_INC := -I$(HALDIR)/Inc -I$(CMSIS_DEV)/Include -I$(CMSIS_CORE)/Include -I$(RTOS)/include -I$(RTOS)/portable/GCC/ARM_CM4F

PLATFORM_3P_SRC := $(CMSIS_DEV)/Source/Templates/gcc/startup_stm32f405xx.s \
	$(CMSIS_DEV)/Source/Templates/system_stm32f4xx.c \
	$(foreach m,$(HAL_MODULES),$(HALDIR)/Src/stm32f4xx_$(m).c) \
	$(RTOS)/tasks.c $(RTOS)/queue.c $(RTOS)/list.c $(RTOS)/timers.c $(RTOS)/event_groups.c \
	$(RTOS)/portable/GCC/ARM_CM4F/port.c $(RTOS)/portable/MemMang/heap_4.c
