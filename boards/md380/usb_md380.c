// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// MD-380 USB identity (docs/spec/hardware-md380.md 15): the STM32F4 radios' IDs and serial number (platform.h)
#include "config.h"
#if CONFIG_USB	// the whole file
#include "platform.h"
#include "usb/usb_port.h"

const usb_ids_t board_usb_ids = {
	.vid = STM32_USB_VID,
	.pid_serial = STM32_USB_PID_SERIAL,
	.pid_network = STM32_USB_PID_NETWORK,
	.bcd_device = 0x0100,
	.manufacturer = "RadioFoundry",
	.product = "MD-380",
	.serial_interface = "MD-380 serial",
	.serial_number = stm32_usb_serial_number,
};

#endif	// CONFIG_USB
