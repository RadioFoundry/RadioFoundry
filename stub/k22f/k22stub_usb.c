// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Polled CDC-ACM device on the K22F full-speed USB controller (stub/common/stubusb.h), written from the reference
// manual's USB chapter: a buffer descriptor table in RAM, each endpoint direction with an even and an odd descriptor
// the controller uses in turn, ISTAT / STAT polled for tokens, bus resets and stalls. The descriptors and the control
// requests are shared (stub/common/stubcdc.c). The USB interrupt stays off.
//
// The stub keeps its own table and its own idea of the even / odd turn: at the start (adopted or not) CTL.ODDRST puts
// every endpoint back to even. Adopting RadioFoundry's device keeps the address and the endpoint registers; the data toggles
// come from RadioFoundry (r1, platform/k22f/stub_handoff.h). Without them the IN endpoints start at DATA0 (if that is wrong the
// host drops one reply and asks again) and the OUT endpoint takes either toggle.
#include <stddef.h>
#include <string.h>
#include "k22f.h"
#include "k22f_usb_regs.h"
#include "stub_handoff.h"
#include "stubcdc.h"
#include "stubrt.h"
#include "stubusb.h"

#define EP0_SIZE	STUBCDC_EP0_SIZE
#define DATA_SIZE	STUBCDC_DATA_SIZE
#define EP_DATA		STUBCDC_EP_DATA
#define EP_NOTIFY	STUBCDC_EP_NOTIFY
#define OUT		0u
#define IN		1u
#define WAIT_MS		1000u	// a host that stops reading for this long is gone
#define ISTAT_ALL	0xFFu

typedef struct {
	volatile uint32_t ctl;		// USB_BD_* and the byte count
	volatile uint32_t addr;
} bd_t;

// The whole table (16 endpoints, 2 directions, even and odd), 512-byte aligned as BDTPAGE1 requires
static bd_t bdt[64] __attribute__((aligned(512)));

static uint8_t ep0_out_buf[2][EP0_SIZE];
static uint8_t out1_buf[2][DATA_SIZE];
static uint8_t in1_buf[2][DATA_SIZE];

static uint8_t next_odd[3][2];		// the half the controller uses next, per endpoint and direction
static uint8_t setup[8];

// Endpoint 0: the IN data stage in progress, the address to set after the status stage
static const uint8_t *ep0_tx;
static uint32_t ep0_tx_left;
static bool ep0_in_busy, ep0_in_data1, ep0_status_in;
static int ep0_in_half = -1;		// the armed IN descriptor's half, -1 none
static bool addr_pending;
static uint8_t addr_new;

// Bulk and interrupt endpoints
static bool out1_armed[2], in1_busy[2];
static uint8_t out1_arm_half, in1_arm_half;	// the half to arm next (they follow the controller's turn)
static bool out1_data1, in1_data1, in2_data1;	// the next toggle
static bool out1_check = true;			// check the OUT toggle (DTS); off while it isn't known

static bd_t *bd(unsigned ep, unsigned dir, unsigned odd)
{
	return &bdt[k22f_usb_bdt_index((uint8_t)ep, (uint8_t)dir, (uint8_t)odd)];
}

static void arm(bd_t *d, void *buf, uint32_t n, bool data1, bool dts)
{
	d->addr = (uint32_t)(uintptr_t)buf;
	__asm volatile("dsb" ::: "memory");
	d->ctl = k22f_usb_bd_word(n, data1, dts);	// the OWN bit goes last
}

static void table_page(void)
{
	uint32_t a = (uint32_t)(uintptr_t)bdt;
	USB0_BDTPAGE1 = (uint8_t)(a >> 8);
	USB0_BDTPAGE2 = (uint8_t)(a >> 16);
	USB0_BDTPAGE3 = (uint8_t)(a >> 24);
}

