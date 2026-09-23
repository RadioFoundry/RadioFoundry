// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
/*
 * Minimal IPv4 gateway between a USB network link and DMR packet data
 */

#ifndef ODR_IPGATEWAY_H
#define ODR_IPGATEWAY_H

#include <stdbool.h>
#include <stdint.h>

// Addressing, chosen at compile time. Each range is a base address and a prefix length of 8 to 24 bits, and maps the
// low bits of the address to a DMR ID. Bigger IDs are truncated: in a /16 radio 0x010203 is x.y.2.3, and its host also
// gets the data sent to radio 0x0203. Sending from the host to x.y.2.3 reaches 0x0203 only.
//   individual   12.0.0.0/8 (default)  DMR IDs. The host gets its own radio's ID by DHCP, so radio 10005 gives it
//                                      12.0.39.21 (or 10.250.39.21 with 10.250.0.0/16). Over the air the packets keep
//                                      the Motorola CAI addresses (12.x.y.z, 225.x.y.z), see dmrData.c
//   multicast    225.0.0.0/8           talkgroups, both ways: send to 225.x.y.z for talkgroup x.y.z, and group data
//                                      received over the air arrives there (join the group on the radio's interface)
//   link         /32 (default)         the host alone: the host is 12.<ID>/32, the radio is 12.<ID> xor 1, and option
//                                      121 has a route to the radio on the link (router 0.0.0.0) and routes the
//                                      individual, multicast (and group) ranges through the radio. Each radio has an
//                                      address of its own, so several radios can be plugged into one host. The radio
//                                      answers at that address for ping, DHCP, IPGW_SERIAL_PORT and the UDP ports its
//                                      own services bind (ipGatewayUdpBind) only, anything else to it goes over the air
//                                      to the radio with that ID. The radio answers ARP for
//                                      every address but the host's, in case the host ignores the on link route.
//                or /31                the same with the host and the radio as a /31 (RFC 3021), the radio answering
//                                      ARP for its own address only. macOS treats both ends of a /31 as broadcast
//                                      addresses, so sending to the radio needs SO_BROADCAST there.
//                or /1 to the individual prefix, holding the individual range (e.g. IPGW_LINK_PREFIX 8), for hosts
//                                      that can't use a /31: every radio is on the link and the radio itself is the
//                                      top individual address but one (12.255.255.254), so that ID can't be used. One
//                                      radio per host. Option 121 routes the multicast range (and the group range when
//                                      it is off the link).
//   group        none (optional)       a unicast range for sending to talkgroups, for hosts or apps that can't use the
//                                      multicast route, e.g. 10.251.0.0/16 next to 10.250.0.0/16.
//                                      Define IPGW_GROUP_NET and IPGW_GROUP_PREFIX to have it.
// The top individual address (12.255.255.255) is the all call (16777215), for the DMR application ports 4000-4099
// only; as are 255.255.255.255 and, on a shared link, the subnet broadcast. Received all calls and monitor records go
// to the host's broadcast address: the subnet broadcast on a shared link, 255.255.255.255 on a /31 or /32.
#ifndef IPGW_INDIVIDUAL_NET
#define IPGW_INDIVIDUAL_NET		0x0C000000u		// 12.0.0.0
#define IPGW_INDIVIDUAL_PREFIX	8
#endif
#ifndef IPGW_MULTICAST_NET
#define IPGW_MULTICAST_NET		0xE1000000u		// 225.0.0.0
#define IPGW_MULTICAST_PREFIX	8
#endif
#ifndef IPGW_LINK_PREFIX
#define IPGW_LINK_PREFIX		32
#endif

#define IPGW_MASK(prefix)		(0xFFFFFFFFu << (32 - (prefix)))
#define IPGW_NETMASK			IPGW_MASK(IPGW_LINK_PREFIX)
#define IPGW_POINT_TO_POINT		(IPGW_LINK_PREFIX >= 31)
#define IPGW_ALL_CALL_IP		(IPGW_INDIVIDUAL_NET | ~IPGW_MASK(IPGW_INDIVIDUAL_PREFIX))
#define IPGW_SHARED_RADIO_IP	(IPGW_ALL_CALL_IP - 1)	// the radio on a shared link
#if IPGW_POINT_TO_POINT
#define IPGW_BROADCAST_IP		0xFFFFFFFFu
#else
#define IPGW_BROADCAST_IP		((IPGW_INDIVIDUAL_NET & IPGW_NETMASK) | ~IPGW_NETMASK)
#endif
#define IPGW_ALL_CALL_ID		0x00FFFFFFu

