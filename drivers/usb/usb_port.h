// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// What the USB device driver (drivers/usb, on TinyUSB) needs from the platform and the board.
#ifndef ODR_USB_PORT_H
#define ODR_USB_PORT_H

#include <stdint.h>
#include <stdbool.h>

// Platform (platform/<soc>/usb.c): clocks, pins, the controller's interrupt priority and its handler calling
// tud_int_handler(0). Called once, before the device stack starts
void usb_port_init(void);
// Platform: drop (true) or restore (false) the D+ pull-up, so the host sees an unplug and enumerates again
void usb_port_soft_disconnect(bool disconnect);

// Board: the identity the radio enumerates with (boards/<board>/usb_<board>.c defines board_usb_ids)
typedef struct {
	uint16_t vid;
	uint16_t pid_serial;		// CDC-ACM (serial port mode)
	uint16_t pid_network;		// CDC-NCM (network adapter mode), a separate PID so Windows binds another driver
	uint16_t bcd_device;
	const char *manufacturer;
	const char *product;
	const char *serial_interface;	// name of the CDC interface (shown by some hosts)
	// The serial number string: the board fills buf (NUL terminated) from a unique chip ID; NULL = no serial number
	void (*serial_number)(char *buf, uint8_t len);
} usb_ids_t;

extern const usb_ids_t board_usb_ids;

#endif