// Every descriptor back to the CPU and every endpoint direction to its even half
static void table_reset(void)
{
	USB0_CTL |= USB_CTL_ODDRST_MASK;
	memset(bdt, 0, sizeof(bdt));
	memset(next_odd, 0, sizeof(next_odd));
	USB0_CTL &= (uint8_t)~USB_CTL_ODDRST_MASK;
	ep0_in_busy = false;
	ep0_in_half = -1;
	out1_armed[0] = out1_armed[1] = in1_busy[0] = in1_busy[1] = false;
	out1_arm_half = in1_arm_half = 0;
}

// ---- endpoint 0

static void ep0_arm_out(void)
{
	unsigned h = next_odd[0][OUT];
	// No toggle check: a SETUP is always DATA0, and the one control write here (SET_LINE_CODING) is a single packet
	arm(bd(0, OUT, h), ep0_out_buf[h], EP0_SIZE, false, false);
}

static void ep0_in_packet(void)
{
	uint32_t n = ep0_tx_left < EP0_SIZE ? ep0_tx_left : EP0_SIZE;
	unsigned h = next_odd[0][IN];
	arm(bd(0, IN, h), (void *)(uintptr_t)ep0_tx, n, ep0_in_data1, true);
	ep0_in_half = (int)h;
	ep0_in_busy = true;
	ep0_tx += n;
	ep0_tx_left -= n;
	ep0_in_data1 = !ep0_in_data1;
}

void cdc_hw_ep0_send(const uint8_t *data, uint32_t len)
{
	ep0_tx = data;
	ep0_tx_left = len;
	ep0_status_in = len == 0;
	ep0_in_data1 = true;		// the data or status stage starts at DATA1
	ep0_in_packet();
}

void cdc_hw_ep0_stall(void)
{
	USB0_ENDPT(0) |= USB_ENDPT_EPSTALL_MASK;	// cleared when the STALL has gone (usb_poll)
}

void cdc_hw_set_address(uint8_t addr)
{
	addr_new = addr;		// after the status stage, at the address the host still talks to
	addr_pending = true;
}

// ---- endpoint 1 and 2

static void out1_arm(void)
{
	while (!out1_armed[out1_arm_half] &&
	       stubcdc_rx_room() >= DATA_SIZE * (1u + (out1_armed[out1_arm_half ^ 1u] ? 1u : 0u))) {
		unsigned h = out1_arm_half;
		arm(bd(EP_DATA, OUT, h), out1_buf[h], DATA_SIZE, out1_data1, out1_check);
		out1_armed[h] = true;
		out1_data1 = !out1_data1;
		out1_arm_half ^= 1u;
	}
}

// Take every descriptor of a direction back (nothing in flight is wanted any more) and restart its toggle at DATA0
static void ep_restart(unsigned ep, unsigned dir)
{
	bd(ep, dir, 0)->ctl = 0;
	bd(ep, dir, 1)->ctl = 0;
	if (ep == EP_DATA && dir == OUT) {
		out1_armed[0] = out1_armed[1] = false;
		out1_arm_half = next_odd[ep][dir];
		out1_data1 = false;
		out1_check = true;
	} else if (ep == EP_DATA) {
		in1_busy[0] = in1_busy[1] = false;
		in1_arm_half = next_odd[ep][dir];
		in1_data1 = false;
	} else {
		in2_data1 = false;
	}
}

void cdc_hw_endpoints(bool on)
{
	USB0_ENDPT(EP_DATA) = on ? (USB_ENDPT_EPHSHK_MASK | USB_ENDPT_EPTXEN_MASK | USB_ENDPT_EPRXEN_MASK |
				    USB_ENDPT_EPCTLDIS_MASK) : 0u;
	USB0_ENDPT(EP_NOTIFY) = on ? (USB_ENDPT_EPHSHK_MASK | USB_ENDPT_EPTXEN_MASK | USB_ENDPT_EPCTLDIS_MASK) : 0u;
	ep_restart(EP_DATA, OUT);
	ep_restart(EP_DATA, IN);
	ep_restart(EP_NOTIFY, IN);
	stubcdc_configured = on;
}