_Static_assert((IPGW_INDIVIDUAL_PREFIX >= 8) && (IPGW_INDIVIDUAL_PREFIX <= 24) &&
				(IPGW_MULTICAST_PREFIX >= 8) && (IPGW_MULTICAST_PREFIX <= 24), "address ranges must be /8 to /24");
#if defined(IPGW_GROUP_NET)
_Static_assert((IPGW_GROUP_PREFIX >= 8) && (IPGW_GROUP_PREFIX <= 24), "address ranges must be /8 to /24");
#endif
_Static_assert(((IPGW_LINK_PREFIX <= 32) && IPGW_POINT_TO_POINT) || ((IPGW_LINK_PREFIX >= 1) && (IPGW_LINK_PREFIX <= IPGW_INDIVIDUAL_PREFIX) &&
				((IPGW_SHARED_RADIO_IP & IPGW_NETMASK) == (IPGW_INDIVIDUAL_NET & IPGW_NETMASK))), "the link must be a /31, a /32 or hold the individual range");
_Static_assert((IPGW_MULTICAST_NET >> 28) == 0xE, "the multicast range must be in 224.0.0.0/4");

// Every data burst received over the air also goes to the host, as a UDP broadcast from the radio to this port:
// version (1), timeslot (1-2), colour code, DT, flags (dmrBurst_t), length, payload.
#define IPGW_MONITOR_PORT	40077

// UDP to the radio itself on this port carries the radio's serial protocol, so that MMDVMHost (Modem Protocol=udp,
// ModemAddress = ipGatewayRadioIP(), ModemPort = 3334) and the CPS style 'D' commands also work without a serial port
#define IPGW_SERIAL_PORT	3334

// Raw DMR data, without the IP and UDP headers (28 bytes, about 2½ Rate 1/2 blocks, less air time). A UDP datagram from
// the host to any radio ID's or talkgroup's address on this port goes over the air as just its payload: the user data
// of an unconfirmed Rate 1/2 packet with SAP 10 (short data), up to 440 bytes. Unconfirmed or confirmed packets
// received over the air that aren't IP based (any SAP but 3 and 4) come to the host the same way, as the payload of a
// datagram from the sender's address to the host's (or the talkgroup's multicast address), both ports this one. The all
// call works here too.
#define IPGW_RAW_PORT		40078

#define IPGW_MAX_FRAME		600				// largest Ethernet frame handled, bigger datagrams can't go over the air anyway

// No hardware dependencies, so the gateway can be unit tested on a host (see tests/host)
void ipGatewayInit(const uint8_t gatewayMac[6], const uint8_t hostMac[6]);

// An Ethernet frame from the host
void ipGatewayEthernetIn(const uint8_t *frame, int length);

// The host's address, from the radio's DMR ID (or the fixed link's), 0 if the ID can't be used
uint32_t ipGatewayHostIP(void);

// The radio's own address (the host's router, DHCP server, ping and IPGW_SERIAL_PORT), 0 if the ID can't be used
uint32_t ipGatewayRadioIP(void);

// A fixed link (control station mode, docs/spec/usb-network.md 5.3), chosen at run time: the radio is radioIp, the
// host the next address, both in a /24 (IPGW_FIXED_NETMASK). DHCP gives the host that address with the same option
// 121 routes through the radio (the individual, multicast and group ranges), the radio answers ARP for its own address
// only, and packets over the air to the radio's own DMR ID reach the host at its fixed address (the destination is
// rewritten, as a NAT would). One radio per host. 0 (or an address that can't be used: not unicast, a range of the
// gateway's own, or a last octet outside 1-253) goes back to the addressing by DMR ID; false then if it wasn't 0
#define IPGW_FIXED_LINK_DEFAULT	0xC0A80A01u		// 192.168.10.1, the host 192.168.10.2
#define IPGW_FIXED_NETMASK		0xFFFFFF00u
bool ipGatewaySetFixedLink(uint32_t radioIp);
uint32_t ipGatewayFixedLink(void);				// the radio's fixed address, 0 = addressed by DMR ID

// A UDP datagram received over the air, to the host from the source's individual address. Private data to another radio
// is sent to a MAC address the host doesn't use, so the host ignores it but a packet capture shows it. Group data goes
// to the multicast range. False if the USB IN endpoint is busy.
bool ipGatewayDeliverUDP(bool group, uint32_t dst, uint32_t src, uint16_t srcPort, uint16_t dstPort, const uint8_t *payload, int length);

