// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The host link's network personality (see hostlink_net.h). No RTOS calls except a short wait for the USB IN
// endpoint; the host tests run it with a fake USB link (usb_ncm.h).
#include "config.h"
#if CONFIG_DMR_IP_GATEWAY	// the whole file
#include <string.h>
#include "hostlink/hostlink_net.h"
#include "dmr/ipGateway.h"
#include "board.h"
#include "hal_time.h"
#if CONFIG_NET_LOCAL
#include "net/net.h"
#endif

#define PACKETS_UP		2	// packets from the air waiting for the host
#define SERIAL_CHUNK		512	// UDP serial datagrams at most this long (the gateway's frames are 600 bytes)
#define WRITE_WAIT_MS		50	// a serial reply waits this long for the USB IN endpoint


static const hostlink_net_air_t *air;
static hostlink_net_serial_fn serial_fn;
static bool started;
static DMR_DATA_BUFFER dmrDataPacket_t up[PACKETS_UP];
static volatile uint8_t up_in, up_out;		// producer: the radio task; consumer: the host link task
#define FRAME_MAX		IPGW_MAX_FRAME
static DMR_DATA_BUFFER uint8_t frame[FRAME_MAX];
static hostlink_net_stats_t stats;
static volatile bool passthrough;		// control station: the PC is the endpoint (the setting)
static volatile uint32_t link_wanted;		// the fixed link asked for, applied by the host link task
static volatile bool link_changed;

static bool net_up(void)
{
	return (board.netif != NULL) && board.netif->up();
}

void hostlink_net_set_air(const hostlink_net_air_t *a)
{
	air = a;
}

void hostlink_net_init(hostlink_net_serial_fn serial_in)
{
	uint8_t host_mac[6], radio_mac[6];

	if (board.netif == NULL) {
		return;
	}
	serial_fn = serial_in;
	board.netif->macs(host_mac, radio_mac);
	ipGatewayInit(radio_mac, host_mac);
#if CONFIG_NET_LOCAL
	net_start(((uint32_t)radio_mac[2] << 24) | ((uint32_t)radio_mac[3] << 16) | ((uint32_t)radio_mac[4] << 8) |
		  radio_mac[5]);
#endif
	started = true;
}

// UDP over the air to our own DMR ID, for a local service of the gateway (ipGatewayUdpBindAir): taken here, in the
// radio task, whether or not the network link is up; true when a service took it. Only with our ID's IP address too:
// a packet to our ID for another address is for a host we route to (the off-net route), never the radio's services
static bool local_udp(const dmrDataPacket_t *p)
{
	uint32_t own = ipGatewayRadioId();
	dmrDataIP_t ip;
	dmrDataUDP_t udp;

	if (p->group || (own == 0) || ((p->dst & 0xFFFFFFu) != own)) {
		return false;
	}
	if ((p->sap == DMR_SAP_IP) && dmrDataGetIP(p, &ip) && (ip.protocol == 17) && (ip.length >= 8) &&
	    dmrDataIpIsId(ip.dstIp, own)) {
		const uint8_t *u = ip.payload;
		int n = (u[4] << 8) | u[5];

		if ((n < 8) || (n > ip.length)) {
			return false;
		}
		return ipGatewayUdpFromAir(p->src, (uint16_t)((u[0] << 8) | u[1]), (uint16_t)((u[2] << 8) | u[3]),
					 u + 8, n - 8);
	}
	if ((p->sap == DMR_SAP_UDPIP_COMPRESSION) && dmrDataGetUDP(p, &udp)) {	// the addresses are the IDs'
		return ipGatewayUdpFromAir(p->src, udp.srcPort, udp.dstPort, udp.payload, udp.length);
	}
	return false;
}

void hostlink_net_packet(const dmrDataPacket_t *p)
{
	if (local_udp(p)) {
		return;
	}
	uint8_t next = (uint8_t)((up_in + 1) % PACKETS_UP);

	stats.packets_air++;
	if (!started || !net_up() || (next == up_out) || (p->length > DMR_DATA_MAX_PACKET)) {
		stats.packets_dropped++;
		return;
	}
	up[up_in] = *p;
	__sync_synchronize();
	up_in = next;
}

