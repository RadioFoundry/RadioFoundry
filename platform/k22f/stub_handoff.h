// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The hand-over from a running RadioFoundry to the K22F flash stub (CONFIG_STUB_ENTRY, docs/spec/stub-k22f.md 3): where the
// stub runs, what RadioFoundry passes in r0..r2 at the jump, and how RadioFoundry reads the USB data toggles the stub continues with.
// Shared by platform/k22f/stub_entry.c and stub/k22f. Pure: tests/host/k22fUsbTest.c checks the toggle reading.
#ifndef K22F_STUB_HANDOFF_H
#define K22F_STUB_HANDOFF_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define K22F_STUB_RUN_ADDR	0x1FFF0000u	// SRAM_L's base: the stub's link address (stub/k22f/k22stub.ld)
#define K22F_STUB_SPAN		0x00010000u	// the stub keeps to SRAM_L; SRAM_U is RadioFoundry's until the jump
#define K22F_STUB_STAGE_SIZE	(8u * 1024u)	// the biggest stub image RadioFoundry accepts

// r0: flags
#define K22F_STUB_USB_ADOPT	0x1u		// the controller is RadioFoundry's configured CDC-ACM device
// r1: the data toggles the endpoints carry on with (DATA1 next when set)
#define K22F_STUB_TOGGLE_IN1	(1u << 0)	// bulk IN 0x81
#define K22F_STUB_TOGGLE_IN2	(1u << 1)	// interrupt IN 0x82
#define K22F_STUB_TOGGLE_OUT1	(1u << 2)	// bulk OUT 0x01
#define K22F_STUB_TOGGLE_VALID	(1u << 31)	// read from the driver; without it the stub starts the IN endpoints at
						// DATA0 and doesn't check the OUT toggle
// r2: the core clock in Hz

// The USB driver RadioFoundry runs (TinyUSB's full-speed controller driver, portable/chipidea/ci_fs) keeps its endpoint state
// right after the 512-byte buffer descriptor table: per endpoint and direction 8 bytes, the first word with the max
// packet size in bits 10:0 and the half (even 0, odd 1) its next transfer goes to in bit 16. Each descriptor's DATA1
// bit is fixed when the endpoint opens and the two halves alternate, so the next toggle is the DATA1 bit of the half
// the driver uses next. The endpoints' sizes must read as configured (control 64, bulk 64, interrupt 8): otherwise
// the layout isn't the one described and the result says so (no K22F_STUB_TOGGLE_VALID).
#define K22F_STUB_EP_STATE	512u	// offset of the endpoint state from the table's start
#define K22F_STUB_MEM_LEN	(512u + 16u * 2u * 8u)

static inline uint32_t k22f_stub_ep_word(const uint8_t *mem, unsigned ep, unsigned dir)
{
	uint32_t w;
	memcpy(&w, mem + K22F_STUB_EP_STATE + (ep * 2u + dir) * 8u, 4);
	return w;
}

// The next toggle of one endpoint direction: true for DATA1
static inline bool k22f_stub_next_data1(const uint8_t *mem, unsigned ep, unsigned dir)
{
	unsigned odd = (k22f_stub_ep_word(mem, ep, dir) >> 16) & 1u;
	return (mem[(ep * 4u + dir * 2u + odd) * 8u] & 0x40u) != 0;	// buffer descriptor bit 6, DATA0/1
}

// mem: K22F_STUB_MEM_LEN bytes from the buffer descriptor table's address (USB0_BDTPAGE1..3)
static inline uint32_t k22f_stub_toggles(const uint8_t *mem)
{
	if ((k22f_stub_ep_word(mem, 0, 0) & 0x7FFu) != 64u || (k22f_stub_ep_word(mem, 1, 0) & 0x7FFu) != 64u ||
	    (k22f_stub_ep_word(mem, 1, 1) & 0x7FFu) != 64u || (k22f_stub_ep_word(mem, 2, 1) & 0x7FFu) != 8u) {
		return 0;
	}
	uint32_t t = K22F_STUB_TOGGLE_VALID;
	if (k22f_stub_next_data1(mem, 1, 1)) {
		t |= K22F_STUB_TOGGLE_IN1;
	}
	if (k22f_stub_next_data1(mem, 2, 1)) {
		t |= K22F_STUB_TOGGLE_IN2;
	}
	if (k22f_stub_next_data1(mem, 1, 0)) {
		t |= K22F_STUB_TOGGLE_OUT1;
	}
	return t;
}

#endif
