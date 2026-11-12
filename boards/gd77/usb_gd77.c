// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// GD-77 USB identity (docs/spec/hardware-gd77.md 13)
#include "config.h"
#if CONFIG_USB	// the whole file
#include "platform.h"
#include "usb/usb_port.h"
#include "k22f_usb_regs.h"

// The same IDs as every other RadioFoundry radio with USB, so the host tools (rfoundry_debug.py, the HIL tests, qdmr, MMDVMHost)
// find it the same way. 0x1FC9 is NXP's vendor ID, not ours: a release should move to an ID of its own (for example a
// pid.codes PID under 0x1209) once the host tools know it
#define USB_VID			0x1FC9
#define USB_PID_SERIAL		0x0094
#define USB_PID_NETWORK		0x0096

// The 128-bit unique ID (SIM_UIDH..UIDL), the low 96 bits as on the STM32 radios
static void serial_number(char *buf, uint8_t len)
{
	const uint32_t uid[3] = { SIM_UIDMH, SIM_UIDML, SIM_UIDL };
	k22f_uid_hex(uid, buf, len);
}

const usb_ids_t board_usb_ids = {
	.vid = USB_VID,
	.pid_serial = USB_PID_SERIAL,
	.pid_network = USB_PID_NETWORK,
	.bcd_device = 0x0100,
	.manufacturer = "RadioFoundry",
	.product = "GD-77",
	.serial_interface = "GD-77 serial",
	.serial_number = serial_number,
};

#endif	// CONFIG_USB
