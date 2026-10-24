// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Polled CDC-ACM device on the STM32F4 OTG_FS core (f4stub_usb.h). Written from the reference manual's OTG_FS
// device programming model (slave mode: the CPU moves every packet through the FIFOs), USB 2.0 chapter 9 and the
// CDC 1.2 / PSTN class documents. The core's status bits are polled; the interrupt line stays off.
#include <stddef.h>
#include <string.h>
#include "stm32f4xx.h"
#include "f4stub_usb.h"

#define BASE		USB_OTG_FS_PERIPH_BASE
#define OTG		((USB_OTG_GlobalTypeDef *)BASE)
#define DEV		((USB_OTG_DeviceTypeDef *)(BASE + USB_OTG_DEVICE_BASE))
#define INEP(n)		((USB_OTG_INEndpointTypeDef *)(BASE + USB_OTG_IN_ENDPOINT_BASE + (n) * USB_OTG_EP_REG_SIZE))
#define OUTEP(n)	((USB_OTG_OUTEndpointTypeDef *)(BASE + USB_OTG_OUT_ENDPOINT_BASE + (n) * USB_OTG_EP_REG_SIZE))
#define FIFO(n)		(*(volatile uint32_t *)(BASE + USB_OTG_FIFO_BASE + (n) * USB_OTG_FIFO_SIZE))
#define PCGCCTL		(*(volatile uint32_t *)(BASE + USB_OTG_PCGCCTL_BASE))

#define EP0_SIZE	64u
#define DATA_SIZE	64u
#define NOTIFY_SIZE	8u
#define EP_DATA		1u	// bulk OUT 0x01, bulk IN 0x81
#define EP_NOTIFY	2u	// interrupt IN 0x82

// FIFO RAM in 32-bit words (1.25 KB on the FS core): RX 128, EP0 TX 16, EP1 TX 64, EP2 TX 16
#define RX_WORDS	128u
#define TX0_WORDS	16u
#define TX1_WORDS	64u
#define TX2_WORDS	16u

#define PKT_OUT_DATA	2u	// GRXSTSP packet status values
#define PKT_OUT_DONE	3u
#define PKT_SETUP_DONE	4u
#define PKT_SETUP_DATA	6u

#define RING_SIZE	8192u	// received bytes waiting for the protocol engine (a power of two)

#define WAIT_MS		1000u	// a host that stops reading for this long is gone

// ---- descriptors (USB 2.0 9.6, CDC 1.2 5, PSTN 5.3)

static const uint8_t dev_desc[18] = {
	18, 1, 0x00, 0x02,	// bLength, DEVICE, bcdUSB 2.00
	0xEF, 0x02, 0x01,	// miscellaneous class, common subclass, interface association
	EP0_SIZE,
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
	7, 5, 0x80 | EP_NOTIFY, 0x03, NOTIFY_SIZE, 0, 16,	// interrupt IN 0x82, 8 bytes, 16 ms
	9, 4, 1, 0, 2, 0x0A, 0x00, 0x00, 0,		// interface 1: data, two endpoints
	7, 5, EP_DATA, 0x02, DATA_SIZE, 0, 0,		// bulk OUT 0x01
	7, 5, 0x80 | EP_DATA, 0x02, DATA_SIZE, 0, 0,	// bulk IN 0x81
};

static const char *const strings[] = { NULL, "OpenDigitalRadio", "ODR flash stub", NULL, "ODR flash stub serial" };

// ---- state

static const char *serial_str;
static bool configured;
static bool ep1_armed;
static uint8_t setup[8];
static uint8_t ep0_buf[EP0_SIZE];
static uint32_t ep0_rx_len;
static bool ep0_expect_out;	// a control write's data stage is due (SET_LINE_CODING)
static uint8_t line_coding[7] = { 0x00, 0xC2, 0x01, 0x00, 0, 0, 8 };	// 115200 8N1: only remembered
static uint8_t desc_buf[2 + 2 * 32];

static uint8_t ring[RING_SIZE];
static volatile uint32_t ring_head, ring_tail;	// head: written by the RX path; tail: read by usb_read

static uint32_t ring_used(void)
{
	return ring_head - ring_tail;
}

// ---- FIFO access

static void fifo_read(uint8_t *dst, uint32_t n)
{
	for (uint32_t i = 0; i < n; i += 4u) {
		uint32_t w = FIFO(0);
		for (uint32_t k = 0; k < 4u && i + k < n; k++) {
			if (dst != NULL) {
				dst[i + k] = (uint8_t)(w >> (8u * k));
			}
		}
	}
}

