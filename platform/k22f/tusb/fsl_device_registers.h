// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// What TinyUSB's Kinetis glue for the full-speed USB controller (portable/chipidea/ci_fs) takes from the device
// header it includes by this name: the controller's base and interrupt, the CMSIS core functions, the core clock, and
// the register bit masks its driver uses (k22f_usb_regs.h). This directory is on the include path of the USB build
// only (platform.mk).
#ifndef K22F_TUSB_DEVICE_REGISTERS_H
#define K22F_TUSB_DEVICE_REGISTERS_H

#include "k22f.h"	// USB0_BASE, USB0_IRQn, the CMSIS core

extern uint32_t SystemCoreClock;

// The IRC48M clock recovery settings survive the controller's reset in dcd_init (usb.c sets them first)
#define FSL_FEATURE_USB_KHCI_IRC48M_MODULE_CLOCK_ENABLED	1

#include "k22f_usb_regs.h"

#endif
