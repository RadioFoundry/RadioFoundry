# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
# AT32F43x: arm-none-eabi GCC, Cortex-M4F hard float, register-level platform code (at32f43x.h) + CMSIS core,
# FreeRTOS CM4F port, TinyUSB's dwc2 driver on the OTGFS1 core
ARM_GCC_DIR ?= $(firstword $(wildcard $(HOME)/opt/arm-gnu-toolchain-*-arm-none-eabi/bin/) /usr/bin/)
CROSS ?= $(ARM_GCC_DIR)arm-none-eabi-
CC := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
SIZE := $(CROSS)size

CPUFLAGS := -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard
# Bytes a task's stack holds beyond its call chain (make stack-check): as on the STM32F4 (platform/stm32f4/platform.mk)
STACK_OVERHEAD := 208

CMSIS_CORE := third_party/cmsis-core
RTOS := third_party/FreeRTOS-Kernel
TUSB := third_party/tinyusb/src

# The board's part (board.mk MCU)
ifeq ($(MCU),at32f435)
else ifeq ($(MCU),at32f437)
else
$(error boards/$(BOARD)/board.mk: MCU must name a supported part (at32f435, at32f437), not "$(MCU)")
endif
$(if $(APP_BASE),,$(error boards/$(BOARD)/board.mk: APP_BASE, the image address, is missing))
LDSCRIPT ?= boards/$(BOARD)/memory.ld

# The USB device stack, built only with CONFIG_USB. TinyUSB reads at32f435_437.h (here) for the core's interrupts
BOARD_USB_DIRS ?= drivers/usb
MCU_DEFS += -DCFG_TUSB_MCU=OPT_MCU_AT32F435_437
TUSB_SRC := $(TUSB)/tusb.c $(TUSB)/common/tusb_fifo.c $(TUSB)/device/usbd.c \
	$(TUSB)/class/cdc/cdc_device.c $(TUSB)/portable/synopsys/dwc2/dcd_dwc2.c \
	$(TUSB)/portable/synopsys/dwc2/dwc2_common.c \
	$(if $(filter y,$(CONFIG_USB_NETWORK)),$(TUSB)/class/net/ncm_device.c)

PLATFORM_DIRS := platform/at32f43x
PLATFORM_INC := -I$(CMSIS_CORE)/Include -I$(RTOS)/include -I$(RTOS)/portable/GCC/ARM_CM4F -I$(TUSB) -Idrivers/usb

PLATFORM_3P_SRC := $(RTOS)/tasks.c $(RTOS)/queue.c $(RTOS)/list.c $(RTOS)/timers.c $(RTOS)/event_groups.c \
	$(RTOS)/portable/GCC/ARM_CM4F/port.c $(RTOS)/portable/MemMang/heap_4.c \
	$(if $(filter y,$(CONFIG_USB)),$(TUSB_SRC))