static void fifo_write(unsigned ep, const uint8_t *src, uint32_t n)
{
	for (uint32_t i = 0; i < n; i += 4u) {
		uint32_t w = 0;
		for (uint32_t k = 0; k < 4u && i + k < n; k++) {
			w |= (uint32_t)src[i + k] << (8u * k);
		}
		FIFO(ep) = w;
	}
}

static bool wait_set(volatile uint32_t *reg, uint32_t mask, uint32_t ms)
{
	uint32_t t0 = stub_ms();
	while ((*reg & mask) == 0) {
		if (stub_ms() - t0 > ms) {
			return false;
		}
	}
	return true;
}

static bool wait_clear(volatile uint32_t *reg, uint32_t mask, uint32_t ms)
{
	uint32_t t0 = stub_ms();
	while ((*reg & mask) != 0) {
		if (stub_ms() - t0 > ms) {
			return false;
		}
	}
	return true;
}

static void flush_tx(uint32_t fifo)	// fifo 0x10 = all
{
	OTG->GRSTCTL = USB_OTG_GRSTCTL_TXFFLSH | (fifo << USB_OTG_GRSTCTL_TXFNUM_Pos);
	(void)wait_clear(&OTG->GRSTCTL, USB_OTG_GRSTCTL_TXFFLSH, 10);
}

static void flush_rx(void)
{
	OTG->GRSTCTL = USB_OTG_GRSTCTL_RXFFLSH;
	(void)wait_clear(&OTG->GRSTCTL, USB_OTG_GRSTCTL_RXFFLSH, 10);
}

// ---- endpoint 0

static void ep0_arm(void)
{
	OUTEP(0)->DOEPTSIZ = (3u << USB_OTG_DOEPTSIZ_STUPCNT_Pos) | (1u << USB_OTG_DOEPTSIZ_PKTCNT_Pos) | EP0_SIZE;
	OUTEP(0)->DOEPCTL |= USB_OTG_DOEPCTL_CNAK | USB_OTG_DOEPCTL_EPENA;
}

static void ep0_stall(void)
{
	INEP(0)->DIEPCTL |= USB_OTG_DIEPCTL_STALL;
	OUTEP(0)->DOEPCTL |= USB_OTG_DOEPCTL_STALL;	// both cleared by the core on the next SETUP
}

// One IN transfer on EP0 (at most 127 bytes, the transfer size field), packet by packet as the FIFO frees up
static void ep0_send(const uint8_t *data, uint32_t len)
{
	uint32_t pkts = len == 0 ? 1u : (len + EP0_SIZE - 1u) / EP0_SIZE;
	INEP(0)->DIEPTSIZ = (pkts << USB_OTG_DIEPTSIZ_PKTCNT_Pos) | len;
	INEP(0)->DIEPCTL |= USB_OTG_DIEPCTL_CNAK | USB_OTG_DIEPCTL_EPENA;
	for (uint32_t off = 0; off < len; off += EP0_SIZE) {
		uint32_t n = len - off < EP0_SIZE ? len - off : EP0_SIZE;
		uint32_t t0 = stub_ms();
		while ((INEP(0)->DTXFSTS & USB_OTG_DTXFSTS_INEPTFSAV) < (n + 3u) / 4u) {
			if (stub_ms() - t0 > 50u) {
				return;		// the host gave up on this request; the next SETUP starts over
			}
		}
		fifo_write(0, data + off, n);
	}
}

static void ep0_zlp(void)
{
	ep0_send(NULL, 0);
}

static void ep0_reply(const uint8_t *data, uint32_t len, uint16_t wlength)
{
	ep0_send(data, len < wlength ? len : wlength);
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
	for (unsigned ep = 1; ep <= 3u; ep++) {
		if (INEP(ep)->DIEPCTL & USB_OTG_DIEPCTL_EPENA) {
			INEP(ep)->DIEPCTL |= USB_OTG_DIEPCTL_SNAK | USB_OTG_DIEPCTL_EPDIS;
		}
		INEP(ep)->DIEPCTL &= ~USB_OTG_DIEPCTL_USBAEP;
		if (OUTEP(ep)->DOEPCTL & USB_OTG_DOEPCTL_EPENA) {
			OUTEP(ep)->DOEPCTL |= USB_OTG_DOEPCTL_SNAK | USB_OTG_DOEPCTL_EPDIS;
		}
		OUTEP(ep)->DOEPCTL &= ~USB_OTG_DOEPCTL_USBAEP;
	}
	configured = false;
	ep1_armed = false;
}

