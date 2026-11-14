// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Polled CDC-ACM device on the GD32F30x USBD core (gd32stub_usb.h). Written from the GD32F30x user manual's USBD
// chapter (the STM32F103's full-speed device core, register for register: one control and status register per
// endpoint with toggle-on-write state bits, a buffer table in the 512-byte packet memory, which the CPU sees as
// 16-bit words on a 32-bit stride), USB 2.0 chapter 9 and the CDC 1.2 / PSTN class documents. The status bits are
// polled; the interrupt lines stay off.
#include <stddef.h>
#include <string.h>
#include "gd32f30x.h"
#include "gd32stub_usb.h"

#define EPCS(i)		GD32_REG(USBD_BASE + 4u * (i))
#define USBD_CTL	GD32_REG(USBD_BASE + 0x40u)
#define USBD_INTF	GD32_REG(USBD_BASE + 0x44u)
#define USBD_DADDR	GD32_REG(USBD_BASE + 0x4Cu)
#define USBD_BADDR	GD32_REG(USBD_BASE + 0x50u)

#define CTL_CLOSE	(1u << 1)	// the transceiver off (power down)
#define CTL_SETRST	(1u << 0)	// the core held in reset
#define INTF_CTR	(1u << 15)	// some endpoint finished a transaction (read only)
#define INTF_RST	(1u << 10)
#define INTF_EPNUM	0x0Fu
#define INTF_OTHERS	0x7B00u		// overrun, error, wake-up, suspend, start-of-frame flags: cleared, not used
#define DADDR_USBEN	0x80u

// Endpoint control and status: two rc_w0 flags, toggle-on-write state, read-write type, kind and address
#define EP_CTR_RX	0x8000u
#define EP_DTOG_RX	0x4000u
#define EP_STAT_RX	0x3000u
#define EP_SETUP	0x0800u
#define EP_TYPE		0x0600u
#define EP_KIND		0x0100u		// double buffer on a bulk endpoint, status-out on a control one
#define EP_CTR_TX	0x0080u
#define EP_DTOG_TX	0x0040u
#define EP_STAT_TX	0x0030u
#define EP_ADDR		0x000Fu
#define EP_RW		(EP_TYPE | EP_KIND | EP_ADDR)
#define TYPE_BULK	0x0000u
#define TYPE_CONTROL	0x0200u
#define TYPE_INTR	0x0600u
enum { ST_OFF = 0, ST_STALL = 1, ST_NAK = 2, ST_VALID = 3 };

#define EP0_SIZE	64u
#define DATA_SIZE	64u
#define NOTIFY_SIZE	8u
#define EP_DATA		1u	// bulk OUT 0x01, bulk IN 0x81
#define EP_NOTIFY	2u	// interrupt IN 0x82
#define PMA_SIZE	512u
#define RX_64		0x8400u	// COUNT_RX's block size field: two 32-byte blocks

// The packet memory after a cold start: the buffer table, then the buffers
#define BTABLE		0x000u
#define BUF_EP0_RX	0x040u
#define BUF_EP0_TX	0x080u
#define BUF_DATA_RX	0x0C0u
#define BUF_DATA_TX	0x100u
#define BUF_NOTIFY_TX	0x140u

#define RING_SIZE	2048u	// received bytes waiting for the protocol engine (a power of two)
#define WAIT_MS		1000u	// a host that stops reading for this long is gone

// ---- descriptors (USB 2.0 9.6, CDC 1.2 5, PSTN 5.3): the STM32F4 stub's (stub/stm32f4/f4stub_usb.c)

static const uint8_t dev_desc[18] = {
	18, 1, 0x00, 0x02,	// bLength, DEVICE, bcdUSB 2.00
	0xEF, 0x02, 0x01,	// miscellaneous class, common subclass, interface association
	EP0_SIZE,
	0xC9, 0x1F, 0x94, 0x00,	// VID 0x1FC9, PID 0x0094: RadioFoundry's serial-port identity (platform.h GD32_USB_*)
	0x00, 0x7F,		// bcdDevice 0x7F00: not a RadioFoundry firmware version
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
	7, 5, 0x80 | EP_NOTIFY, 0x03, NOTIFY_SIZE, 0, 16,	// interrupt IN 0x82, 8 bytes, 16 ms
	9, 4, 1, 0, 2, 0x0A, 0x00, 0x00, 0,		// interface 1: data, two endpoints
	7, 5, EP_DATA, 0x02, DATA_SIZE, 0, 0,		// bulk OUT 0x01
	7, 5, 0x80 | EP_DATA, 0x02, DATA_SIZE, 0, 0,	// bulk IN 0x81
};

