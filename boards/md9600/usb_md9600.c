// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// MD-9600 USB identity (docs/spec/hardware-md9600.md 15): the STM32F4 radios' IDs and serial number (platform.h), so
// the host tools (qdmr, the open-firmware CPS, tools/odr_debug.py) find it the same way
#include "config.h"
#if CONFIG_USB	// the whole file
#include "platform.h"
#include "usb/usb_port.h"

const usb_ids_t board_usb_ids = {
	.vid = STM32_USB_VID,
	.pid_serial = STM32_USB_PID_SERIAL,
	.pid_network = STM32_USB_PID_NETWORK,
	.bcd_device = 0x0100,
	.manufacturer = "OpenDigitalRadio",
	.product = "MD-9600",
	.serial_interface = "MD-9600 serial",
	.serial_number = stm32_usb_serial_number,
};

#endif	// CONFIG_USB