void cdc_hw_halt(uint8_t ep_addr, bool on)
{
	unsigned ep = ep_addr & 0x0Fu;
	if (ep == 0 || ep > EP_NOTIFY || (ep == EP_NOTIFY && !(ep_addr & 0x80u))) {
		return;
	}
	// The endpoint register's stall covers both directions; a clear restarts this direction at DATA0
	if (on) {
		USB0_ENDPT(ep) |= USB_ENDPT_EPSTALL_MASK;
	} else {
		USB0_ENDPT(ep) &= (uint8_t)~USB_ENDPT_EPSTALL_MASK;
		ep_restart(ep, (ep_addr & 0x80u) ? IN : OUT);
	}
}

// ---- the controller's events

static void bus_reset(void)
{
	USB0_ADDR = 0;
	addr_pending = false;
	table_reset();
	USB0_ENDPT(0) = USB_ENDPT_EPHSHK_MASK | USB_ENDPT_EPRXEN_MASK | USB_ENDPT_EPTXEN_MASK;
	for (unsigned ep = 1; ep < 16u; ep++) {
		USB0_ENDPT(ep) = 0;
	}
	stubcdc_configured = false;
	in1_data1 = in2_data1 = out1_data1 = false;
	out1_check = true;
	USB0_USBCTRL &= (uint8_t)~USB_USBCTRL_SUSP_MASK;
	ep0_arm_out();
}

static void ep0_token(unsigned dir, uint32_t ctl, unsigned half)
{
	if (dir == IN) {
		ep0_in_busy = false;
		ep0_in_half = -1;
		if (ep0_tx_left > 0) {
			ep0_in_packet();		// the next packet of the data stage
		} else if (ep0_status_in && addr_pending) {
			USB0_ADDR = addr_new;		// SET_ADDRESS's status stage is done
			addr_pending = false;
		}
		return;
	}
	uint32_t n = k22f_usb_bd_bc(ctl);
	if (k22f_usb_bd_pid(ctl) == USB_TOKEN_PID_SETUP) {
		memcpy(setup, ep0_out_buf[half], sizeof(setup));
		// A new SETUP ends whatever control transfer was going: an IN descriptor still armed is taken back (the
		// controller holds tokens while TXSUSPENDTOKENBUSY is set, so nothing is using it)
		if (ep0_in_half >= 0) {
			bd(0, IN, (unsigned)ep0_in_half)->ctl = 0;
			ep0_in_half = -1;
			ep0_in_busy = false;
		}
		ep0_tx_left = 0;
		addr_pending = false;
		USB0_ENDPT(0) &= (uint8_t)~USB_ENDPT_EPSTALL_MASK;
		ep0_arm_out();
		stubcdc_setup(setup);
		USB0_CTL &= (uint8_t)~USB_CTL_TXSUSPENDTOKENBUSY_MASK;
		return;
	}
	if (n > 0) {
		stubcdc_ep0_data(ep0_out_buf[half], n);
	}
	ep0_arm_out();
	stubcdc_ep0_out_done();		// SET_LINE_CODING's data: the status stage; a control read's status: nothing
}

static void token(void)
{
	uint8_t stat = USB0_STAT;
	USB0_ISTAT = USB_ISTAT_TOKDNE_MASK;	// the next token's status moves up
	unsigned ep = stat >> USB_STAT_ENDP_SHIFT;
	unsigned dir = (stat & USB_STAT_TX_MASK) ? IN : OUT;
	unsigned half = (stat & USB_STAT_ODD_MASK) ? 1u : 0u;
	if (ep > EP_NOTIFY) {
		return;
	}
	uint32_t ctl = bd(ep, dir, half)->ctl;
	next_odd[ep][dir] = (uint8_t)(half ^ 1u);
	if (ep == 0) {
		ep0_token(dir, ctl, half);
	} else if (ep == EP_DATA && dir == OUT) {
		out1_armed[half] = false;
		stubcdc_rx(out1_buf[half], k22f_usb_bd_bc(ctl));
	} else if (ep == EP_DATA) {
		in1_busy[half] = false;
	}
}