static const char *const strings[] = { NULL, "RadioFoundry", "RadioFoundry flash stub", NULL, "RadioFoundry flash stub serial" };

// ---- state

static const char *serial_str;
static bool configured;
// Hardware endpoint registers and packet buffers in use: the cold start's, or what RadioFoundry's stack set up
static unsigned out_ep, in_ep, notify_ep;
static uint16_t btable, buf_ep0_rx, buf_ep0_tx, buf_data_rx, buf_data_tx;
static bool out_paused;		// the OUT endpoint is left NAKing until the ring has room for a packet
static bool in_busy;		// an IN packet waits for the host
static uint8_t setup[8];
static const uint8_t *ep0_ptr;	// the control IN data stage still to send
static uint32_t ep0_left;
static bool ep0_zlp_due;
static bool ep0_expect_out;	// a control write's data stage is due (SET_LINE_CODING)
static uint8_t pending_addr;	// SET_ADDRESS takes effect after its status stage
static uint8_t line_coding[7] = { 0x00, 0xC2, 0x01, 0x00, 0, 0, 8 };	// 115200 8N1: only remembered
static uint8_t desc_buf[2 + 2 * 32];

static uint8_t ring[RING_SIZE];
static uint32_t ring_head, ring_tail;

static uint32_t ring_used(void)
{
	return ring_head - ring_tail;
}

// ---- packet memory: byte offsets in the core's view, 16 bits per 32-bit CPU word

static volatile uint16_t *pma(uint32_t off)
{
	return (volatile uint16_t *)(uintptr_t)(USBD_PMA_BASE + 2u * off);
}

static void pma_write(uint32_t off, const uint8_t *src, uint32_t n)
{
	for (uint32_t i = 0; i < n; i += 2u) {
		*pma(off + i) = (uint16_t)(src[i] | (i + 1u < n ? src[i + 1u] << 8 : 0));
	}
}

static void pma_read(uint32_t off, uint8_t *dst, uint32_t n)
{
	for (uint32_t i = 0; i < n; i += 2u) {
		uint16_t v = *pma(off + i);
		dst[i] = (uint8_t)v;
		if (i + 1u < n) {
			dst[i + 1u] = (uint8_t)(v >> 8);
		}
	}
}

// The buffer table: per endpoint register the transmit address and count, the receive address and count
static volatile uint16_t *bt(unsigned ep, unsigned field)
{
	return pma(btable + 8u * ep + 2u * field);
}

enum { BT_TX_ADDR = 0, BT_TX_COUNT = 1, BT_RX_ADDR = 2, BT_RX_COUNT = 3 };

// ---- endpoint registers: write 1 to the CTR flags to keep them, 0 to the toggle bits to keep them

static void set_tx(unsigned ep, unsigned st)
{
	uint32_t r = EPCS(ep);
	EPCS(ep) = ((r & (EP_RW | EP_STAT_TX)) ^ (st << 4)) | EP_CTR_RX | EP_CTR_TX;
}

static void set_rx(unsigned ep, unsigned st)
{
	uint32_t r = EPCS(ep);
	EPCS(ep) = ((r & (EP_RW | EP_STAT_RX)) ^ (st << 12)) | EP_CTR_RX | EP_CTR_TX;
}

static void clear_ctr_rx(unsigned ep)
{
	EPCS(ep) = (EPCS(ep) & EP_RW) | EP_CTR_TX;
}

static void clear_ctr_tx(unsigned ep)
{
	EPCS(ep) = (EPCS(ep) & EP_RW) | EP_CTR_RX;
}

// Type and address, both data toggles 0, the two states as given; both CTR flags cleared
static void ep_init(unsigned ep, uint32_t type_addr, unsigned rx, unsigned tx)
{
	uint32_t r = EPCS(ep);
	uint32_t want = ((uint32_t)rx << 12) | ((uint32_t)tx << 4);
	EPCS(ep) = type_addr | ((r & (EP_DTOG_RX | EP_STAT_RX | EP_DTOG_TX | EP_STAT_TX)) ^ want);
}

