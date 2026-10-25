// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The flash stubs' USB device: a polled CDC-ACM serial port with RadioFoundry's identity and endpoints (bulk OUT 0x01 / IN 0x81,
// 64 bytes; interrupt IN 0x82, 8 bytes). The chapter 9 requests, the descriptors and the receive ring are shared
// (stubcdc.c); each target's controller driver implements the rest (stub/stm32f4/f4stub_usb.c on the OTG_FS core,
// stub/k22f/k22stub_usb.c on the Kinetis full-speed controller). No interrupts, no DMA.
#ifndef STUBUSB_H
#define STUBUSB_H

#include <stdbool.h>
#include <stdint.h>

// Start the device. adopt: RadioFoundry left the controller configured as its CDC-ACM serial port, so keep the address and the
// endpoints and carry on without the host noticing. state: what the target's hand-over says about the device besides
// (the K22F's data toggles; 0 elsewhere). Returns true if it adopted; false means it disconnected and is enumerating
// again (the host sees an unplug and a new port with the same serial number)
bool usb_start(bool adopt, uint32_t state, const char *serial);
// Service the controller: bus reset, enumeration, control requests, received data. Call often
void usb_poll(void);
// Received bytes, up to max; 0 when none (stubcdc.c)
uint32_t usb_read(uint8_t *buf, uint32_t max);
// Send bytes (blocks while polling); false if the host is gone or stopped reading (1 s)
bool usb_write(const uint8_t *buf, uint32_t n);
// Wait until the last IN transfer has left (before a reset), at most ms
void usb_flush(uint32_t ms);
bool usb_configured(void);

#endif
