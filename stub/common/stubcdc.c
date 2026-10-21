// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The flash stubs' CDC-ACM function (stubcdc.h), written from USB 2.0 chapter 9 and the CDC 1.2 / PSTN class
// documents. The descriptors give ODR's serial port identity, so the host binds the same driver as for ODR.
#include <stddef.h>
#include <string.h>
#include "stubcdc.h"
#include "stubusb.h"

// ---- descriptors (USB 2.0 9.6, CDC 1.2 5, PSTN 5.3)

static const uint8_t dev_desc[18] = {
	18, 1, 0x00, 0x02,	// bLength, DEVICE, bcdUSB 2.00
	0xEF, 0x02, 0x01,	// miscellaneous class, common subclass, interface association
	STUBCDC_EP0_SIZE,
	0xC9, 0x1F, 0x94, 0x00,	// VID 0x1FC9, PID 0x0094: ODR's serial-port identity (boards/*/usb_*.c)
	0x00, 0x7F,		// bcdDevice 0x7F00: not an ODR firmware version
	1, 2, 3, 1,		// iManufacturer, iProduct, iSerialNumber, one configuration
};

#define CFG_LEN		75u
static const uint8_t cfg_desc[CFG_LEN] = {
	9, 2, CFG_LEN, 0, 2, 1, 0, 0xC0, 50,		// configuration: 2 interfaces, self powered, 100 mA
	8, 11, 0, 2, 0x02, 0x02, 0x00, 0,		// interface association: interfaces 0-1, CDC ACM
	9, 4, 0, 0, 1, 0x02, 0x02, 0x00, 4,		// interface 0: communications, ACM, one endpoint, string 4
	5, 0x24, 0x00, 0x20, 0x01,			// header functional descriptor, CDC 1.20
	5, 0x24, 0x01, 0x00, 1,				// call management: none, data interface 1
	4, 0x24, 0x02, 0x02,				// ACM: line coding and serial state
	5, 0x24, 0x06, 0, 1,				// union: master 0, slave 1
	7, 5, 0x80 | STUBCDC_EP_NOTIFY, 0x03, STUBCDC_NOTIFY_SIZE, 0, 16,	// interrupt IN 0x82, 8 bytes, 16 ms
	9, 4, 1, 0, 2, 0x0A, 0x00, 0x00, 0,		// interface 1: data, two endpoints
	7, 5, STUBCDC_EP_DATA, 0x02, STUBCDC_DATA_SIZE, 0, 0,		// bulk OUT 0x01
	7, 5, 0x80 | STUBCDC_EP_DATA, 0x02, STUBCDC_DATA_SIZE, 0, 0,	// bulk IN 0x81
};

static const char *const strings[] = { NULL, "OpenDigitalRadio", "ODR flash stub", NULL, "ODR flash stub serial" };

// ---- state

bool stubcdc_configured;
static const char *serial_str;
static uint8_t ep0_buf[STUBCDC_EP0_SIZE];
static uint32_t ep0_rx_len;
static bool ep0_expect_out;	// a control write's data stage is due (SET_LINE_CODING)
static uint8_t line_coding[7] = { 0x00, 0xC2, 0x01, 0x00, 0, 0, 8 };	// 115200 8N1: only remembered
static uint8_t desc_buf[2 + 2 * 32];

static uint8_t ring[STUBCDC_RING_SIZE];
static uint32_t ring_head, ring_tail;	// head: written by the receive path; tail: read by usb_read

static uint32_t ring_used(void)
{
	return ring_head - ring_tail;
}

void stubcdc_init(const char *serial)
{
	serial_str = serial;
	ring_head = ring_tail = 0;
	ep0_expect_out = false;
}

uint32_t stubcdc_rx_room(void)
{
	return STUBCDC_RING_SIZE - ring_used();
}

void stubcdc_rx(const uint8_t *p, uint32_t n)
{
	for (uint32_t i = 0; i < n && ring_used() < STUBCDC_RING_SIZE; i++) {
		ring[ring_head % STUBCDC_RING_SIZE] = p[i];
		ring_head++;
	}
}

uint32_t usb_read(uint8_t *buf, uint32_t max)
{
	uint32_t n = 0;
	while (n < max && ring_used() > 0) {
		buf[n++] = ring[ring_tail % STUBCDC_RING_SIZE];
		ring_tail++;
	}
	return n;
}

bool usb_configured(void)
{
	return stubcdc_configured;
}

static void ep0_reply(const uint8_t *data, uint32_t len, uint16_t wlength)
{
	cdc_hw_ep0_send(data, len < wlength ? len : wlength);
}

