// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The flash stubs' CDC-ACM function, controller-independent (stubusb.h): the descriptors, the standard and class
// control requests (USB 2.0 chapter 9, CDC 1.2, PSTN), the configured state and the receive ring the engine reads
// through usb_read. The target's controller driver hands it every SETUP packet and control OUT data stage, and
// implements the cdc_hw_* calls below.
#ifndef STUBCDC_H
#define STUBCDC_H

#include <stdbool.h>
#include <stdint.h>

#define STUBCDC_EP0_SIZE	64u
#define STUBCDC_DATA_SIZE	64u
#define STUBCDC_NOTIFY_SIZE	8u
#define STUBCDC_EP_DATA		1u	// bulk OUT 0x01, bulk IN 0x81
#define STUBCDC_EP_NOTIFY	2u	// interrupt IN 0x82
#define STUBCDC_RING_SIZE	8192u	// received bytes waiting for the protocol engine (a power of two)

extern bool stubcdc_configured;		// SET_CONFIGURATION 1 seen, or the device adopted from RadioFoundry

// The USB serial number string (RadioFoundry's, so the host's port keeps its name across a re-enumeration)
void stubcdc_init(const char *serial);
// One SETUP packet: answered through the cdc_hw_* calls
void stubcdc_setup(const uint8_t setup[8]);
// Control write data (SET_LINE_CODING), then its end: the status stage is sent
void stubcdc_ep0_data(const uint8_t *p, uint32_t n);
void stubcdc_ep0_out_done(void);
// Received bulk OUT bytes into the ring (what doesn't fit is dropped; the drivers only arm OUT with room left)
void stubcdc_rx(const uint8_t *p, uint32_t n);
uint32_t stubcdc_rx_room(void);

// ---- the target's controller driver
// A control IN data stage (at most 127 bytes), or the status stage of a control write when len is 0
void cdc_hw_ep0_send(const uint8_t *data, uint32_t len);
void cdc_hw_ep0_stall(void);
// SET_ADDRESS: a controller that needs the status stage first applies it then
void cdc_hw_set_address(uint8_t addr);
// SET_CONFIGURATION: the data and notification endpoints on (DATA0) or off; stubcdc_configured follows
void cdc_hw_endpoints(bool on);
// SET / CLEAR_FEATURE(ENDPOINT_HALT) on 0x01, 0x81 or 0x82 (a clear restarts the endpoint's toggle at DATA0)
void cdc_hw_halt(uint8_t ep_addr, bool on);

#endif
