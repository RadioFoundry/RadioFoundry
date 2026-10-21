// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// MD-9600 USB identity (docs/spec/hardware-md9600.md 15): the IDs the other ODR STM32 radios use, so the host tools
// (qdmr, the open-firmware CPS, tools/odr_debug.py) find it the same way
#include "config.h"
#if CONFIG_USB	// the whole file
#include "platform.h"
#include "usb/usb_port.h"

#define USB_VID			0x1FC9
#define USB_PID_SERIAL		0x0094
#define USB_PID_NETWORK		0x0096

static void serial_number(char *buf, uint8_t len)
{
	static const char hex[] = "0123456789ABCDEF";
	const uint8_t *uid = (const uint8_t *)UID_BASE;	// 96-bit unique device ID
	uint8_t n = 0;

	for (uint8_t i = 0; (i < 12) && (n + 2 < len); i++) {
		buf[n++] = hex[uid[i] >> 4];
		buf[n++] = hex[uid[i] & 0x0F];
	}
	buf[n] = '\0';
}

const usb_ids_t board_usb_ids = {
	.vid = USB_VID,
	.pid_serial = USB_PID_SERIAL,
	.pid_network = USB_PID_NETWORK,
	.bcd_device = 0x0100,
	.manufacturer = "OpenDigitalRadio",
	.product = "MD-9600",
	.serial_interface = "MD-9600 serial",
	.serial_number = serial_number,
};

#endif	// CONFIG_USB
