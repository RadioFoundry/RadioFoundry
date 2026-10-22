# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The OpenDigitalRadio Authors
# STM32F4: arm-none-eabi GCC, Cortex-M4F hard float, ST HAL + CMSIS, FreeRTOS CM4F port
ARM_GCC_DIR ?= $(firstword $(wildcard $(HOME)/opt/arm-gnu-toolchain-*-arm-none-eabi/bin/) /usr/bin/)
CROSS ?= $(ARM_GCC_DIR)arm-none-eabi-
CC := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
SIZE := $(CROSS)size

CPUFLAGS := -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard
# Bytes a task's stack holds beyond its call chain (make stack-check): interrupts run on the main stack (MSP), but the
# exception entry stacks its frame on the task's stack, 26 words with the FPU's lazy space plus 1 for alignment, and the
# port's context switch adds r4-r11, lr and s16-s31 (25 words): 52 words
STACK_OVERHEAD := 208

HALDIR := third_party/stm32f4xx-hal-driver
CMSIS_DEV := third_party/cmsis-device-f4
CMSIS_CORE := third_party/cmsis-core
RTOS := third_party/FreeRTOS-Kernel
TUSB := third_party/tinyusb/src

# The board's part (board.mk MCU): its CMSIS device define and start-up file
ifeq ($(MCU),stm32f405)
MCU_DEFS += -DSTM32F405xx
MCU_STARTUP := $(CMSIS_DEV)/Source/Templates/gcc/startup_stm32f405xx.s
else
$(error boards/$(BOARD)/board.mk: MCU must name a supported part (stm32f405), not "$(MCU)")
endif
$(if $(HSE_VALUE),,$(error boards/$(BOARD)/board.mk: HSE_VALUE, the crystal in Hz, is missing))
$(if $(APP_BASE),,$(error boards/$(BOARD)/board.mk: APP_BASE, the image address, is missing))
# The vector table is the image's first word: SystemInit sets VTOR = FLASH_BASE | VECT_TAB_OFFSET (FLASH_BASE is the
# CMSIS device header's; - binds tighter than |). No parentheses or spaces: the flags file is written by a shell echo
MCU_DEFS += -DHSE_VALUE=$(HSE_VALUE) -DUSER_VECT_TAB_ADDRESS -DVECT_TAB_OFFSET=$(APP_BASE)u-FLASH_BASE
# common.ld's CPU-only RAM (CCM: no DMA): zeroed at start-up for ODR_CPU_RAM, left alone for _NOINIT
MCU_DEFS += -DODR_CPU_RAM_SECTION='".ccmbss"' -DODR_CPU_RAM_NOINIT_SECTION='".ccmram"'
LDSCRIPT ?= boards/$(BOARD)/memory.ld

# The USB device stack, built only with CONFIG_USB
BOARD_USB_DIRS ?= drivers/usb
# The ST HAL modules every board builds, in link order, and the optional ones a board adds with BOARD_HAL_MODULES. The
# order is fixed so the image doesn't depend on the order a board lists its extras in
HAL_ORDER := hal hal_cortex hal_rcc hal_rcc_ex hal_gpio hal_pwr hal_pwr_ex hal_flash hal_flash_ex hal_dma hal_i2c \
	hal_spi hal_tim hal_tim_ex hal_adc hal_adc_ex hal_dac hal_dac_ex hal_sram ll_fsmc
HAL_OPTIONAL := hal_i2c
$(foreach m,$(filter-out $(HAL_OPTIONAL),$(BOARD_HAL_MODULES)),$(error BOARD_HAL_MODULES: $(m) is not one of $(HAL_OPTIONAL)))
HAL_MODULES := $(filter-out $(filter-out $(BOARD_HAL_MODULES),$(HAL_OPTIONAL)),$(HAL_ORDER))

# TinyUSB (MIT), device stack on the OTG_FS core (Synopsys DWC2); configured by drivers/usb/tusb_config.h
MCU_DEFS += -DCFG_TUSB_MCU=OPT_MCU_STM32F4
TUSB_SRC := $(TUSB)/tusb.c $(TUSB)/common/tusb_fifo.c $(TUSB)/device/usbd.c \
	$(TUSB)/class/cdc/cdc_device.c $(TUSB)/portable/synopsys/dwc2/dcd_dwc2.c $(TUSB)/portable/synopsys/dwc2/dwc2_common.c \
	$(if $(filter y,$(CONFIG_USB_NETWORK)),$(TUSB)/class/net/ncm_device.c)

PLATFORM_DIRS := platform/stm32f4
PLATFORM_INC := -I$(HALDIR)/Inc -I$(CMSIS_DEV)/Include -I$(CMSIS_CORE)/Include -I$(RTOS)/include -I$(RTOS)/portable/GCC/ARM_CM4F \
	-I$(TUSB) -Idrivers/usb

PLATFORM_3P_SRC := $(MCU_STARTUP) \
	$(CMSIS_DEV)/Source/Templates/system_stm32f4xx.c \
	$(foreach m,$(HAL_MODULES),$(HALDIR)/Src/stm32f4xx_$(m).c) \
	$(RTOS)/tasks.c $(RTOS)/queue.c $(RTOS)/list.c $(RTOS)/timers.c $(RTOS)/event_groups.c \
	$(RTOS)/portable/GCC/ARM_CM4F/port.c $(RTOS)/portable/MemMang/heap_4.c \
	$(if $(filter y,$(CONFIG_USB)),$(TUSB_SRC))
