// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Polled CDC-ACM device on the STM32F4 OTG_FS core (stub/common/stubusb.h). Written from the reference manual's OTG_FS
// device programming model (slave mode: the CPU moves every packet through the FIFOs). The descriptors and the control
// requests are shared (stub/common/stubcdc.c). The core's status bits are polled; the interrupt line stays off.
#include <stddef.h>
#include <string.h>
#include "stm32f4xx.h"
#include "stubcdc.h"
#include "stubrt.h"
#include "stubusb.h"

#define BASE		USB_OTG_FS_PERIPH_BASE
#define OTG		((USB_OTG_GlobalTypeDef *)BASE)
#define DEV		((USB_OTG_DeviceTypeDef *)(BASE + USB_OTG_DEVICE_BASE))
#define INEP(n)		((USB_OTG_INEndpointTypeDef *)(BASE + USB_OTG_IN_ENDPOINT_BASE + (n) * USB_OTG_EP_REG_SIZE))
#define OUTEP(n)	((USB_OTG_OUTEndpointTypeDef *)(BASE + USB_OTG_OUT_ENDPOINT_BASE + (n) * USB_OTG_EP_REG_SIZE))
#define FIFO(n)		(*(volatile uint32_t *)(BASE + USB_OTG_FIFO_BASE + (n) * USB_OTG_FIFO_SIZE))
#define PCGCCTL		(*(volatile uint32_t *)(BASE + USB_OTG_PCGCCTL_BASE))

#define EP0_SIZE	STUBCDC_EP0_SIZE
#define DATA_SIZE	STUBCDC_DATA_SIZE
#define NOTIFY_SIZE	STUBCDC_NOTIFY_SIZE
#define EP_DATA		STUBCDC_EP_DATA
#define EP_NOTIFY	STUBCDC_EP_NOTIFY

// FIFO RAM in 32-bit words (1.25 KB on the FS core): RX 128, EP0 TX 16, EP1 TX 64, EP2 TX 16
#define RX_WORDS	128u
#define TX0_WORDS	16u
#define TX1_WORDS	64u
#define TX2_WORDS	16u

#define PKT_OUT_DATA	2u	// GRXSTSP packet status values
#define PKT_OUT_DONE	3u
#define PKT_SETUP_DONE	4u
#define PKT_SETUP_DATA	6u

#define WAIT_MS		1000u	// a host that stops reading for this long is gone

// ---- state

static bool ep1_armed;
static uint8_t setup[8];

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
	stubcdc_configured = false;
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
	stubcdc_configured = true;
}

void cdc_hw_halt(uint8_t ep_addr, bool on)
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

// ---- what the shared control request code (stubcdc.c) asks of the core

void cdc_hw_ep0_send(const uint8_t *data, uint32_t len)
{
	ep0_send(data, len);
}

void cdc_hw_ep0_stall(void)
{
	ep0_stall();
}

// SET_ADDRESS takes effect at once on this core (before the status stage)
void cdc_hw_set_address(uint8_t addr)
{
	DEV->DCFG = (DEV->DCFG & ~USB_OTG_DCFG_DAD) | ((uint32_t)addr << USB_OTG_DCFG_DAD_Pos);
}

void cdc_hw_endpoints(bool on)
{
	if (on) {
		endpoints_on();
	} else {
		endpoints_off();
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
		ep0_arm();		// ready for a data stage or the host's status packet
		stubcdc_setup(setup);
		break;
	case PKT_OUT_DATA:
		if (ep == 0 || ep == EP_DATA) {
			uint8_t tmp[DATA_SIZE];
			uint32_t take = n < sizeof(tmp) ? n : sizeof(tmp);
			fifo_read(tmp, take);
			fifo_read(NULL, n - take);
			if (ep == 0) {
				stubcdc_ep0_data(tmp, take);
			} else {
				stubcdc_rx(tmp, take);
			}
		} else {
			fifo_read(NULL, n);
		}
		break;
	case PKT_OUT_DONE:
		if (ep == 0) {
			stubcdc_ep0_out_done();	// SET_LINE_CODING's data arrived: status stage
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

	if (stubcdc_configured && !ep1_armed && stubcdc_rx_room() >= DATA_SIZE) {
		OUTEP(EP_DATA)->DOEPTSIZ = (1u << USB_OTG_DOEPTSIZ_PKTCNT_Pos) | DATA_SIZE;
		OUTEP(EP_DATA)->DOEPCTL |= USB_OTG_DOEPCTL_CNAK | USB_OTG_DOEPCTL_EPENA;
		ep1_armed = true;
	}
}

static bool in_idle(uint32_t ms)
{
	uint32_t t0 = stub_ms();
	while (INEP(EP_DATA)->DIEPCTL & USB_OTG_DIEPCTL_EPENA) {
		usb_poll();
		if (!stubcdc_configured || stub_ms() - t0 > ms) {
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
	if (!stubcdc_configured) {
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
	stubcdc_configured = true;
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
	stubcdc_configured = false;
	ep1_armed = false;
	stub_delay_ms(100);			// long enough that every host notices the unplug
	DEV->DCTL &= ~USB_OTG_DCTL_SDIS;
}

bool usb_start(bool want_adopt, uint32_t state, const char *serial)
{
	(void)state;		// the OTG_FS core keeps its own data toggles
	stubcdc_init(serial);
	if (want_adopt && adopt()) {
		return true;
	}
	cold_start();
	return false;
}