// ---- endpoint 0

static void ep0_tx_next(void)
{
	uint32_t n = ep0_left < EP0_SIZE ? ep0_left : EP0_SIZE;
	if (n > 0) {
		pma_write(buf_ep0_tx, ep0_ptr, n);
	}
	*bt(0, BT_TX_COUNT) = (uint16_t)n;
	ep0_ptr += n;
	ep0_left -= n;
	if (n < EP0_SIZE) {
		ep0_zlp_due = false;	// a short packet ends the transfer
	}
	set_tx(0, ST_VALID);
	set_rx(0, ST_VALID);	// the host's status packet may come early
}

static void ep0_send(const uint8_t *data, uint32_t len, uint16_t wlength)
{
	if (len > wlength) {
		len = wlength;
	}
	ep0_ptr = data;
	ep0_left = len;
	// A data stage shorter than asked for that ends on a full packet needs a zero-length one to end it
	ep0_zlp_due = len < wlength && len % EP0_SIZE == 0 && len > 0;
	ep0_tx_next();
}

static void ep0_zlp(void)
{
	ep0_ptr = NULL;
	ep0_left = 0;
	ep0_zlp_due = false;
	ep0_tx_next();
}

static void ep0_stall(void)
{
	ep0_left = 0;
	ep0_zlp_due = false;
	set_tx(0, ST_STALL);
	set_rx(0, ST_STALL);	// a SETUP is taken even so, and clears it
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

static void endpoints_off(void)
{
	ep_init(out_ep, TYPE_BULK | EP_DATA, ST_OFF, ST_OFF);
	if (in_ep != out_ep) {
		ep_init(in_ep, TYPE_BULK | EP_DATA, ST_OFF, ST_OFF);
	}
	ep_init(notify_ep, TYPE_INTR | EP_NOTIFY, ST_OFF, ST_OFF);
	configured = false;
	in_busy = false;
	out_paused = false;
}

// The buffers stay where bus_reset or the adopted stack put them. After a cold start one register serves both
// directions of 0x01 / 0x81; an adopted stack may have used two
static void endpoints_on(void)
{
	*bt(out_ep, BT_RX_COUNT) = RX_64;
	*bt(in_ep, BT_TX_COUNT) = 0;
	ep_init(out_ep, TYPE_BULK | EP_DATA, ST_VALID, in_ep == out_ep ? ST_NAK : ST_OFF);
	if (in_ep != out_ep) {
		ep_init(in_ep, TYPE_BULK | EP_DATA, ST_OFF, ST_NAK);
	}
	ep_init(notify_ep, TYPE_INTR | EP_NOTIFY, ST_OFF, ST_NAK);
	in_busy = false;
	out_paused = false;
	configured = true;
}

static void halt(uint8_t ep_addr, bool on)
{
	if ((ep_addr & 0x0Fu) != EP_DATA || !configured) {
		return;
	}
	unsigned ep = (ep_addr & 0x80u) ? in_ep : out_ep;
	uint32_t r = EPCS(ep);
	if (ep_addr & 0x80u) {
		// Clearing the halt starts the toggle at DATA0 again (USB 2.0 9.4.5)
		EPCS(ep) = ((r & (EP_RW | EP_STAT_TX | EP_DTOG_TX)) ^ ((on ? ST_STALL : ST_NAK) << 4)) | EP_CTR_RX | EP_CTR_TX;
		in_busy = false;
	} else {
		EPCS(ep) = ((r & (EP_RW | EP_STAT_RX | EP_DTOG_RX)) ^ ((uint32_t)(on ? ST_STALL : ST_NAK) << 12)) | EP_CTR_RX |
			   EP_CTR_TX;
		out_paused = !on;	// re-armed by usb_poll once the ring has room
	}
}

static void handle_setup(void)
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
			set_rx(0, ST_VALID);
			return;
		case 0x21:			// GET_LINE_CODING
			ep0_send(line_coding, sizeof(line_coding), length);
			return;
		case 0x22:			// SET_CONTROL_LINE_STATE
		case 0x23:			// SEND_BREAK
			ep0_zlp();
			return;
		default:
			ep0_stall();
			return;
		}
	}
	if ((type & 0x60u) != 0) {
		ep0_stall();
		return;
	}
	switch (req) {
	case 0x00:				// GET_STATUS
		small[0] = (type & 0x1Fu) == 0 ? 1u : 0u;	// device: self powered
		small[1] = 0;
		ep0_send(small, 2, length);
		break;
	case 0x01:				// CLEAR_FEATURE
	case 0x03:				// SET_FEATURE
		if ((type & 0x1Fu) == 2u && value == 0) {
			halt((uint8_t)index, req == 0x03);	// ENDPOINT_HALT
		}
		ep0_zlp();
		break;
	case 0x05:				// SET_ADDRESS: this core takes it after the status stage
		pending_addr = (uint8_t)(value & 0x7Fu);
		ep0_zlp();
		break;
	case 0x06: {				// GET_DESCRIPTOR
		uint8_t dtype = (uint8_t)(value >> 8);
		if (dtype == 1u) {
			ep0_send(dev_desc, sizeof(dev_desc), length);
		} else if (dtype == 2u) {
			ep0_send(cfg_desc, sizeof(cfg_desc), length);
		} else if (dtype == 3u) {
			uint32_t n = string_desc((uint8_t)value);
			if (n == 0) {
				ep0_stall();
			} else {
				ep0_send(desc_buf, n, length);
			}
		} else {
			ep0_stall();		// device qualifier and the rest: full speed only
		}
		break;
	}
	case 0x08:				// GET_CONFIGURATION
		small[0] = configured ? 1u : 0u;
		ep0_send(small, 1, length);
		break;
	case 0x09:				// SET_CONFIGURATION
		if (value == 1u) {
			endpoints_on();
		} else {
			endpoints_off();
		}
		ep0_zlp();
		break;
	case 0x0A:				// GET_INTERFACE
		small[0] = 0;
		ep0_send(small, 1, length);
		break;
	case 0x0B:				// SET_INTERFACE: only alternate setting 0 exists
		ep0_zlp();
		break;
	default:
		ep0_stall();
		break;
	}
}

