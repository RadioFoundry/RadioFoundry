// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The radio as a USB device, on TinyUSB: descriptors from the board's IDs (usb_port.h), the device stack task, and
// the serial mode's CDC-ACM port as a hal_stream_t for the host link.
//
// One personality at a time (docs/spec/hardware-dm1701.md 16): serial (CDC-ACM) now; network (CDC-NCM) to come, as
// a second set of descriptors with its own PID, chosen from the settings before usb_device_start().
#ifndef ODR_USB_DEVICE_H
#define ODR_USB_DEVICE_H

#include <stdint.h>
#include <stdbool.h>
#include "hal_stream.h"

typedef enum {
	USB_MODE_SERIAL,	// CDC-ACM: MMDVM hotspot and CPS over a serial port
	USB_MODE_NETWORK,	// CDC-NCM: not implemented yet (falls back to serial)
} usb_mode_t;

// Choose the personality; only before the stream's start()
void usb_device_set_mode(usb_mode_t mode);
usb_mode_t usb_device_mode(void);

// Configured by the host (the reduced-power clock must not be used meanwhile, docs/spec/hardware-dm1701.md 1.3)
bool usb_device_configured(void);

// The CDC-ACM port. start() brings the stack up (its task included)
extern const hal_stream_t usb_cdc_stream;

#endif