static uint32_t string_desc(uint8_t index)
{
	if (index == 0) {
		desc_buf[0] = 4;
		desc_buf[1] = 3;
		desc_buf[2] = 0x09;	// English (US)
		desc_buf[3] = 0x04;
		return 4;
	}
	const char *s = index == 3 ? serial_str : (index < sizeof(strings) / sizeof(strings[0]) ? strings[index] : NULL);
	if (s == NULL) {
		return 0;
	}
	uint32_t n = 0;
	while (s[n] != '\0' && n < 32u) {
		desc_buf[2u + 2u * n] = (uint8_t)s[n];
		desc_buf[3u + 2u * n] = 0;
		n++;
	}
	desc_buf[0] = (uint8_t)(2u + 2u * n);
	desc_buf[1] = 3;
	return 2u + 2u * n;
}

void stubcdc_ep0_data(const uint8_t *p, uint32_t n)
{
	uint32_t room = sizeof(ep0_buf) - ep0_rx_len;
	uint32_t take = n < room ? n : room;
	memcpy(ep0_buf + ep0_rx_len, p, take);
	ep0_rx_len += take;
}

void stubcdc_ep0_out_done(void)
{
	if (!ep0_expect_out) {
		return;		// the status stage of a control read: nothing to answer
	}
	if (ep0_rx_len >= sizeof(line_coding)) {
		memcpy(line_coding, ep0_buf, sizeof(line_coding));
	}
	ep0_expect_out = false;
	cdc_hw_ep0_send(NULL, 0);
}

void stubcdc_setup(const uint8_t setup[8])
{
	uint8_t type = setup[0], req = setup[1];
	uint16_t value = (uint16_t)(setup[2] | (setup[3] << 8));
	uint16_t index = (uint16_t)(setup[4] | (setup[5] << 8));
	uint16_t length = (uint16_t)(setup[6] | (setup[7] << 8));
	static uint8_t small[2];

	ep0_expect_out = false;
	if ((type & 0x60u) == 0x20u) {		// class requests to the communications interface
		switch (req) {
		case 0x20:			// SET_LINE_CODING: 7 bytes follow
			ep0_expect_out = true;
			ep0_rx_len = 0;
			return;
		case 0x21:			// GET_LINE_CODING
			ep0_reply(line_coding, sizeof(line_coding), length);
			return;
		case 0x22:			// SET_CONTROL_LINE_STATE
		case 0x23:			// SEND_BREAK
			cdc_hw_ep0_send(NULL, 0);
			return;
		default:
			cdc_hw_ep0_stall();
			return;
		}
	}
	if ((type & 0x60u) != 0) {
		cdc_hw_ep0_stall();
		return;
	}
	switch (req) {
	case 0x00:				// GET_STATUS
		small[0] = (type & 0x1Fu) == 0 ? 1u : 0u;	// device: self powered
		small[1] = 0;
		ep0_reply(small, 2, length);
		break;
	case 0x01:				// CLEAR_FEATURE
	case 0x03:				// SET_FEATURE
		if ((type & 0x1Fu) == 2u && value == 0) {
			cdc_hw_halt((uint8_t)index, req == 0x03);	// ENDPOINT_HALT
		}
		cdc_hw_ep0_send(NULL, 0);
		break;
	case 0x05:				// SET_ADDRESS
		cdc_hw_set_address((uint8_t)(value & 0x7Fu));
		cdc_hw_ep0_send(NULL, 0);
		break;
	case 0x06: {				// GET_DESCRIPTOR
		uint8_t dtype = (uint8_t)(value >> 8);
		if (dtype == 1u) {
			ep0_reply(dev_desc, sizeof(dev_desc), length);
		} else if (dtype == 2u) {
			ep0_reply(cfg_desc, sizeof(cfg_desc), length);
		} else if (dtype == 3u) {
			uint32_t n = string_desc((uint8_t)value);
			if (n == 0) {
				cdc_hw_ep0_stall();
			} else {
				ep0_reply(desc_buf, n, length);
			}
		} else {
			cdc_hw_ep0_stall();	// device qualifier and the rest: full speed only
		}
		break;
	}
	case 0x08:				// GET_CONFIGURATION
		small[0] = stubcdc_configured ? 1u : 0u;
		ep0_reply(small, 1, length);
		break;
	case 0x09:				// SET_CONFIGURATION
		cdc_hw_endpoints(value == 1u);
		stubcdc_configured = value == 1u;
		cdc_hw_ep0_send(NULL, 0);
		break;
	case 0x0A:				// GET_INTERFACE
		small[0] = 0;
		ep0_reply(small, 1, length);
		break;
	case 0x0B:				// SET_INTERFACE: only alternate setting 0 exists
		cdc_hw_ep0_send(NULL, 0);
		break;
	default:
		cdc_hw_ep0_stall();
		break;
	}
}