void usb_poll(void)
{
	uint8_t is = USB0_ISTAT;
	if (is & USB_ISTAT_USBRST_MASK) {
		USB0_ISTAT = ISTAT_ALL;		// whatever else was pending belonged to the old session
		bus_reset();
		return;
	}
	if (is & USB_ISTAT_ERROR_MASK) {
		USB0_ERRSTAT = 0xFFu;
		USB0_ISTAT = USB_ISTAT_ERROR_MASK;
	}
	if (is & (USB_ISTAT_SLEEP_MASK | USB_ISTAT_RESUME_MASK | USB_ISTAT_SOFTOK_MASK)) {
		// Suspend and resume need nothing here: the stub never sleeps, and the transceiver stays on
		USB0_ISTAT = is & (USB_ISTAT_SLEEP_MASK | USB_ISTAT_RESUME_MASK | USB_ISTAT_SOFTOK_MASK);
	}
	if (is & USB_ISTAT_STALL_MASK) {
		USB0_ISTAT = USB_ISTAT_STALL_MASK;
		USB0_ENDPT(0) &= (uint8_t)~USB_ENDPT_EPSTALL_MASK;	// the control endpoint's STALL went out
	}
	for (unsigned i = 0; i < 8u && (USB0_ISTAT & USB_ISTAT_TOKDNE_MASK); i++) {
		token();
	}
	if (stubcdc_configured) {
		out1_arm();
	}
}

// ---- sending

static bool in1_wait(unsigned half, uint32_t ms)
{
	uint32_t t0 = stub_ms();
	while (in1_busy[half]) {
		usb_poll();
		if (!stubcdc_configured || stub_ms() - t0 > ms) {
			return false;
		}
	}
	return true;
}

static bool in1_packet(const uint8_t *data, uint32_t n)
{
	unsigned h = in1_arm_half;
	if (!in1_wait(h, WAIT_MS)) {
		return false;
	}
	if (n > 0) {
		memcpy(in1_buf[h], data, n);
	}
	arm(bd(EP_DATA, IN, h), in1_buf[h], n, in1_data1, true);
	in1_busy[h] = true;
	in1_data1 = !in1_data1;
	in1_arm_half ^= 1u;
	return true;
}

bool usb_write(const uint8_t *buf, uint32_t n)
{
	if (!stubcdc_configured) {
		return false;
	}
	for (uint32_t off = 0; off < n; off += DATA_SIZE) {
		if (!in1_packet(buf + off, n - off < DATA_SIZE ? n - off : DATA_SIZE)) {
			return false;
		}
	}
	// A transfer that ends on a full packet gets a zero-length one, so a host reading bigger blocks sees its end
	if (n > 0 && n % DATA_SIZE == 0) {
		return in1_packet(NULL, 0);
	}
	return true;
}

void usb_flush(uint32_t ms)
{
	(void)in1_wait(0, ms);
	(void)in1_wait(1, ms);
}

// ---- start

// Take over RadioFoundry's configured device. RadioFoundry parked the controller on a table of descriptors the CPU owns, so every token
// since was answered NAK: what completed before that is dropped, the table becomes the stub's own
static bool adopt(uint32_t toggles)
{
	if ((USB0_ADDR & USB_ADDR_MASK) == 0 || !(USB0_ENDPT(EP_DATA) & USB_ENDPT_EPTXEN_MASK) ||
	    !(USB0_ENDPT(EP_DATA) & USB_ENDPT_EPRXEN_MASK)) {
		return false;
	}
	for (unsigned i = 0; i < 4u && (USB0_ISTAT & USB_ISTAT_TOKDNE_MASK); i++) {
		(void)USB0_STAT;
		USB0_ISTAT = USB_ISTAT_TOKDNE_MASK;
	}
	table_reset();
	table_page();
	USB0_ISTAT = (uint8_t)(ISTAT_ALL & ~USB_ISTAT_USBRST_MASK);	// a bus reset since is still served
	USB0_ENDPT(0) &= (uint8_t)~USB_ENDPT_EPSTALL_MASK;
	USB0_ENDPT(EP_DATA) &= (uint8_t)~USB_ENDPT_EPSTALL_MASK;
	USB0_ENDPT(EP_NOTIFY) &= (uint8_t)~USB_ENDPT_EPSTALL_MASK;
	USB0_USBCTRL &= (uint8_t)~USB_USBCTRL_SUSP_MASK;
	bool known = (toggles & K22F_STUB_TOGGLE_VALID) != 0;
	in1_data1 = known && (toggles & K22F_STUB_TOGGLE_IN1);
	in2_data1 = known && (toggles & K22F_STUB_TOGGLE_IN2);
	out1_data1 = known && (toggles & K22F_STUB_TOGGLE_OUT1);
	out1_check = known;
	stubcdc_configured = true;
	ep0_arm_out();
	USB0_CTL &= (uint8_t)~USB_CTL_TXSUSPENDTOKENBUSY_MASK;
	return true;
}