static void ep0_event(void)
{
	uint32_t r = EPCS(0);
	if (r & EP_CTR_TX) {
		clear_ctr_tx(0);
		if (pending_addr != 0) {
			USBD_DADDR = DADDR_USBEN | pending_addr;
			pending_addr = 0;
		}
		if (ep0_left > 0 || ep0_zlp_due) {
			ep0_tx_next();
		}
	}
	if (r & EP_CTR_RX) {
		uint32_t n = *bt(0, BT_RX_COUNT) & 0x3FFu;
		if (r & EP_SETUP) {
			pma_read(buf_ep0_rx, setup, 8);
			clear_ctr_rx(0);
			handle_setup();
			return;
		}
		if (ep0_expect_out) {		// SET_LINE_CODING's data arrived: status stage
			if (n >= sizeof(line_coding)) {
				pma_read(buf_ep0_rx, line_coding, sizeof(line_coding));
			}
			ep0_expect_out = false;
			clear_ctr_rx(0);
			ep0_zlp();
			return;
		}
		clear_ctr_rx(0);		// the status packet of a control read
		set_rx(0, ST_VALID);
	}
}

static void out_event(void)
{
	uint32_t n = *bt(out_ep, BT_RX_COUNT) & 0x3FFu;
	uint8_t tmp[DATA_SIZE];
	if (n > sizeof(tmp)) {
		n = sizeof(tmp);
	}
	pma_read(buf_data_rx, tmp, n);
	clear_ctr_rx(out_ep);
	// The endpoint was armed only with a packet's room in the ring
	for (uint32_t i = 0; i < n && ring_used() < RING_SIZE; i++) {
		ring[ring_head % RING_SIZE] = tmp[i];
		ring_head++;
	}
	if (RING_SIZE - ring_used() >= DATA_SIZE) {
		set_rx(out_ep, ST_VALID);
	} else {
		out_paused = true;
	}
}

