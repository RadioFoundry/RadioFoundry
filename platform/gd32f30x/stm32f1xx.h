// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// What TinyUSB's stm32_fsdev driver reads from an STM32F1 device header, for the GD32F30x USBD core (platform usb.c):
// the core's register and packet buffer addresses and its two interrupts. Found on the include path in place of ST's
// header, which the tree doesn't carry
#ifndef RFOUNDRY_GD32_STM32F1XX_SHIM_H
#define RFOUNDRY_GD32_STM32F1XX_SHIM_H

#include "gd32f30x.h"

#define USB_BASE			USBD_BASE
#define USB_PMAADDR			USBD_PMA_BASE
#define USB_HP_CAN1_TX_IRQn		USBD_HP_CAN0_TX_IRQn
#define USB_LP_CAN1_RX0_IRQn		USBD_LP_CAN0_RX0_IRQn

#endif
