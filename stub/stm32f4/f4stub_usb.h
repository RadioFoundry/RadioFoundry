// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The flash stub's USB device: a polled CDC-ACM serial port on the STM32F4 OTG_FS core (no interrupts, no DMA), with
// the endpoints ODR uses (docs/spec/hardware-dm1701.md 16: bulk OUT 0x01 / IN 0x81, 64 bytes; interrupt IN 0x82).
#ifndef F4STUB_USB_H
#define F4STUB_USB_H

#include <stdbool.h>
#include <stdint.h>

// Start the device. adopt: ODR left the core configured as its CDC-ACM serial port, so keep the address and the
// endpoints and carry on without the host noticing. Returns true if it adopted; false means it disconnected, reset
// the core and is enumerating again (the host sees an unplug and a new port with the same serial number)
bool usb_start(bool adopt, const char *serial);
// Service the core: bus reset, enumeration, control requests, received data. Call often
void usb_poll(void);
// Received bytes, up to max; 0 when none
uint32_t usb_read(uint8_t *buf, uint32_t max);
// Send bytes (blocks while polling); false if the host is gone or stopped reading (1 s)
bool usb_write(const uint8_t *buf, uint32_t n);
// Wait until the last IN transfer has left the FIFO (before a reset), at most ms
void usb_flush(uint32_t ms);
bool usb_configured(void);

// Time (f4stub_main.c): milliseconds since start, from the cycle counter
uint32_t stub_ms(void);
void stub_delay_ms(uint32_t ms);

#endif
