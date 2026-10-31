// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// AT-D578UV USB identity: the IDs every RadioFoundry radio enumerates with and the chip's unique ID (platform.h)
#include "config.h"
#if CONFIG_USB	// the whole file
#include "mcu_io.h"
#include "usb/usb_port.h"

const usb_ids_t board_usb_ids = {
	.vid = MCU_USB_VID,
	.pid_serial = MCU_USB_PID_SERIAL,
	.pid_network = MCU_USB_PID_NETWORK,
	.bcd_device = 0x0100,
	.manufacturer = "RadioFoundry",
	.product = "AT-D578UV",
	.serial_interface = "AT-D578UV serial",
	.serial_number = mcu_usb_serial_number,
};

#endif	// CONFIG_USB