// The same for an IPv4 packet received over the air, from layer 4 on: ICMP, UDP (its checksum is redone for the new
// addresses) and SCTP; other protocols are dropped (true).
bool ipGatewayDeliverIP(bool group, uint32_t dst, uint32_t src, uint8_t protocol, const uint8_t *l4, int length);

// Raw DMR data received over the air (see IPGW_RAW_PORT), the user data of the packet. False if the USB IN endpoint is busy.
bool ipGatewayDeliverRaw(bool group, uint32_t dst, uint32_t src, const uint8_t *data, int length);

// A monitor record (see IPGW_MONITOR_PORT) to the host
bool ipGatewayDeliverMonitor(const uint8_t *record, int length);

// Bytes from the radio's serial protocol, as one UDP datagram to the host and port that last sent to IPGW_SERIAL_PORT.
// False if the USB IN endpoint is busy; dropped (true) while no host has sent anything yet.
bool ipGatewaySerialOut(const uint8_t *data, int length);

// The radio's own services (core/net: TCP and its services, mDNS, TFTP, ...). Everything for them runs in the caller of
// ipGatewayEthernetIn (the host link task) and never goes over the air.
//
// Receive. TCP: segments to the radio's address, from any source (the host, or through it a server the radio
// connected to), go to the one TCP handler, layer 4 on; NULL (the default) drops them. UDP: a datagram from the link to
// the radio's own address, or to the local network control block 224.0.0.0/24 (mDNS: 224.0.0.251), on a bound port
// goes to that port's handler instead of over the air; 224.0.0.0/24 never goes over the air, bound or not. One small
// table: a service binds its port at start-up (handler NULL unbinds; false if the table is full)
typedef void (*ipGatewayLocalFn)(uint32_t src, const uint8_t *l4, int length);
void ipGatewaySetTCP(ipGatewayLocalFn fn);
#define IPGW_UDP_PORTS			8
#define IPGW_LOCAL_MULTICAST	0xE0000000u		// 224.0.0.0/24, link local: never forwarded (RFC 5771)
typedef void (*ipGatewayUdpHandler)(uint32_t srcIp, uint16_t srcPort, uint32_t dstIp, uint16_t dstPort, const uint8_t *data, int length);
bool ipGatewayUdpBind(uint16_t port, ipGatewayUdpHandler handler);
//
// Send. One buffer and one path for both: build layer 4 (at most IPGW_LOCAL_MAX bytes) at ipGatewayLocalPayload(),
// in the gateway's frame buffer, then ipGatewaySendLocal it with nothing else sent in between. From the radio's own
// address to dst: the host (any unicast address on the link, or beyond it when the host routes), the broadcast
// address or a multicast group (its multicast MAC), with IP TTL 255 (mDNS checks it). False if the USB IN endpoint is
// busy (try again) or the radio has no address yet. UDP: the payload at ipGatewayUdpPayload() (IPGW_UDP_PAYLOAD_MAX
// bytes, after the 8 byte header), or anywhere, then ipGatewayUdpSend adds the header and checksum
#define IPGW_LOCAL_MAX		(IPGW_MAX_FRAME - 14 - 20)
#define IPGW_UDP_PAYLOAD_MAX	(IPGW_LOCAL_MAX - 8)
uint8_t *ipGatewayLocalPayload(void);
bool ipGatewaySendLocal(uint8_t protocol, uint32_t dst, int length);
static inline uint8_t *ipGatewayUdpPayload(void)
{
	return ipGatewayLocalPayload() + 8;
}
bool ipGatewayUdpSend(uint32_t dstIp, uint16_t srcPort, uint16_t dstPort, const uint8_t *payload, int length);

// Provided by the user of the gateway
bool ipGatewaySendFrame(const uint8_t *frame, int length);
uint32_t ipGatewayRadioId(void);
void ipGatewaySerialIn(const uint8_t *data, int length);
// ICMP, UDP or SCTP from the host to a radio ID or talkgroup, from layer 4 on (UDP: header and payload, UDP length bytes)
bool ipGatewayIPToAir(bool group, uint32_t dst, uint8_t protocol, const uint8_t *l4, int length);
// The payload of a datagram from the host to IPGW_RAW_PORT, to go over the air as the user data of a packet
bool ipGatewayRawToAir(bool group, uint32_t dst, const uint8_t *data, int length);

#endif
