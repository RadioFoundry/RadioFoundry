// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The host link's network personality (CONFIG_DMR_IP_GATEWAY): the radio as a USB network adapter (CDC-NCM,
// drivers/usb/usb_ncm.h) routing IPv4 over DMR packet data through the IP gateway (core/dmr/ipGateway.h).
//
//   host -> air   Ethernet frames from the host go to the gateway (ARP, DHCP, ping answered on the radio); IP for a
//                 radio ID or talkgroup goes to the air ops below, which the DMR data integration registers
//   air -> host   every packet the data service receives is handed to hostlink_net_packet() (from the radio task);
//                 the host link task delivers it as IP / UDP / raw data (ipGatewayDeliver*)
//   serial        UDP to the radio's own address on IPGW_SERIAL_PORT carries the serial protocols (MMDVM, the debug
//                 link, CPS) in network mode, so the radio stays reachable without its serial port
//
// The air ops are called from the host link task and must not block (post to the radio task and return).
#ifndef ODR_HOSTLINK_NET_H
#define ODR_HOSTLINK_NET_H

#include <stdint.h>
#include <stdbool.h>
#include "config.h"
#if CONFIG_DMR_IP_GATEWAY
#include "dmr/dmrData.h"

typedef struct {
	// The radio's own DMR ID (the codeplug's, not a per channel one: the host's address must stay put); 0 = none
	uint32_t (*radio_id)(void);
	// ICMP, UDP or SCTP from the host for a radio ID or talkgroup, from layer 4 on: queue it as an IP packet (SAP 4,
	// or SAP 3 with the compressed UDP/IPv4 header; confirmed or not: the data service's settings) on the current DMR
	// channel. False if it can't be taken now (one still going, not a DMR channel, receive only)
	bool (*ip_to_air)(bool group, uint32_t dst, uint8_t protocol, const uint8_t *l4, int length);
	// The payload of a datagram to IPGW_RAW_PORT: the user data of an unconfirmed short data packet (SAP 10)
	bool (*raw_to_air)(bool group, uint32_t dst, const uint8_t *data, int length);
} hostlink_net_air_t;

void hostlink_net_set_air(const hostlink_net_air_t *air);

// A packet the data service received (any task; copied): goes to the host when the network link is up. UDP to our
// own DMR ID on a port a local service bound (ipGatewayUdpBindAir) goes to that service instead, link up or not
void hostlink_net_packet(const dmrDataPacket_t *packet);

// The host link task (hostlink.c): start once, then poll every pass while in network mode. bytes_in feeds UDP serial
// bytes to the host link's protocol dispatch
typedef void (*hostlink_net_serial_fn)(const uint8_t *buf, size_t len);
void hostlink_net_init(hostlink_net_serial_fn serial_in);
void hostlink_net_poll(uint32_t now_ms);
// The serial protocols' bytes to the host as UDP datagrams (hostlink_write in network mode)
size_t hostlink_net_write(const uint8_t *buf, size_t len);

// Control station mode (CONFIG_DMR_CONTROL_STATION, docs/spec/usb-network.md 5), any task. passthrough: the PC is
// the endpoint of the radio's own IP data while the link is up; fixed_link_ip: the radio's address on a fixed link
// pair (ipGatewaySetFixedLink), 0 = addressed by DMR ID. The link addressing changes in the host link task's next
// poll; the host picks it up at its next DHCP exchange (the radio refuses a renewal of the old address)
void hostlink_net_set_control(bool passthrough, uint32_t fixed_link_ip);
// In the host link task in either USB personality: puts hostlink_net_set_control's link address in force, so the view
// shows it before the network link is up
void hostlink_net_control_poll(void);
// Passthrough is on and the PC can take the packets now (the network link is up): what the data service is told
bool hostlink_net_passthrough_active(void);

#if CONFIG_NET_PPP
// The adapter's receive buffer (IPGW_MAX_FRAME bytes), lent to PPP: the two never run at once
uint8_t *hostlink_net_frame(void);
#endif

typedef struct {
	uint32_t frames_in, packets_air, packets_up, packets_dropped, to_air, to_air_refused;
} hostlink_net_stats_t;
void hostlink_net_stats(hostlink_net_stats_t *s);
#endif

#endif