// Start over: the host sees an unplug, then a new device with the same serial number. The clock set-up is RadioFoundry's
// (platform/k22f/usb.c), repeated so this path works whatever RadioFoundry left
static void cold_start(void)
{
	SIM_SCGC4 |= SIM_SCGC4_USBOTG;
	SIM_SOPT1CFG |= SIM_SOPT1CFG_URWE;
	SIM_SOPT1 |= SIM_SOPT1_USBREGEN;
	USB0_CLK_RECOVER_IRC_EN = USB_CLK_RECOVER_IRC_EN_REG | USB_CLK_RECOVER_IRC_EN_IRC;
	SIM_CLKDIV2 &= ~SIM_CLKDIV2_USB_MASK;
	SIM_SOPT2 = (SIM_SOPT2 & ~SIM_SOPT2_PLLFLLSEL_MASK) | SIM_SOPT2_PLLFLLSEL(3u) | SIM_SOPT2_USBSRC;

	USB0_CONTROL &= (uint8_t)~USB_CONTROL_DPPULLUPNONOTG;	// the host sees an unplug
	USB0_CTL = 0;
	uint8_t irc = USB0_CLK_RECOVER_IRC_EN;
	USB0_USBTRC0 |= USB_USBTRC0_USBRESET_MASK;
	uint32_t t0 = stub_ms();
	while ((USB0_USBTRC0 & USB_USBTRC0_USBRESET_MASK) && stub_ms() - t0 < 10u) {
	}
	USB0_CLK_RECOVER_IRC_EN = irc;			// the module reset clears the clock recovery set-up
	USB0_CLK_RECOVER_CTRL |= USB_CLK_RECOVER_EN;
	USB0_USBTRC0 |= USB_USBTRC0_SET_ONE_MASK;
	table_reset();
	table_page();
	USB0_ERRSTAT = 0xFFu;
	USB0_ISTAT = ISTAT_ALL;
	// The sources usb_poll reads; the interrupt line itself stays off in the NVIC
	USB0_INTEN = USB_INTEN_USBRSTEN_MASK | USB_INTEN_TOKDNEEN_MASK | USB_INTEN_STALLEN_MASK | USB_INTEN_ERROREN_MASK |
		     USB_INTEN_SLEEPEN_MASK;
	USB0_ADDR = 0;
	for (unsigned ep = 0; ep < 16u; ep++) {
		USB0_ENDPT(ep) = 0;
	}
	stubcdc_configured = false;
	stub_delay_ms(100);			// long enough that every host notices the unplug
	USB0_USBCTRL = 0;
	USB0_CONTROL |= USB_CONTROL_DPPULLUPNONOTG;
	USB0_CTL = USB_CTL_USBENSOFEN_MASK;	// the host's bus reset (usb_poll) arms endpoint 0
}

bool usb_start(bool want_adopt, uint32_t toggles, const char *serial)
{
	stubcdc_init(serial);
	if (want_adopt && adopt(toggles)) {
		return true;
	}
	cold_start();
	return false;
}
