// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Polled CDC-ACM device on the AT32F43x OTGFS1 core (at32stub_usb.h). The core is a Synopsys DWC2 with the register
// map of the STM32F4's OTG_FS (the AT32F435/437 reference manual's OTGFS chapter; RadioFoundry runs TinyUSB's dwc2
// driver on it, platform/at32f43x/usb.c), so this is the STM32F4 stub's device (stub/stm32f4/f4stub_usb.c) with the
// registers by offset and Artery's clock and pins: slave mode, the CPU moves every packet through the FIFOs, USB 2.0
// chapter 9 and the CDC 1.2 / PSTN class documents. The core's status bits are polled; the interrupt line stays off.
#include <stddef.h>
#include <string.h>
#include "at32f43x.h"
#include "at32stub_usb.h"

#define OTG(off)		AT32_REG(OTGFS1_BASE + (off))
#define GAHBCFG			OTG(0x008u)
#define GUSBCFG			OTG(0x00Cu)
#define GRSTCTL			OTG(0x010u)
#define GINTSTS			OTG(0x014u)
#define GINTMSK			OTG(0x018u)
#define GRXSTSP			OTG(0x020u)
#define GRXFSIZ			OTG(0x024u)
#define DIEPTXF0		OTG(0x028u)	// the non-periodic (EP0) transmit FIFO size
#define GCCFG			OTG(0x038u)
#define DIEPTXF(n)		OTG(0x104u + 4u * ((n) - 1u))
#define DCFG			OTG(0x800u)
#define DCTL			OTG(0x804u)
#define DIEPMSK			OTG(0x810u)
#define DOEPMSK			OTG(0x814u)
#define DAINTMSK		OTG(0x81Cu)
#define DIEPCTL(n)		OTG(0x900u + 0x20u * (n))
#define DIEPINT(n)		OTG(0x908u + 0x20u * (n))
#define DIEPTSIZ(n)		OTG(0x910u + 0x20u * (n))
#define DTXFSTS(n)		OTG(0x918u + 0x20u * (n))
#define DOEPCTL(n)		OTG(0xB00u + 0x20u * (n))
#define DOEPINT(n)		OTG(0xB08u + 0x20u * (n))
#define DOEPTSIZ(n)		OTG(0xB10u + 0x20u * (n))
#define PCGCCTL			OTG(0xE00u)
#define FIFO(n)			OTG(0x1000u + 0x1000u * (n))

#define GUSBCFG_PHYSEL		(1u << 6)	// the full-speed transceiver
#define GUSBCFG_TRDT_POS	10u
#define GUSBCFG_FDMOD		(1u << 30)	// forced device mode
#define GRSTCTL_CSRST		(1u << 0)
#define GRSTCTL_RXFFLSH		(1u << 4)
#define GRSTCTL_TXFFLSH		(1u << 5)
#define GRSTCTL_TXFNUM_POS	6u
#define GRSTCTL_AHBIDL		(1u << 31)
#define GINTSTS_RXFLVL		(1u << 4)
#define GINTSTS_USBSUSP		(1u << 11)
#define GINTSTS_USBRST		(1u << 12)
#define GINTSTS_ENUMDNE		(1u << 13)
#define GRXSTSP_EPNUM		0xFu
#define GRXSTSP_BCNT_POS	4u
#define GRXSTSP_BCNT		(0x7FFu << GRXSTSP_BCNT_POS)
#define GRXSTSP_PKTSTS_POS	17u
#define GRXSTSP_PKTSTS		(0xFu << GRXSTSP_PKTSTS_POS)
#define DCFG_DSPD_FS		3u		// full speed on the internal transceiver
#define DCFG_DAD_POS		4u
#define DCFG_DAD		(0x7Fu << DCFG_DAD_POS)
#define DCTL_SDIS		(1u << 1)	// soft disconnect: the core's D+ pull-up off
#define DCTL_CGINAK		(1u << 8)
#define DCTL_CGONAK		(1u << 10)
#define EPCTL_MPSIZ		0x7FFu
#define EPCTL_USBAEP		(1u << 15)
#define EPCTL_EPTYP_POS		18u
#define EPCTL_STALL		(1u << 21)
#define DIEPCTL_TXFNUM_POS	22u
#define EPCTL_CNAK		(1u << 26)
#define EPCTL_SNAK		(1u << 27)
#define EPCTL_SD0PID		(1u << 28)
#define EPCTL_EPDIS		(1u << 30)
#define EPCTL_EPENA		(1u << 31)
#define DIEPINT_EPDISD		(1u << 1)
#define DIEPINT_INEPNE		(1u << 6)
#define TSIZ_PKTCNT_POS		19u
#define DOEPTSIZ_STUPCNT_POS	29u
#define DTXFSTS_INEPTFSAV	0xFFFFu

