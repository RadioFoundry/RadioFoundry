# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
# NXP Kinetis K22F (Cortex-M4F): arm-none-eabi GCC, hard float, CMSIS core headers, FreeRTOS CM4F port. No vendor SDK:
# the registers are in k22f.h, start-up and vector table in startup.c. USB on TinyUSB's driver for the full-speed
# controller (usb.c)
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
TUSB := third_party/tinyusb/src

# TinyUSB (MIT), device stack on the full-speed controller (ChipIdea FS, buffer descriptor table); configured by
# drivers/usb/tusb_config.h. Its Kinetis glue includes a device header by name: platform/k22f/tusb has ours
MCU_DEFS += -DCFG_TUSB_MCU=OPT_MCU_KINETIS_K
TUSB_SRC := $(TUSB)/tusb.c $(TUSB)/common/tusb_fifo.c $(TUSB)/device/usbd.c $(TUSB)/class/cdc/cdc_device.c \
	$(TUSB)/portable/chipidea/ci_fs/dcd_ci_fs.c \
	$(if $(filter y,$(CONFIG_USB_NETWORK)),$(TUSB)/class/net/ncm_device.c)

$(if $(APP_BASE),,$(error boards/$(BOARD)/board.mk: APP_BASE, the image address, is missing))
# common.ld's upper SRAM half: zeroed at start-up for ODR_CPU_RAM, left alone for _NOINIT
MCU_DEFS += -DODR_CPU_RAM_SECTION='".ramu_bss"' -DODR_CPU_RAM_NOINIT_SECTION='".ramu_noinit"'
LDSCRIPT ?= boards/$(BOARD)/memory.ld
# The USB device stack, built only with CONFIG_USB
BOARD_USB_DIRS ?= drivers/usb

PLATFORM_DIRS := platform/k22f
PLATFORM_INC := -I$(CMSIS_CORE)/Include -I$(RTOS)/include -I$(RTOS)/portable/GCC/ARM_CM4F \
	$(if $(filter y,$(CONFIG_USB)),-I$(TUSB) -Idrivers/usb -Iplatform/k22f/tusb)

PLATFORM_3P_SRC := $(RTOS)/tasks.c $(RTOS)/queue.c $(RTOS)/list.c $(RTOS)/timers.c $(RTOS)/event_groups.c \
	$(RTOS)/portable/GCC/ARM_CM4F/port.c $(RTOS)/portable/MemMang/heap_4.c \
	$(if $(filter y,$(CONFIG_USB)),$(TUSB_SRC))

# The image starts with the vector table (startup.c, .isr_vector)
LDSPECS := --specs=nano.specs --specs=nosys.specs -Wl,--no-warn-rwx-segments