static void bus_reset(void)
{
	btable = BTABLE;
	USBD_BADDR = BTABLE;
	buf_ep0_rx = BUF_EP0_RX;
	buf_ep0_tx = BUF_EP0_TX;
	buf_data_rx = BUF_DATA_RX;
	buf_data_tx = BUF_DATA_TX;
	out_ep = in_ep = EP_DATA;
	notify_ep = EP_NOTIFY;
	*bt(0, BT_RX_ADDR) = buf_ep0_rx;
	*bt(0, BT_RX_COUNT) = RX_64;
	*bt(0, BT_TX_ADDR) = buf_ep0_tx;
	*bt(0, BT_TX_COUNT) = 0;
	*bt(EP_DATA, BT_RX_ADDR) = buf_data_rx;
	*bt(EP_DATA, BT_TX_ADDR) = buf_data_tx;
	*bt(EP_NOTIFY, BT_TX_ADDR) = BUF_NOTIFY_TX;
	*bt(EP_NOTIFY, BT_TX_COUNT) = 0;
	ep_init(0, TYPE_CONTROL, ST_VALID, ST_NAK);
	for (unsigned ep = 1; ep < 8u; ep++) {
		ep_init(ep, ep, ST_OFF, ST_OFF);
	}
	configured = false;
	in_busy = false;
	out_paused = false;
	ep0_left = 0;
	ep0_zlp_due = false;
	ep0_expect_out = false;
	pending_addr = 0;
	USBD_DADDR = DADDR_USBEN;	// address 0
}

void usb_poll(void)
{
	uint32_t f = USBD_INTF;

	if (f & INTF_RST) {
		USBD_INTF = (uint16_t)~INTF_RST;
		bus_reset();
	}
	if (f & INTF_OTHERS) {
		USBD_INTF = (uint16_t)~(f & INTF_OTHERS);
	}
	for (unsigned k = 0; k < 16u && (USBD_INTF & INTF_CTR); k++) {
		unsigned ep = USBD_INTF & INTF_EPNUM;
		uint32_t r = EPCS(ep);
		if (ep == 0) {
			ep0_event();
			continue;
		}
		if (ep == out_ep && (r & EP_CTR_RX)) {
			out_event();
			r = EPCS(ep);
		}
		if (ep == in_ep && (r & EP_CTR_TX)) {
			clear_ctr_tx(ep);
			in_busy = false;
			r = EPCS(ep);
		}
		// Anything else (the notification endpoint never sends): drop the flags so the loop ends
		if (r & (EP_CTR_RX | EP_CTR_TX)) {
			EPCS(ep) = r & EP_RW;
		}
	}
	if (configured && out_paused && RING_SIZE - ring_used() >= DATA_SIZE) {
		out_paused = false;
		set_rx(out_ep, ST_VALID);
	}
}

uint32_t usb_read(uint8_t *buf, uint32_t max)
{
	uint32_t n = 0;
	while (n < max && ring_used() > 0) {
		buf[n++] = ring[ring_tail % RING_SIZE];
		ring_tail++;
	}
	return n;
}

static bool in_idle(uint32_t ms)
{
	uint32_t t0 = stub_ms();
	while (in_busy) {
		usb_poll();
		if (!configured || stub_ms() - t0 > ms) {
			return false;
		}
	}
	return true;
}

static bool in_packet(const uint8_t *data, uint32_t n)
{
	if (!in_idle(WAIT_MS)) {
		return false;
	}
	if (n > 0) {
		pma_write(buf_data_tx, data, n);
	}
	*bt(in_ep, BT_TX_COUNT) = (uint16_t)n;
	in_busy = true;
	set_tx(in_ep, ST_VALID);
	return true;
}

bool usb_write(const uint8_t *buf, uint32_t n)
{
	if (!configured) {
		return false;
	}
	for (uint32_t off = 0; off < n; off += DATA_SIZE) {
		if (!in_packet(buf + off, n - off < DATA_SIZE ? n - off : DATA_SIZE)) {
			return false;
		}
	}
	// A transfer that ends on a full packet gets a zero-length one, so a host reading bigger blocks sees its end
	if (n > 0 && n % DATA_SIZE == 0) {
		return in_packet(NULL, 0);
	}
	return true;
}

void usb_flush(uint32_t ms)
{
	(void)in_idle(ms);
}

bool usb_configured(void)
{
	return configured;
}

// ---- start