static void endpoints_on(void)
{
	INEP(EP_DATA)->DIEPCTL = DATA_SIZE | (2u << USB_OTG_DIEPCTL_EPTYP_Pos) | (1u << USB_OTG_DIEPCTL_TXFNUM_Pos) |
				 USB_OTG_DIEPCTL_SD0PID_SEVNFRM | USB_OTG_DIEPCTL_USBAEP | USB_OTG_DIEPCTL_SNAK;
	INEP(EP_NOTIFY)->DIEPCTL = NOTIFY_SIZE | (3u << USB_OTG_DIEPCTL_EPTYP_Pos) | (2u << USB_OTG_DIEPCTL_TXFNUM_Pos) |
				   USB_OTG_DIEPCTL_SD0PID_SEVNFRM | USB_OTG_DIEPCTL_USBAEP | USB_OTG_DIEPCTL_SNAK;
	OUTEP(EP_DATA)->DOEPCTL = DATA_SIZE | (2u << USB_OTG_DOEPCTL_EPTYP_Pos) | USB_OTG_DOEPCTL_SD0PID_SEVNFRM |
				  USB_OTG_DOEPCTL_USBAEP | USB_OTG_DOEPCTL_SNAK;
	ep1_armed = false;
	configured = true;
}

static void halt(uint8_t ep_addr, bool on)
{
	unsigned ep = ep_addr & 0x0Fu;
	if (ep == 0 || ep > 3u) {
		return;
	}
	if (ep_addr & 0x80u) {
		INEP(ep)->DIEPCTL = on ? (INEP(ep)->DIEPCTL | USB_OTG_DIEPCTL_STALL)
				       : ((INEP(ep)->DIEPCTL & ~USB_OTG_DIEPCTL_STALL) | USB_OTG_DIEPCTL_SD0PID_SEVNFRM);
	} else {
		OUTEP(ep)->DOEPCTL = on ? (OUTEP(ep)->DOEPCTL | USB_OTG_DOEPCTL_STALL)
					: ((OUTEP(ep)->DOEPCTL & ~USB_OTG_DOEPCTL_STALL) | USB_OTG_DOEPCTL_SD0PID_SEVNFRM);
		if (!on && ep == EP_DATA) {
			ep1_armed = false;
		}
	}
}

static void handle_setup(void)
{
	uint8_t type = setup[0], req = setup[1];
	uint16_t value = (uint16_t)(setup[2] | (setup[3] << 8));
	uint16_t index = (uint16_t)(setup[4] | (setup[5] << 8));
	uint16_t length = (uint16_t)(setup[6] | (setup[7] << 8));
	static uint8_t small[2];

	ep0_arm();		// ready for a data stage or the host's status packet
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
		ep0_reply(small, 2, length);
		break;
	case 0x01:				// CLEAR_FEATURE
	case 0x03:				// SET_FEATURE
		if ((type & 0x1Fu) == 2u && value == 0) {
			halt((uint8_t)index, req == 0x03);	// ENDPOINT_HALT
		}
		ep0_zlp();
		break;
	case 0x05:				// SET_ADDRESS: takes effect at once on this core (before the status stage)
		DEV->DCFG = (DEV->DCFG & ~USB_OTG_DCFG_DAD) | ((uint32_t)(value & 0x7Fu) << USB_OTG_DCFG_DAD_Pos);
		ep0_zlp();
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
				ep0_stall();
			} else {
				ep0_reply(desc_buf, n, length);
			}
		} else {
			ep0_stall();		// device qualifier and the rest: full speed only
		}
		break;
	}
	case 0x08:				// GET_CONFIGURATION
		small[0] = configured ? 1u : 0u;
		ep0_reply(small, 1, length);
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
		ep0_reply(small, 1, length);
		break;
	case 0x0B:				// SET_INTERFACE: only alternate setting 0 exists
		ep0_zlp();
		break;
	default:
		ep0_stall();
		break;
	}
}

static void bus_reset(void)
{
	endpoints_off();
	DEV->DCFG &= ~USB_OTG_DCFG_DAD;
	OUTEP(0)->DOEPCTL |= USB_OTG_DOEPCTL_SNAK;
	flush_tx(0x10u);
	flush_rx();
	for (unsigned ep = 0; ep <= 3u; ep++) {
		INEP(ep)->DIEPINT = 0xFFFFu;
		OUTEP(ep)->DOEPINT = 0xFFFFu;
	}
	ep0_arm();
}