// One packet to the host: IP (SAP 4) with its layer 4, compressed UDP/IP (SAP 3) as UDP, anything else as raw data
// on IPGW_RAW_PORT. False while the USB IN endpoint is busy (tried again next poll)
static bool deliver(const dmrDataPacket_t *q)
{
	dmrDataIP_t ip;
	dmrDataUDP_t udp;

	if (q->dpf == DMR_DPF_RESPONSE) {
		return true;	// a confirmed data response: the data service's (dmr-data.md 12), not the host's
	}
	if ((q->sap == DMR_SAP_IP) && dmrDataGetIP(q, &ip)) {
		return ipGatewayDeliverIP(q->group, q->dst, q->src, ip.srcIp, ip.dstIp, ip.protocol, ip.payload, ip.length);
	}
	if ((q->sap == DMR_SAP_UDPIP_COMPRESSION) && dmrDataGetUDP(q, &udp)) {
		return ipGatewayDeliverUDP(q->group, q->dst, q->src, udp.srcIp, udp.dstIp, udp.srcPort, udp.dstPort,
					   udp.payload, udp.length);
	}
	if ((q->sap == DMR_SAP_IP) || (q->sap == DMR_SAP_UDPIP_COMPRESSION)) {
		return true;	// a fragment, or not for the gateway: dropped
	}
	return ipGatewayDeliverRaw(q->group, q->dst, q->src, q->data, q->length);
}

void hostlink_net_set_control(bool pt, uint32_t fixed_link_ip)
{
	passthrough = pt && CONFIG_DMR_CONTROL_STATION;
	link_wanted = CONFIG_DMR_CONTROL_STATION ? fixed_link_ip : 0;
	__sync_synchronize();
	link_changed = true;
}

bool hostlink_net_passthrough_active(void)
{
	return passthrough && started && net_up();
}

void hostlink_net_control_poll(void)
{
	if (link_changed) {
		link_changed = false;
		__sync_synchronize();
		ipGatewaySetFixedLink(link_wanted);	// an address that can't be used leaves the DMR ID addressing
	}
}

void hostlink_net_poll(uint32_t now_ms)
{
	if (!started) {
		return;
	}
	hostlink_net_control_poll();
	board.netif->tick(now_ms);
	for (int i = 0; i < 4; i++) {
		int n = board.netif->receive(frame, sizeof(frame));

		if (n <= 0) {
			break;
		}
		stats.frames_in++;
		ipGatewayEthernetIn(frame, n);
	}
#if CONFIG_NET_LOCAL
	net_poll(now_ms, net_up());	// the radio's own services: timers, output
#endif
	while (up_out != up_in) {
		__sync_synchronize();
		if (!net_up()) {
			up_out = up_in;		// the link went down: what was waiting is dropped
			break;
		}
		if (!deliver(&up[up_out])) {
			break;
		}
		stats.packets_up++;
		up_out = (uint8_t)((up_out + 1) % PACKETS_UP);
	}
}

size_t hostlink_net_write(const uint8_t *buf, size_t len)
{
	size_t done = 0;

	while (done < len) {
		size_t n = ((len - done) > SERIAL_CHUNK) ? SERIAL_CHUNK : (len - done);
		uint32_t t0 = hal_millis();

		while (!ipGatewaySerialOut(buf + done, (int)n)) {
			if (!net_up() || ((hal_millis() - t0) >= WRITE_WAIT_MS)) {
				return done;
			}
			hal_delay_ms(1);
			board.netif->tick(hal_millis());
		}
		done += n;
	}
	return done;
}

#if CONFIG_NET_PPP
uint8_t *hostlink_net_frame(void)
{
	return frame;
}
#endif

void hostlink_net_stats(hostlink_net_stats_t *s)
{
	*s = stats;
}

// ---- What the gateway needs from its user (ipGateway.h) ----

bool ipGatewaySendFrame(const uint8_t *f, int length)
{
	return (board.netif != NULL) && board.netif->send(f, length);
}

uint32_t ipGatewayRadioId(void)
{
	return ((air != NULL) && (air->radio_id != NULL)) ? air->radio_id() : 0;
}

void ipGatewaySerialIn(const uint8_t *data, int length)
{
	if ((serial_fn != NULL) && (length > 0)) {
		serial_fn(data, (size_t)length);
	}
}

bool ipGatewayIPToAir(bool group, uint32_t dst, uint32_t src_ip, uint32_t dst_ip, uint8_t ttl, uint8_t protocol,
		      const uint8_t *l4, int length)
{
	bool ok = (air != NULL) && (air->ip_to_air != NULL) &&
		  air->ip_to_air(group, dst, src_ip, dst_ip, ttl, protocol, l4, length);

	if (ok) {
		stats.to_air++;
	} else {
		stats.to_air_refused++;
	}
	return ok;
}

bool ipGatewayRawToAir(bool group, uint32_t dst, const uint8_t *data, int length)
{
	bool ok = (air != NULL) && (air->raw_to_air != NULL) && air->raw_to_air(group, dst, data, length);

	if (ok) {
		stats.to_air++;
	} else {
		stats.to_air_refused++;
	}
	return ok;
}

#endif	// CONFIG_DMR_IP_GATEWAY