// The register with this address and type whose direction is on (its state not disabled), or 0 for none
static unsigned find_ep(unsigned addr, uint32_t type, uint32_t stat_mask)
{
	for (unsigned ep = 1; ep < 8u; ep++) {
		uint32_t r = EPCS(ep);
		if ((r & EP_ADDR) == addr && (r & EP_TYPE) == type && (r & stat_mask) != 0) {
			return ep;
		}
	}
	return 0;
}

// Take over RadioFoundry's configured device: the address, the endpoint registers and their buffers stay (read back from
// the buffer table). An IN packet its stack left waiting is dropped (the host saw the ENTER reply complete before the
// jump), and so is an OUT packet it had not read. A double-buffered data endpoint, or anything that doesn't look like
// RadioFoundry's serial port, is not adopted: the stub enumerates afresh instead
static bool adopt(void)
{
	if ((USBD_DADDR & DADDR_USBEN) == 0 || (USBD_DADDR & 0x7Fu) == 0 || (EPCS(0) & EP_TYPE) != TYPE_CONTROL) {
		return false;
	}
	out_ep = find_ep(EP_DATA, TYPE_BULK, EP_STAT_RX);
	in_ep = find_ep(EP_DATA, TYPE_BULK, EP_STAT_TX);
	notify_ep = find_ep(EP_NOTIFY, TYPE_INTR, EP_STAT_TX);
	if (out_ep == 0 || in_ep == 0 || notify_ep == 0 || (EPCS(out_ep) & EP_KIND) || (EPCS(in_ep) & EP_KIND)) {
		return false;
	}
	btable = (uint16_t)(USBD_BADDR & 0xFFF8u);
	buf_ep0_rx = *bt(0, BT_RX_ADDR);
	buf_ep0_tx = *bt(0, BT_TX_ADDR);
	buf_data_rx = *bt(out_ep, BT_RX_ADDR);
	buf_data_tx = *bt(in_ep, BT_TX_ADDR);
	if (btable >= PMA_SIZE || buf_ep0_rx + EP0_SIZE > PMA_SIZE || buf_ep0_tx + EP0_SIZE > PMA_SIZE ||
	    buf_data_rx + DATA_SIZE > PMA_SIZE || buf_data_tx + DATA_SIZE > PMA_SIZE) {
		return false;
	}
	set_tx(in_ep, ST_NAK);
	clear_ctr_tx(in_ep);
	clear_ctr_rx(out_ep);
	set_rx(out_ep, ST_VALID);
	EPCS(0) = EPCS(0) & EP_RW;	// both flags of a transaction its stack had not handled
	set_rx(0, ST_VALID);
	USBD_INTF = 0;
	configured = true;
	in_busy = false;
	out_paused = false;
	return true;
}

// PA12 (D+) driven low as a plain output: the host sees an unplug whether the board's pull-up is fixed or switched,
// with no board pin to know. Back to an input after. The core lets go of PA11 / PA12 only while its clock is off
static void force_unplug(uint32_t ms)
{
	gd32_gpio_t *pa = GPIOA;
	pa->BC = 1u << 12;
	pa->CTL1 = (pa->CTL1 & ~(0xFu << 16)) | (0x3u << 16);	// push-pull output
	stub_delay_ms(ms);
	pa->CTL1 = (pa->CTL1 & ~(0xFu << 16)) | (0x4u << 16);	// floating input
}

static void cold_start(void)
{
	RCU_APB2EN |= RCU_APB2EN_PAEN;
	RCU_APB1EN &= ~RCU_APB1EN_USBDEN;
	(void)RCU_APB1EN;
	force_unplug(100);			// long enough that every host notices
	RCU_APB1EN |= RCU_APB1EN_USBDEN;
	(void)RCU_APB1EN;
	RCU_APB1RST |= RCU_APB1RST_USBDRST;	// every register and the address back to reset values
	RCU_APB1RST &= ~RCU_APB1RST_USBDRST;
	USBD_CTL = CTL_SETRST;			// transceiver on, core still in reset
	stub_delay_ms(1);			// the transceiver's start-up time
	USBD_CTL = 0;				// running, every interrupt source masked (all polled)
	USBD_INTF = 0;
	bus_reset();
	USBD_DADDR = 0;				// off until the host's bus reset
}

bool usb_start(bool want_adopt, const char *serial)
{
	serial_str = serial;
	ring_head = ring_tail = 0;
	if (want_adopt && adopt()) {
		return true;
	}
	cold_start();
	return false;
}