#define EP0_SIZE	64u
#define DATA_SIZE	64u
#define NOTIFY_SIZE	8u
#define EP_DATA		1u	// bulk OUT 0x01, bulk IN 0x81
#define EP_NOTIFY	2u	// interrupt IN 0x82

// FIFO RAM in 32-bit words (320 on this core, TinyUSB's dwc2_at32.h): RX 128, EP0 TX 16, EP1 TX 64, EP2 TX 16
#define RX_WORDS	128u
#define TX0_WORDS	16u
#define TX1_WORDS	64u
#define TX2_WORDS	16u

#define PKT_OUT_DATA	2u	// GRXSTSP packet status values
#define PKT_OUT_DONE	3u
#define PKT_SETUP_DONE	4u
#define PKT_SETUP_DATA	6u

#define RING_SIZE	4096u	// received bytes waiting for the protocol engine (a power of two)

#define WAIT_MS		1000u	// a host that stops reading for this long is gone

#define OTGFS_AF	10u	// PA11 / PA12's multiplexed function for OTGFS1
// The HICK / 6 cycles in a 1 ms frame the ACC trims the HICK to (platform/at32f43x/usb.c)
#define ACC_WINDOW_LOW	7980u
#define ACC_WINDOW_MID	8000u
#define ACC_WINDOW_HIGH	8020u

// ---- descriptors (USB 2.0 9.6, CDC 1.2 5, PSTN 5.3)

static const uint8_t dev_desc[18] = {
	18, 1, 0x00, 0x02,	// bLength, DEVICE, bcdUSB 2.00
	0xEF, 0x02, 0x01,	// miscellaneous class, common subclass, interface association
	EP0_SIZE,
	0xC9, 0x1F, 0x94, 0x00,	// VID 0x1FC9, PID 0x0094: RadioFoundry's serial-port identity (platform.h AT32_USB_*)
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
	GRSTCTL = GRSTCTL_TXFFLSH | (fifo << GRSTCTL_TXFNUM_POS);
	(void)wait_clear(&GRSTCTL, GRSTCTL_TXFFLSH, 10);
}

static void flush_rx(void)
{
	GRSTCTL = GRSTCTL_RXFFLSH;
	(void)wait_clear(&GRSTCTL, GRSTCTL_RXFFLSH, 10);
}

// ---- endpoint 0

static void ep0_arm(void)
{
	DOEPTSIZ(0) = (3u << DOEPTSIZ_STUPCNT_POS) | (1u << TSIZ_PKTCNT_POS) | EP0_SIZE;
	DOEPCTL(0) |= EPCTL_CNAK | EPCTL_EPENA;
}

static void ep0_stall(void)
{
	DIEPCTL(0) |= EPCTL_STALL;
	DOEPCTL(0) |= EPCTL_STALL;	// both cleared by the core on the next SETUP
}