static void rx_pop(void)
{
	uint32_t st = OTG->GRXSTSP;
	unsigned ep = st & USB_OTG_GRXSTSP_EPNUM;
	uint32_t n = (st & USB_OTG_GRXSTSP_BCNT) >> USB_OTG_GRXSTSP_BCNT_Pos;
	uint32_t pkt = (st & USB_OTG_GRXSTSP_PKTSTS) >> USB_OTG_GRXSTSP_PKTSTS_Pos;

	switch (pkt) {
	case PKT_SETUP_DATA:
		fifo_read(setup, n < 8u ? n : 8u);
		if (n > 8u) {
			fifo_read(NULL, n - 8u);
		}
		break;
	case PKT_SETUP_DONE:
		handle_setup();
		break;
	case PKT_OUT_DATA:
		if (ep == 0) {
			uint32_t room = sizeof(ep0_buf) - ep0_rx_len;
			uint32_t take = n < room ? n : room;
			fifo_read(ep0_buf + ep0_rx_len, take);
			fifo_read(NULL, n - take);
			ep0_rx_len += take;
		} else if (ep == EP_DATA) {
			uint8_t tmp[DATA_SIZE];
			uint32_t take = n < sizeof(tmp) ? n : sizeof(tmp);
			fifo_read(tmp, take);
			fifo_read(NULL, n - take);
			for (uint32_t i = 0; i < take && ring_used() < RING_SIZE; i++) {
				ring[ring_head % RING_SIZE] = tmp[i];
				ring_head++;
			}
		} else {
			fifo_read(NULL, n);
		}
		break;
	case PKT_OUT_DONE:
		if (ep == 0) {
			if (ep0_expect_out) {	// SET_LINE_CODING's data arrived: status stage
				if (ep0_rx_len >= sizeof(line_coding)) {
					memcpy(line_coding, ep0_buf, sizeof(line_coding));
				}
				ep0_expect_out = false;
				ep0_zlp();
			}
			ep0_arm();
		} else if (ep == EP_DATA) {
			ep1_armed = false;
		}
		break;
	default:
		break;	// global OUT NAK and the like: nothing to read
	}
}

