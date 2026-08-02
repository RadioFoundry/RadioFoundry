// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// DM-1701 USB identity (docs/spec/hardware-dm1701.md 16)
#include "config.h"
#if CONFIG_USB	// the whole file
#include "config.h"
#if CONFIG_USB
#include "platform.h"
#include "usb/usb_port.h"

// The IDs the old firmware enumerated with, so existing hosts find the radio: qdmr and the legacy-protocol CPS look
// for this pair, MMDVMHost and RadioDesk rfgw only need the serial port. 0x1FC9 is NXP's vendor ID, not ours: a
// release should move to an ID of its own (for example a pid.codes PID under 0x1209) once the host tools know it.
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
	.product = "DM-1701",
	.serial_interface = "DM-1701 serial",
	.serial_number = serial_number,
};

#endif

#endif	// CONFIG_USB
