// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The host link's PPP personality (see hostlink_ppp.h). No RTOS calls: the host tests run it against scripted peers
// and pppd on a pseudo-terminal.
#include "config.h"
#if CONFIG_NET_PPP	// the whole file
#include <string.h>
#include "hostlink/hostlink_ppp.h"
#include "net/ppp.h"
#include "dmr/ipGateway.h"
#include "hostlink/hostlink_net.h"
#include "board.h"

#define ETH_HEADER	14

_Static_assert(PPP_HEADROOM >= ETH_HEADER, "the Ethernet header is added in front of a packet from PPP");
_Static_assert(PPP_RX_BUFFER <= IPGW_MAX_FRAME, "PPP receives into the adapter's frame buffer (hostlink_net_frame)");

static const hal_netif_t *usb;

void hostlink_ppp_set_usb(const hal_netif_t *u)
{
	usb = u;
}

void hostlink_ppp_begin(bool active, uint32_t now_ms)
{
	ppp_start(active, now_ms ^ ipGatewayRadioId(), now_ms, hostlink_net_frame());
}

// ---- the netif ----

static bool n_select(bool network)
{
	return (usb != NULL) && usb->select(network);
}

static bool n_selected(void)
{
	return ppp_running() || ((usb != NULL) && usb->selected());
}

static bool n_up(void)
{
	return ppp_ip_up() || (!ppp_running() && (usb != NULL) && usb->up());
}

static bool n_send(const uint8_t *f, int length)
{
	if (ppp_running()) {
		if ((length > ETH_HEADER) && (f[12] == 0x08) && (f[13] == 0x00)) {
			ppp_send_ip(f + ETH_HEADER, length - ETH_HEADER);
		}
		return true;	// sent, or dropped: not IPv4, longer than the peer's MRU, IPCP not open
	}
	return (usb != NULL) && usb->send(f, length);
}

static int n_receive(uint8_t *buf, int max)
{
	// PPP hands its packets to the gateway as they arrive (ppp_ip_in)
	return (!ppp_running() && (usb != NULL)) ? usb->receive(buf, max) : 0;
}

static void n_tick(uint32_t now_ms)
{
	if (ppp_running()) {
		ppp_poll(now_ms);
	} else if (usb != NULL) {
		usb->tick(now_ms);
	}
}

static void n_macs(uint8_t host[6], uint8_t radio[6])
{
	// Without the adapter: a locally administered pair, never seen on the wire (the gateway's frames stay inside)
	static const uint8_t mac[6] = { 0x02, 0x4F, 0x44, 0x52, 0x50, 0x00 };

	if (usb != NULL) {
		usb->macs(host, radio);
		return;
	}
	memcpy(host, mac, 6);
	memcpy(radio, mac, 6);
	radio[5] = 1;
}

const hal_netif_t hostlink_ppp_netif = {
	.select = n_select, .selected = n_selected, .up = n_up, .send = n_send, .receive = n_receive, .tick = n_tick,
	.macs = n_macs, .frame_max = IPGW_MAX_FRAME,
};

// ---- what the PPP engine needs (ppp.h); ppp_foreign_start is the host link's (hostlink.c) ----

size_t ppp_write(const uint8_t *buf, size_t len)
{
	return (board.host_port != NULL) ? board.host_port->write(buf, len) : 0;
}

void ppp_ip_in(uint8_t *packet, int length)
{
	uint8_t *f = packet - ETH_HEADER;	// PPP_HEADROOM: ours to write

	memset(f, 0, 12);			// the gateway answers through n_send, which takes the header off again
	f[12] = 0x08;
	f[13] = 0x00;
	ipGatewayEthernetIn(f, length + ETH_HEADER);
}

uint32_t ppp_local_ip(void)
{
	return ipGatewayRadioIP();
}

uint32_t ppp_peer_ip(void)
{
	return ipGatewayHostIP();
}

#endif	// CONFIG_NET_PPP