void usb_poll(void)
{
	uint32_t g = OTG->GINTSTS;

	if (g & USB_OTG_GINTSTS_USBRST) {
		OTG->GINTSTS = USB_OTG_GINTSTS_USBRST;
		bus_reset();
	}
	if (g & USB_OTG_GINTSTS_ENUMDNE) {
		OTG->GINTSTS = USB_OTG_GINTSTS_ENUMDNE;
		INEP(0)->DIEPCTL &= ~USB_OTG_DIEPCTL_MPSIZ;	// 0 = 64-byte control packets
		DEV->DCTL |= USB_OTG_DCTL_CGINAK;
	}
	if (g & USB_OTG_GINTSTS_USBSUSP) {
		OTG->GINTSTS = USB_OTG_GINTSTS_USBSUSP;
	}
	for (unsigned i = 0; i < 16u && (OTG->GINTSTS & USB_OTG_GINTSTS_RXFLVL); i++) {
		rx_pop();
	}
	// The endpoint interrupt bits are not used (the RX FIFO's status words drive everything): keep them clear
	OUTEP(0)->DOEPINT = 0xFFFFu;
	OUTEP(EP_DATA)->DOEPINT = 0xFFFFu;
	INEP(0)->DIEPINT = 0xFFFFu;
	INEP(EP_DATA)->DIEPINT = 0xFFFFu;

	if (configured && !ep1_armed && RING_SIZE - ring_used() >= DATA_SIZE) {
		OUTEP(EP_DATA)->DOEPTSIZ = (1u << USB_OTG_DOEPTSIZ_PKTCNT_Pos) | DATA_SIZE;
		OUTEP(EP_DATA)->DOEPCTL |= USB_OTG_DOEPCTL_CNAK | USB_OTG_DOEPCTL_EPENA;
		ep1_armed = true;
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
	while (INEP(EP_DATA)->DIEPCTL & USB_OTG_DIEPCTL_EPENA) {
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
	INEP(EP_DATA)->DIEPTSIZ = (1u << USB_OTG_DIEPTSIZ_PKTCNT_Pos) | n;
	INEP(EP_DATA)->DIEPCTL |= USB_OTG_DIEPCTL_CNAK | USB_OTG_DIEPCTL_EPENA;
	uint32_t t0 = stub_ms();
	while ((INEP(EP_DATA)->DTXFSTS & USB_OTG_DTXFSTS_INEPTFSAV) < (n + 3u) / 4u) {
		if (stub_ms() - t0 > WAIT_MS) {
			return false;
		}
	}
	fifo_write(EP_DATA, data, n);
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

// Take over ODR's configured device: the address and the endpoints stay, a transfer TinyUSB left half done on
// the IN side is cut off (the host saw the ENTER reply complete before the jump), and the FIFOs start empty
static bool adopt(void)
{
	if ((DEV->DCFG & USB_OTG_DCFG_DAD) == 0 || !(INEP(EP_DATA)->DIEPCTL & USB_OTG_DIEPCTL_USBAEP) ||
	    !(OUTEP(EP_DATA)->DOEPCTL & USB_OTG_DOEPCTL_USBAEP)) {
		return false;
	}
	for (unsigned ep = 0; ep <= EP_NOTIFY; ep++) {
		if (INEP(ep)->DIEPCTL & USB_OTG_DIEPCTL_EPENA) {
			INEP(ep)->DIEPCTL |= USB_OTG_DIEPCTL_SNAK;
			(void)wait_set(&INEP(ep)->DIEPINT, USB_OTG_DIEPINT_INEPNE, 5);
			INEP(ep)->DIEPCTL |= USB_OTG_DIEPCTL_EPDIS;
			(void)wait_set(&INEP(ep)->DIEPINT, USB_OTG_DIEPINT_EPDISD, 5);
		}
		INEP(ep)->DIEPINT = 0xFFFFu;
	}
	flush_tx(0x10u);
	// The data OUT endpoint: TinyUSB may have left it armed; then the next packet just lands in the RX FIFO
	ep1_armed = (OUTEP(EP_DATA)->DOEPCTL & USB_OTG_DOEPCTL_EPENA) != 0;
	configured = true;
	DEV->DCTL |= USB_OTG_DCTL_CGINAK | USB_OTG_DCTL_CGONAK;
	ep0_arm();
	return true;
}

static void cold_start(void)
{
	DEV->DCTL |= USB_OTG_DCTL_SDIS;		// the host sees an unplug
	OTG->GAHBCFG = 0;			// interrupt line off (everything is polled)
	(void)wait_set(&OTG->GRSTCTL, USB_OTG_GRSTCTL_AHBIDL, 10);
	OTG->GRSTCTL |= USB_OTG_GRSTCTL_CSRST;
	(void)wait_clear(&OTG->GRSTCTL, USB_OTG_GRSTCTL_CSRST, 10);
	(void)wait_set(&OTG->GRSTCTL, USB_OTG_GRSTCTL_AHBIDL, 10);
	DEV->DCTL |= USB_OTG_DCTL_SDIS;		// the reset may have cleared it
	// Forced device mode, the full speed PHY, turnaround 6 (AHB at 32 MHz and more)
	OTG->GUSBCFG = USB_OTG_GUSBCFG_PHYSEL | USB_OTG_GUSBCFG_FDMOD | (6u << USB_OTG_GUSBCFG_TRDT_Pos);
	stub_delay_ms(25);
	OTG->GCCFG = USB_OTG_GCCFG_PWRDWN | USB_OTG_GCCFG_NOVBUSSENS;	// transceiver on, no VBUS sensing (16)
	PCGCCTL = 0;
	DEV->DCFG = 3u << USB_OTG_DCFG_DSPD_Pos;			// full speed, internal PHY; address 0
	OTG->GRXFSIZ = RX_WORDS;
	OTG->DIEPTXF0_HNPTXFSIZ = (TX0_WORDS << 16) | RX_WORDS;
	OTG->DIEPTXF[0] = (TX1_WORDS << 16) | (RX_WORDS + TX0_WORDS);
	OTG->DIEPTXF[1] = (TX2_WORDS << 16) | (RX_WORDS + TX0_WORDS + TX1_WORDS);
	flush_tx(0x10u);
	flush_rx();
	DEV->DIEPMSK = 0;
	DEV->DOEPMSK = 0;
	DEV->DAINTMSK = 0;
	for (unsigned ep = 0; ep <= 3u; ep++) {
		INEP(ep)->DIEPINT = 0xFFFFu;
		OUTEP(ep)->DOEPINT = 0xFFFFu;
	}
	OTG->GINTSTS = 0xFFFFFFFFu;
	OTG->GINTMSK = 0;
	configured = false;
	ep1_armed = false;
	stub_delay_ms(100);			// long enough that every host notices the unplug
	DEV->DCTL &= ~USB_OTG_DCTL_SDIS;
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