// One IN transfer on EP0 (at most 127 bytes, the transfer size field), packet by packet as the FIFO frees up
static void ep0_send(const uint8_t *data, uint32_t len)
{
	uint32_t pkts = len == 0 ? 1u : (len + EP0_SIZE - 1u) / EP0_SIZE;
	DIEPTSIZ(0) = (pkts << TSIZ_PKTCNT_POS) | len;
	DIEPCTL(0) |= EPCTL_CNAK | EPCTL_EPENA;
	for (uint32_t off = 0; off < len; off += EP0_SIZE) {
		uint32_t n = len - off < EP0_SIZE ? len - off : EP0_SIZE;
		uint32_t t0 = stub_ms();
		while ((DTXFSTS(0) & DTXFSTS_INEPTFSAV) < (n + 3u) / 4u) {
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
		if (DIEPCTL(ep) & EPCTL_EPENA) {
			DIEPCTL(ep) |= EPCTL_SNAK | EPCTL_EPDIS;
		}
		DIEPCTL(ep) &= ~EPCTL_USBAEP;
		if (DOEPCTL(ep) & EPCTL_EPENA) {
			DOEPCTL(ep) |= EPCTL_SNAK | EPCTL_EPDIS;
		}
		DOEPCTL(ep) &= ~EPCTL_USBAEP;
	}
	configured = false;
	ep1_armed = false;
}

static void endpoints_on(void)
{
	DIEPCTL(EP_DATA) = DATA_SIZE | (2u << EPCTL_EPTYP_POS) | (1u << DIEPCTL_TXFNUM_POS) |
				 EPCTL_SD0PID | EPCTL_USBAEP | EPCTL_SNAK;
	DIEPCTL(EP_NOTIFY) = NOTIFY_SIZE | (3u << EPCTL_EPTYP_POS) | (2u << DIEPCTL_TXFNUM_POS) |
				   EPCTL_SD0PID | EPCTL_USBAEP | EPCTL_SNAK;
	DOEPCTL(EP_DATA) = DATA_SIZE | (2u << EPCTL_EPTYP_POS) | EPCTL_SD0PID |
				  EPCTL_USBAEP | EPCTL_SNAK;
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
		DIEPCTL(ep) = on ? (DIEPCTL(ep) | EPCTL_STALL)
				       : ((DIEPCTL(ep) & ~EPCTL_STALL) | EPCTL_SD0PID);
	} else {
		DOEPCTL(ep) = on ? (DOEPCTL(ep) | EPCTL_STALL)
					: ((DOEPCTL(ep) & ~EPCTL_STALL) | EPCTL_SD0PID);
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
		DCFG = (DCFG & ~DCFG_DAD) | ((uint32_t)(value & 0x7Fu) << DCFG_DAD_POS);
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
	DCFG &= ~DCFG_DAD;
	DOEPCTL(0) |= EPCTL_SNAK;
	flush_tx(0x10u);
	flush_rx();
	for (unsigned ep = 0; ep <= 3u; ep++) {
		DIEPINT(ep) = 0xFFFFu;
		DOEPINT(ep) = 0xFFFFu;
	}
	ep0_arm();
}

static void rx_pop(void)
{
	uint32_t st = GRXSTSP;
	unsigned ep = st & GRXSTSP_EPNUM;
	uint32_t n = (st & GRXSTSP_BCNT) >> GRXSTSP_BCNT_POS;
	uint32_t pkt = (st & GRXSTSP_PKTSTS) >> GRXSTSP_PKTSTS_POS;

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
	uint32_t g = GINTSTS;

	if (g & GINTSTS_USBRST) {
		GINTSTS = GINTSTS_USBRST;
		bus_reset();
	}
	if (g & GINTSTS_ENUMDNE) {
		GINTSTS = GINTSTS_ENUMDNE;
		DIEPCTL(0) &= ~EPCTL_MPSIZ;	// 0 = 64-byte control packets
		DCTL |= DCTL_CGINAK;
	}
	if (g & GINTSTS_USBSUSP) {
		GINTSTS = GINTSTS_USBSUSP;
	}
	for (unsigned i = 0; i < 16u && (GINTSTS & GINTSTS_RXFLVL); i++) {
		rx_pop();
	}
	// The endpoint interrupt bits are not used (the RX FIFO's status words drive everything): keep them clear
	DOEPINT(0) = 0xFFFFu;
	DOEPINT(EP_DATA) = 0xFFFFu;
	DIEPINT(0) = 0xFFFFu;
	DIEPINT(EP_DATA) = 0xFFFFu;

	if (configured && !ep1_armed && RING_SIZE - ring_used() >= DATA_SIZE) {
		DOEPTSIZ(EP_DATA) = (1u << TSIZ_PKTCNT_POS) | DATA_SIZE;
		DOEPCTL(EP_DATA) |= EPCTL_CNAK | EPCTL_EPENA;
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
	while (DIEPCTL(EP_DATA) & EPCTL_EPENA) {
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
	DIEPTSIZ(EP_DATA) = (1u << TSIZ_PKTCNT_POS) | n;
	DIEPCTL(EP_DATA) |= EPCTL_CNAK | EPCTL_EPENA;
	uint32_t t0 = stub_ms();
	while ((DTXFSTS(EP_DATA) & DTXFSTS_INEPTFSAV) < (n + 3u) / 4u) {
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

// Take over RadioFoundry's configured device: the address and the endpoints stay, a transfer TinyUSB left half done on
// the IN side is cut off (the host saw the ENTER reply complete before the jump), and the FIFOs start empty. Anything
// but the serial port's bulk pair and notification endpoint enumerates afresh instead
static bool adopt(void)
{
	if ((DCFG & DCFG_DAD) == 0 || !(DIEPCTL(EP_DATA) & EPCTL_USBAEP) || !(DOEPCTL(EP_DATA) & EPCTL_USBAEP) ||
	    !(DIEPCTL(EP_NOTIFY) & EPCTL_USBAEP)) {
		return false;
	}
	for (unsigned ep = 0; ep <= EP_NOTIFY; ep++) {
		if (DIEPCTL(ep) & EPCTL_EPENA) {
			DIEPCTL(ep) |= EPCTL_SNAK;
			(void)wait_set(&DIEPINT(ep), DIEPINT_INEPNE, 5);
			DIEPCTL(ep) |= EPCTL_EPDIS;
			(void)wait_set(&DIEPINT(ep), DIEPINT_EPDISD, 5);
		}
		DIEPINT(ep) = 0xFFFFu;
	}
	flush_tx(0x10u);
	// The data OUT endpoint: TinyUSB may have left it armed; then the next packet just lands in the RX FIFO
	ep1_armed = (DOEPCTL(EP_DATA) & EPCTL_EPENA) != 0;
	configured = true;
	DCTL |= DCTL_CGINAK | DCTL_CGONAK;
	ep0_arm();
	return true;
}

// The clocks and pins the core needs, as platform/at32f43x/usb.c sets them: RadioFoundry may have had USB off. The
// 48 MHz HICK to the USB, the ACC trimming it from the host's start of frame, PA11 / PA12 on the core. Each step is
// harmless when it is done already
static void clocks_and_pins(void)
{
	CRM_AHBEN1 |= CRM_AHBEN1_GPIOAEN;
	CRM_MISC1 |= CRM_MISC1_USB_HICK_A;
	CRM_MISC1 |= CRM_MISC1_USB_HICK_B;
	CRM_MISC1 |= CRM_MISC1_USB_HICK_C;
	CRM_APB2EN |= CRM_APB2EN_ACCEN;
	(void)CRM_APB2EN;
	if ((ACC_CTRL1 & 1u) == 0) {
		ACC_C1 = ACC_WINDOW_LOW;
		ACC_C2 = ACC_WINDOW_MID;
		ACC_C3 = ACC_WINDOW_HIGH;
		ACC_CTRL1 |= 1u << 1;
		ACC_CTRL1 |= 1u << 0;
	}
	at32_gpio_t *pa = GPIOA;
	pa->MUXH = (pa->MUXH & ~(0xFFu << 12)) | (OTGFS_AF << 12) | (OTGFS_AF << 16);
	pa->CFGR = (pa->CFGR & ~(0xFu << 22)) | (0xAu << 22);	// PA11, PA12 multiplexed
	CRM_AHBEN2 |= CRM_AHBEN2_OTGFS1EN;
	(void)CRM_AHBEN2;
}

static void cold_start(void)
{
	clocks_and_pins();
	DCTL |= DCTL_SDIS;			// the host sees an unplug
	GAHBCFG = 0;				// interrupt line off (everything is polled)
	(void)wait_set(&GRSTCTL, GRSTCTL_AHBIDL, 10);
	GRSTCTL |= GRSTCTL_CSRST;
	(void)wait_clear(&GRSTCTL, GRSTCTL_CSRST, 10);
	(void)wait_set(&GRSTCTL, GRSTCTL_AHBIDL, 10);
	DCTL |= DCTL_SDIS;			// the reset may have cleared it
	// Forced device mode, the full speed transceiver, turnaround 5 (TinyUSB's for a full-speed DWC2)
	GUSBCFG = GUSBCFG_PHYSEL | GUSBCFG_FDMOD | (5u << GUSBCFG_TRDT_POS);
	stub_delay_ms(25);
	GCCFG |= OTGFS_GCCFG_PWRDOWN | OTGFS_GCCFG_VBUSIG;	// transceiver on; no VBUS pin, the session always valid
	PCGCCTL = 0;
	DCFG = DCFG_DSPD_FS;					// address 0
	GRXFSIZ = RX_WORDS;
	DIEPTXF0 = (TX0_WORDS << 16) | RX_WORDS;
	DIEPTXF(1) = (TX1_WORDS << 16) | (RX_WORDS + TX0_WORDS);
	DIEPTXF(2) = (TX2_WORDS << 16) | (RX_WORDS + TX0_WORDS + TX1_WORDS);
	flush_tx(0x10u);
	flush_rx();
	DIEPMSK = 0;
	DOEPMSK = 0;
	DAINTMSK = 0;
	for (unsigned ep = 0; ep <= 3u; ep++) {
		DIEPINT(ep) = 0xFFFFu;
		DOEPINT(ep) = 0xFFFFu;
	}
	GINTSTS = 0xFFFFFFFFu;
	GINTMSK = 0;
	configured = false;
	ep1_armed = false;
	stub_delay_ms(100);			// long enough that every host notices the unplug
	DCTL &= ~DCTL_SDIS;
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
