// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
/*
 * Minimal IPv4 gateway between a USB network link and DMR packet data
 *
 * Ethernet, ARP (on a /31 for the radio's address, on a shared link for every address but the host's), ICMP echo to the
 * radio's address,
 * a DHCP server with a single lease, ICMP, UDP and SCTP between the host and DMR IDs / talkgroups (addressing in
 * ipGateway.h), raw DMR data on IPGW_RAW_PORT, and the radio's own services (core/net): TCP to the radio's address
 * (ipGatewaySetTCP) and a port table for its UDP services (ipGatewayUdpBind; SNMP also from the air,
 * ipGatewayUdpFromAir), all sending to the link through ipGatewaySendLocal. No other protocols, no fragments: a host
 * packet too long for one transmission is dropped, with an ICMP "fragmentation needed" back when it has DF set.
 */

#include <string.h>
#include "dmr/ipGateway.h"
#include "dmr/dmrData.h"

#define ETH_HEADER			14
#define ETHERTYPE_IPV4		0x0800
#define ETHERTYPE_ARP		0x0806
#define IP_PROTO_ICMP		1
#define IP_PROTO_TCP		6
#define IP_PROTO_UDP		17
#define IP_PROTO_SCTP		132
#define DHCP_SERVER_PORT	67
#define DHCP_CLIENT_PORT	68
#define DHCP_MIN_LENGTH		300				// BOOTP minimum, some clients drop shorter replies
#define DHCP_LEASE_SECONDS	600				// short, so the host soon moves if the radio's DMR ID is changed
#define DMR_APP_PORT_FIRST	4000			// broadcasts go over the air only to these ports, not the host's own
#define DMR_APP_PORT_LAST	4099			// chatter (NetBIOS, discovery, LAN sync ...)
#define DHCP_OPTION_MTU		26				// interface MTU (RFC 2132 5.1)
#define ICMP_UNREACHABLE	3
#define ICMP_FRAG_NEEDED	4				// with ICMP_UNREACHABLE, the next-hop MTU in octets 6-7 (RFC 1191)
#define IP_DF				0x4000
#define ICMP_QUOTED_L4		8				// the original datagram's first 64 bits after its header (RFC 792)
_Static_assert(IPGW_AIR_MTU <= DMR_DATA_IP_MTU, "the data service takes every packet of the gateway's air MTU");
_Static_assert(DMR_DATA_IP_MTU >= IPGW_LINK_MTU, "the data service takes every packet the link holds (the MTU we advertise)");

// An ID in a range and back. IDs too big for the range are truncated to its low bits, so in a /16 radio 0x010203 is
// x.y.2.3, the same address as radio 0x0203
#define ADDRESS(range, id)	(IPGW_##range##_NET | ((id) & ~IPGW_MASK(IPGW_##range##_PREFIX)))
#define IN_RANGE(range, ip)	(((ip) & IPGW_MASK(IPGW_##range##_PREFIX)) == IPGW_##range##_NET)
#define ID(range, ip)		((ip) & ~IPGW_MASK(IPGW_##range##_PREFIX))

static uint8_t gwMac[6];
static uint8_t hostMac[6];
static uint8_t monitorMac[6];				// a unicast address the host doesn't have, for traffic between other radios
DMR_DATA_BUFFER static uint8_t tx[IPGW_MAX_FRAME];
static uint16_t ipId = 0;
static uint16_t serialPeerPort = 0;		// host port of the serial protocol, 0 until the host has sent to IPGW_SERIAL_PORT
static uint32_t fixedLink = 0;			// the radio's address on a fixed link, 0 = addressed by DMR ID
static volatile uint32_t offNetId = 0;	// the DMR ID off-net unicast goes to, 0 = dropped (set from the radio task)
static ipGatewayLocalFn localTCP;			// the radio's own TCP, NULL without it
static struct {
	uint16_t port;
	bool air;						// also over the air to our own DMR ID
	ipGatewayUdpHandler handler;
} udpPorts[IPGW_UDP_PORTS];				// the radio's own UDP services
static uint8_t txTtl = 64;				// IP TTL of the next sendIPv4 (the radio's own packets: 255)
static ipGatewayStats_t stats;
static bool deliverLocal(uint8_t protocol, uint32_t src, uint32_t dst, const uint8_t *l4, int length);

static const uint8_t BROADCAST_MAC[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static uint16_t get16(const uint8_t *p)
{
	return (p[0] << 8) | p[1];
}

static uint32_t get32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

static void put16(uint8_t *p, uint16_t v)
{
	p[0] = v >> 8;
	p[1] = v & 0xFF;
}

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = v >> 24;
	p[1] = v >> 16;
	p[2] = v >> 8;
	p[3] = v & 0xFF;
}

// Ones complement checksum, sum is the partial sum of a pseudo header
static uint16_t checksum(const uint8_t *p, int length, uint32_t sum)
{
	for (; length > 1; p += 2, length -= 2)
	{
		sum += get16(p);
	}
	if (length)
	{
		sum += p[0] << 8;
	}

	while (sum >> 16)
	{
		sum = (sum & 0xFFFF) + (sum >> 16);
	}

	return ~sum;
}

static uint32_t pseudoHeaderSum(uint32_t src, uint32_t dst, int udpLength)
{
	return (src >> 16) + (src & 0xFFFF) + (dst >> 16) + (dst & 0xFFFF) + IP_PROTO_UDP + udpLength;
}

void ipGatewayInit(const uint8_t gatewayMac[6], const uint8_t defaultHostMac[6])
{
	memcpy(gwMac, gatewayMac, 6);
	memcpy(hostMac, defaultHostMac, 6);
	memcpy(monitorMac, gatewayMac, 6);
	monitorMac[0] = 0x0A;// locally administered, unicast
}

bool ipGatewaySetFixedLink(uint32_t radioIp)
{
	uint8_t last = radioIp & 0xFF;
	bool usable = (radioIp != 0) && (last >= 1) && (last <= 253) && ((radioIp >> 28) < 0xE) && ((radioIp >> 24) != 127) &&
			!IN_RANGE(INDIVIDUAL, radioIp) && !IN_RANGE(MULTICAST, radioIp)
#if defined(IPGW_GROUP_NET)
			&& !IN_RANGE(GROUP, radioIp)
#endif
			;

	fixedLink = usable ? radioIp : 0;
	return usable || (radioIp == 0);
}

uint32_t ipGatewayFixedLink(void)
{
	return fixedLink;
}

void ipGatewaySetOffNetId(uint32_t id)
{
	offNetId = id & 0xFFFFFFu;
}

uint32_t ipGatewayOffNetId(void)
{
	return offNetId;
}

// The host's broadcast address: on a fixed link the limited broadcast, as on a /31 or /32
static uint32_t broadcastIP(void)
{
	return (fixedLink != 0) ? 0xFFFFFFFFu : IPGW_BROADCAST_IP;
}

uint32_t ipGatewayHostIP(void)
{
	if (fixedLink != 0)
	{
		return fixedLink + 1;
	}

	uint32_t id = ipGatewayRadioId();

	uint32_t ip = ADDRESS(INDIVIDUAL, id);

	if ((ip == IPGW_INDIVIDUAL_NET) || (ip >= IPGW_SHARED_RADIO_IP))
	{
		return 0;// the truncated ID is 0, the all call, or (on a /31 the pair of) the all call / the shared radio address
	}
	return ip;
}

uint32_t ipGatewayRadioIP(void)
{
	if (fixedLink != 0)
	{
		return fixedLink;
	}
#if IPGW_POINT_TO_POINT
	uint32_t hostIp = ipGatewayHostIP();

	return (hostIp != 0) ? (hostIp ^ 1) : 0;
#else
	return IPGW_SHARED_RADIO_IP;
#endif
}

// An individual address that can be sent to over the air: not the all call, and on a shared link not the radio. On a
// /31 the radio's address is also radio (host's ID xor 1), for everything but what the radio itself answers
static bool isRadioAddress(uint32_t ip)
{
	return IN_RANGE(INDIVIDUAL, ip) && (ip != IPGW_ALL_CALL_IP) && (IPGW_POINT_TO_POINT || (ip != IPGW_SHARED_RADIO_IP));
}

// A unicast address outside the gateway's ranges and off the link: what the off-net route takes (ipGatewaySetOffNetId),
// and a source from the air that isn't a radio's. Not 0/8, loopback, link local, multicast and above, the radio's or
// the host's own address, nor on a fixed link the link's /24
static bool isOffNet(uint32_t ip)
{
	uint32_t top = ip >> 24;

	return (top != 0) && (top != 127) && (top < 224) && ((ip & 0xFFFF0000u) != 0xA9FE0000u) &&
			!IN_RANGE(INDIVIDUAL, ip) && !IN_RANGE(MULTICAST, ip) &&
#if defined(IPGW_GROUP_NET)
			!IN_RANGE(GROUP, ip) &&
#endif
			((fixedLink == 0) || ((ip & IPGW_FIXED_NETMASK) != (fixedLink & IPGW_FIXED_NETMASK))) &&
			(ip != ipGatewayRadioIP()) && (ip != ipGatewayHostIP());
}

// The IPv4 header goes at tx[14], the payload must already be at tx[34]
static bool sendIPv4(const uint8_t *dstMac, uint8_t protocol, uint32_t src, uint32_t dst, int payloadLength)
{
	uint8_t *ip = &tx[ETH_HEADER];

	memcpy(tx, dstMac, 6);
	memcpy(&tx[6], gwMac, 6);
	put16(&tx[12], ETHERTYPE_IPV4);

	ip[0] = 0x45;
	ip[1] = 0;
	put16(&ip[2], 20 + payloadLength);
	put16(&ip[4], ipId++);
	put16(&ip[6], 0x4000);// Don't fragment
	ip[8] = txTtl;
	ip[9] = protocol;
	put16(&ip[10], 0);
	put32(&ip[12], src);
	put32(&ip[16], dst);
	put16(&ip[10], checksum(ip, 20, 0));

	return ipGatewaySendFrame(tx, ETH_HEADER + 20 + payloadLength);
}

static void handleARP(const uint8_t *frame, const uint8_t *arp)
{
	uint32_t senderIp = get32(&arp[14]);
	uint32_t targetIp = get32(&arp[24]);

	// IPv4 over Ethernet only (RFC 826: hardware type 1, protocol 0x0800, address lengths 6 and 4): the reply copies
	// these fields, and the sender's hardware address is learned
	if ((get16(&arp[0]) != 1) || (get16(&arp[2]) != ETHERTYPE_IPV4) || (arp[4] != 6) || (arp[5] != 4))
	{
		return;
	}
	// Requests only, and not the host's own probes and announcements
	if ((get16(&arp[6]) != 1) || (targetIp == senderIp) || (targetIp == ipGatewayHostIP()) || (senderIp == 0))
	{
		return;
	}
	if (((IPGW_LINK_PREFIX == 31) || (fixedLink != 0)) && (targetIp != ipGatewayRadioIP()))
	{
		return;// everything else is routed through the radio
	}

	memcpy(hostMac, &arp[8], 6);

	uint8_t *reply = &tx[ETH_HEADER];
	memcpy(tx, &arp[8], 6);
	memcpy(&tx[6], gwMac, 6);
	put16(&tx[12], ETHERTYPE_ARP);
	memcpy(reply, arp, 6);// hardware / protocol types and sizes
	put16(&reply[6], 2);// reply
	memcpy(&reply[8], gwMac, 6);
	put32(&reply[14], targetIp);
	memcpy(&reply[18], &arp[8], 10);// requester MAC and IP
	ipGatewaySendFrame(tx, ETH_HEADER + 28);
}

static uint8_t *putOption(uint8_t *o, uint8_t code, uint8_t length, uint32_t value)
{
	*o++ = code;
	*o++ = length;
	if (length == 4)
	{
		put32(o, value);
		o += 4;
	}
	else if (length == 2)
	{
		put16(o, (uint16_t)value);
		o += 2;
	}
	else
	{
		*o++ = value;
	}
	return o;
}

// RFC 3442: prefix length, the significant octets of the network, the router
static uint8_t *putRoute(uint8_t *o, uint32_t network, int prefix, uint32_t router)
{
	*o++ = prefix;
	for (int i = 0; i < ((prefix + 7) / 8); i++)
	{
		*o++ = network >> (24 - (8 * i));
	}
	put32(o, router);
	return o + 4;
}

// Classless static routes (option 121, and 249 for older Windows) through the radio: on a /32 the radio itself on the
// link (router 0.0.0.0) first, on a /31 or /32 the individual range,
// the multicast range, so that sending to and joining a talkgroup picks this link, and the group range if it isn't on
// the link
static uint8_t *putRoutes(uint8_t *o, uint8_t code, uint32_t router)
{
	uint8_t *start;

	*o++ = code;
	start = o++;
	// A fixed link: the radio is on the host's /24, so only the ranges go through it
	if ((IPGW_LINK_PREFIX == 32) && (fixedLink == 0))
	{
		o = putRoute(o, router, 32, 0);
	}
	if (IPGW_POINT_TO_POINT || (fixedLink != 0))
	{
		o = putRoute(o, IPGW_INDIVIDUAL_NET, IPGW_INDIVIDUAL_PREFIX, router);
	}
	o = putRoute(o, IPGW_MULTICAST_NET, IPGW_MULTICAST_PREFIX, router);
#if defined(IPGW_GROUP_NET)
	if ((fixedLink != 0) || (IPGW_LINK_PREFIX > IPGW_GROUP_PREFIX) ||
			((IPGW_GROUP_NET & IPGW_NETMASK) != (IPGW_INDIVIDUAL_NET & IPGW_NETMASK)))
	{
		o = putRoute(o, IPGW_GROUP_NET, IPGW_GROUP_PREFIX, router);// not on the link
	}
#endif
	*start = o - start - 1;
	return o;
}

static void handleDHCP(const uint8_t *bootp, int length)
{
	uint8_t messageType = 0;
	uint32_t requestedIp = 0;
	uint32_t hostIp = ipGatewayHostIP();
	uint32_t radioIp = ipGatewayRadioIP();

	if ((length < 240) || (bootp[0] != 1) || (get32(&bootp[236]) != 0x63825363))
	{
		return;
	}

	for (int i = 240; (i + 1) < length;)
	{
		uint8_t option = bootp[i];

		if (option == 255)
		{
			break;
		}
		if (option == 0)
		{
			i++;
			continue;
		}
		if ((option == 53) && ((i + 2) < length))
		{
			messageType = bootp[i + 2];
		}
		if ((option == 50) && ((i + 5) < length))
		{
			requestedIp = get32(&bootp[i + 2]);
		}
		i += 2 + bootp[i + 1];
	}

	if (((messageType != 1) && (messageType != 3)) || (hostIp == 0))// DISCOVER, REQUEST
	{
		return;
	}

	// A renewal of an address from an earlier DMR ID is refused, so that the host starts again
	if (requestedIp == 0)
	{
		requestedIp = get32(&bootp[12]);// ciaddr
	}
	bool nak = (messageType == 3) && (requestedIp != 0) && (requestedIp != hostIp);

	uint8_t *udp = &tx[ETH_HEADER + 20];
	uint8_t *reply = &udp[8];

	memset(reply, 0, DHCP_MIN_LENGTH);
	reply[0] = 2;// BOOTREPLY
	reply[1] = 1;// Ethernet
	reply[2] = 6;
	memcpy(&reply[4], &bootp[4], 4);// xid
	memcpy(&reply[10], &bootp[10], 2);// flags
	put32(&reply[16], nak ? 0 : hostIp);// yiaddr
	put32(&reply[20], radioIp);// siaddr
	memcpy(&reply[28], &bootp[28], 16);// chaddr
	put32(&reply[236], 0x63825363);

	uint8_t *o = &reply[240];
	o = putOption(o, 53, 1, nak ? 6 : ((messageType == 1) ? 2 : 5));// NAK, OFFER, ACK
	o = putOption(o, 54, 4, radioIp);
	if (!nak)
	{
		o = putOption(o, 51, 4, DHCP_LEASE_SECONDS);
		o = putOption(o, 1, 4, (fixedLink != 0) ? IPGW_FIXED_NETMASK : IPGW_NETMASK);
		o = putOption(o, DHCP_OPTION_MTU, 2, IPGW_LINK_MTU);// the frames the link holds, and one transmission
		o = putRoutes(o, 121, radioIp);
		o = putRoutes(o, 249, radioIp);
	}
	*o++ = 255;

	int replyLength = o - reply;
	if (replyLength < DHCP_MIN_LENGTH)
	{
		replyLength = DHCP_MIN_LENGTH;
	}

	put16(&udp[0], DHCP_SERVER_PORT);
	put16(&udp[2], DHCP_CLIENT_PORT);
	put16(&udp[4], 8 + replyLength);
	put16(&udp[6], 0);
	uint16_t sum = checksum(udp, 8 + replyLength, pseudoHeaderSum(radioIp, 0xFFFFFFFF, 8 + replyLength));
	put16(&udp[6], (sum == 0) ? 0xFFFF : sum);

	memcpy(hostMac, &bootp[28], 6);
	sendIPv4(BROADCAST_MAC, IP_PROTO_UDP, radioIp, 0xFFFFFFFF, 8 + replyLength);
}

static void handleICMP(const uint8_t *frame, const uint8_t *ip, const uint8_t *icmp, int length)
{
	if ((length < 8) || (icmp[0] != 8) || ((ETH_HEADER + 20 + length) > IPGW_MAX_FRAME))// echo request
	{
		return;
	}

	uint8_t *reply = &tx[ETH_HEADER + 20];
	memmove(reply, icmp, length);
	reply[0] = 0;// echo reply
	put16(&reply[2], 0);
	put16(&reply[2], checksum(reply, length, 0));
	sendIPv4(&frame[6], IP_PROTO_ICMP, get32(&ip[16]), get32(&ip[12]), length);
}

void ipGatewayGetStats(ipGatewayStats_t *s)
{
	*s = stats;
}

// Not inlined, nor fitsAir: the gateway is inlined into the host link task's loop, whose frame is on the task's
// deepest path (docs/dev/stack-check.md); their locals stay in their own frames, off it
__attribute__((noinline)) int ipGatewayAirLength(bool group, bool offNet, uint8_t protocol, const uint8_t *l4, int length)
{
	if ((protocol != IP_PROTO_UDP) || (length < 8))
	{
		return 20 + length;
	}
	uint16_t srcPort = get16(&l4[0]);
	uint16_t dstPort = get16(&l4[2]);

	if (dstPort == IPGW_RAW_PORT)
	{
		return length - 8;
	}
	if (CONFIG_DMR_UDP_COMPRESS && !offNet && dmrDataUdpCompress(srcPort, dstPort))	// as encodeUDP decides
	{
		uint8_t h[DMR_UDPC_HEADER_MAX];

		return dmrDataCompressUDPHeader(0, group, srcPort, dstPort, h) + length - 8;
	}
	return 20 + length;
}

// RFC 1122 3.2.2: never about an ICMP error, nor about a packet to a multicast or broadcast address
static bool mayAnswerWithError(const uint8_t *ip, const uint8_t *l4, int l4Length)
{
	uint32_t dst = get32(&ip[16]);

	if ((ip[9] == IP_PROTO_ICMP) && (l4Length > 0) && (l4[0] != 0) && (l4[0] != 8))
	{
		return false;// anything but an echo or its reply: an error, or a query this radio never forwards anyway
	}
	return ((dst >> 28) != 0xE) && (dst != 0xFFFFFFFFu) && (dst != IPGW_BROADCAST_IP) && (dst != IPGW_ALL_CALL_IP) &&
			((fixedLink == 0) || (dst != (fixedLink | ~IPGW_FIXED_NETMASK)));
}

// A host packet for the air (the IPv4 header at frame[14], layer 4 l4Length octets) fits one transmission: true. Else
// it is dropped, and with DF set answered with an ICMP destination unreachable, fragmentation needed, next-hop MTU the
// air's (RFC 792, RFC 1191), from the radio's address, quoting the packet's header and its first 64 bits. Without DF a
// router would fragment it: there are no fragments on the air (dmr-data.md 12.7), and a receiver couldn't put them
// together, so it is just dropped. Four arguments, all in registers, and not inlined: see ipGatewayAirLength
__attribute__((noinline)) static bool fitsAir(const uint8_t *frame, int l4Length, bool group, bool offNet)
{
	const uint8_t *ip = &frame[ETH_HEADER];
	int ihl = (ip[0] & 0x0F) * 4;
	const uint8_t *l4 = &ip[ihl];

	if (ipGatewayAirLength(group, offNet, ip[9], l4, l4Length) <= IPGW_AIR_MTU)
	{
		return true;
	}
	stats.airTooBig++;
	if (!(get16(&ip[6]) & IP_DF) || !mayAnswerWithError(ip, l4, l4Length))
	{
		return false;
	}
	uint8_t *m = &tx[ETH_HEADER + 20];
	int quoted = ihl + ((l4Length < ICMP_QUOTED_L4) ? l4Length : ICMP_QUOTED_L4);

	memmove(&m[8], ip, quoted);
	m[0] = ICMP_UNREACHABLE;
	m[1] = ICMP_FRAG_NEEDED;
	put16(&m[2], 0);
	put16(&m[4], 0);// unused
	put16(&m[6], IPGW_AIR_MTU);
	put16(&m[2], checksum(m, 8 + quoted, 0));
	if (sendIPv4(&frame[6], IP_PROTO_ICMP, ipGatewayRadioIP(), get32(&ip[12]), 8 + quoted))
	{
		stats.fragNeeded++;
	}
	return false;
}

void ipGatewayEthernetIn(const uint8_t *frame, int length)
{
	if (length < ETH_HEADER)
	{
		return;
	}

	uint16_t etherType = get16(&frame[12]);
	const uint8_t *ip = &frame[ETH_HEADER];

	if (etherType == ETHERTYPE_ARP)
	{
		if (length >= (ETH_HEADER + 28))
		{
			handleARP(frame, ip);
		}
		return;
	}

	if ((etherType != ETHERTYPE_IPV4) || (length < (ETH_HEADER + 20)) || ((ip[0] >> 4) != 4))
	{
		return;
	}

	int ihl = (ip[0] & 0x0F) * 4;
	int total = get16(&ip[2]);

	if ((ihl < 20) || (total < ihl) || (total > (length - ETH_HEADER)) || (checksum(ip, ihl, 0) != 0) || (get16(&ip[6]) & 0x3FFF))
	{
		return;// bad header, or a fragment
	}

	uint32_t src = get32(&ip[12]);
	uint32_t dst = get32(&ip[16]);
	const uint8_t *l4 = &ip[ihl];
	int l4Length = total - ihl;

	if ((ip[9] == IP_PROTO_ICMP) && (dst == ipGatewayRadioIP()) && (dst != 0))
	{
		handleICMP(frame, ip, l4, l4Length);
		return;
	}

	uint16_t dstPort = 0;

	if (ip[9] == IP_PROTO_UDP)
	{
		if (l4Length < 8)
		{
			return;
		}

		int udpLength = get16(&l4[4]);
		if ((udpLength < 8) || (udpLength > l4Length))
		{
			return;
		}
		l4Length = udpLength;

		if (get16(&l4[2]) == DHCP_SERVER_PORT)
		{
			handleDHCP(&l4[8], udpLength - 8);
			return;
		}
		dstPort = get16(&l4[2]);
	}
	else if ((ip[9] != IP_PROTO_ICMP) && (ip[9] != IP_PROTO_SCTP) && (ip[9] != IP_PROTO_TCP))
	{
		return;// ICMP, UDP and SCTP only, and TCP to the radio itself
	}

	if (deliverLocal(ip[9], src, dst, l4, l4Length))
	{
		return;
	}

	// Only what the host itself sends, never anything a host that forwards packets sends back
	if ((src == 0) || (src != ipGatewayHostIP()))
	{
		return;
	}

	if ((ip[9] == IP_PROTO_UDP) && (dst == ipGatewayRadioIP()) && (dstPort == IPGW_SERIAL_PORT))
	{
		serialPeerPort = get16(&l4[0]);
		ipGatewaySerialIn(&l4[8], l4Length - 8);
		return;
	}

	bool group = true;
	bool offNet = false;
	uint32_t id;
	uint8_t ttl = ip[8];// the host's, kept: the air is one link of its path

	if (ttl == 0)
	{
		return;// never valid on the wire (RFC 1122 3.2.1.7); 0 would also read as the radio's own TTL on the air
	}

	if ((dst == IPGW_ALL_CALL_IP) || (dst == IPGW_BROADCAST_IP) || (dst == 0xFFFFFFFF) ||
			((fixedLink != 0) && (dst == (fixedLink | ~IPGW_FIXED_NETMASK))))
	{
		if ((ip[9] != IP_PROTO_UDP) || (((dstPort < DMR_APP_PORT_FIRST) || (dstPort > DMR_APP_PORT_LAST)) && (dstPort != IPGW_RAW_PORT)))
		{
			return;
		}
		id = IPGW_ALL_CALL_ID;
	}
#if defined(IPGW_GROUP_NET)
	else if (IN_RANGE(GROUP, dst))
	{
		id = ID(GROUP, dst);
	}
#endif
	else if (IN_RANGE(MULTICAST, dst))
	{
		id = ID(MULTICAST, dst);
	}
	else if (isRadioAddress(dst))
	{
		group = false;
		id = ID(INDIVIDUAL, dst);
	}
	else if ((offNetId != 0) && isOffNet(dst))
	{
		// Off-net unicast: to the gateway radio's ID, the real address in the full header. The radio routes it, so the
		// TTL goes down by one and a packet at its last hop is dropped (no ICMP time exceeded: nothing is sent back)
		if (ttl <= 1)
		{
			return;
		}
		ttl--;
		group = false;
		offNet = true;
		id = offNetId;
	}
	else
	{
		return;// no route: no off-net ID, or an address of the link or one never routed
	}

	if ((ip[9] == IP_PROTO_UDP) && (dstPort == IPGW_RAW_PORT) && ((l4Length <= 8) || offNet))
	{
		return;// empty, or off-net: raw data has no IP header to carry the off-net address
	}
	if (!fitsAir(frame, l4Length, group, offNet))
	{
		return;
	}
	if ((ip[9] == IP_PROTO_UDP) && (dstPort == IPGW_RAW_PORT))
	{
		ipGatewayRawToAir(group, id, &l4[8], l4Length - 8);
		return;
	}

	// On the air the addresses are the IDs' CAI addresses (12.x.y.z, 225.x.y.z), whatever the gateway's ranges, and
	// only an off-net destination is carried as it is. The host's source becomes ours (IPGW_AIR_SRC_OURS): a fixed link
	// address means nothing on the air, and the answer must come back to this radio's ID
	ipGatewayIPToAir(group, id, IPGW_AIR_SRC_OURS, offNet ? dst : dmrDataIdAddress(group, id), ttl, ip[9], l4, l4Length);
}

// The UDP header and payload at tx[34] (payload may already be in place): the layer 4 length, or -1 when too long
static int buildUDP(uint32_t srcIp, uint32_t dstIp, uint16_t srcPort, uint16_t dstPort, const uint8_t *payload, int length)
{
	uint8_t *udp = &tx[ETH_HEADER + 20];

	if ((length < 0) || ((ETH_HEADER + 20 + 8 + length) > IPGW_MAX_FRAME))
	{
		return -1;
	}

	memmove(&udp[8], payload, length);
	put16(&udp[0], srcPort);
	put16(&udp[2], dstPort);
	put16(&udp[4], 8 + length);
	put16(&udp[6], 0);
	uint16_t sum = checksum(udp, 8 + length, pseudoHeaderSum(srcIp, dstIp, 8 + length));
	put16(&udp[6], (sum == 0) ? 0xFFFF : sum);
	return 8 + length;
}

static bool sendUDP(const uint8_t *dstMac, uint32_t srcIp, uint32_t dstIp, uint16_t srcPort, uint16_t dstPort, const uint8_t *payload, int length)
{
	int l4 = buildUDP(srcIp, dstIp, srcPort, dstPort, payload, length);

	return (l4 > 0) && sendIPv4(dstMac, IP_PROTO_UDP, srcIp, dstIp, l4);
}

// IPv4 multicast MAC: 01:00:5E and the low 23 bits of the address
static void setMulticastMac(uint8_t mac[6], uint32_t ip)
{
	mac[0] = 0x01;
	mac[1] = 0x00;
	mac[2] = 0x5E;
	mac[3] = (ip >> 16) & 0x7F;
	mac[4] = ip >> 8;
	mac[5] = ip;
}

// The destination of a packet whose IP destination isn't the address of its DMR destination (airIp, as on the air):
// our own ID's address is the host's, as for a packet to our ID; a multicast group or the broadcast goes to its MAC;
// another unicast address goes to the host when the packet came to our ID (we are the gateway: the host routes it
// on), else to the monitor MAC (traffic between others). False: never on the link (0/8, loopback)
static bool literalDestination(bool group, uint32_t dst, uint32_t airIp, uint32_t *dstIp, const uint8_t **dstMac,
								uint8_t multicastMac[6])
{
	uint32_t own = ipGatewayRadioId();
	uint32_t top = airIp >> 24;

	*dstIp = airIp;
	*dstMac = monitorMac;
	if ((top == 0) || (top == 127))
	{
		return false;
	}
	if (((airIp == dmrDataIdAddress(false, own)) || (airIp == ADDRESS(INDIVIDUAL, own))) && (ipGatewayHostIP() != 0))
	{
		*dstIp = ipGatewayHostIP();
		*dstMac = hostMac;
	}
	else if ((top >= 224) && (top < 240))
	{
		setMulticastMac(multicastMac, airIp);
		*dstMac = multicastMac;
	}
	else if (airIp == 0xFFFFFFFFu)
	{
		*dstIp = broadcastIP();
		*dstMac = BROADCAST_MAC;
	}
	else if (!group && (dst == own) && (own != 0))
	{
		*dstMac = hostMac;
	}
	return true;
}

// Layer 4 (length bytes) is already in tx after the IPv4 header. srcAir / dstAir: the IPv4 addresses on the air. When
// they are the IDs' CAI addresses, the host sees the gateway's ranges as always; another address is kept as it is (an
// off-net host's, behind a gateway radio). ttl: the air's, kept, so a packet that goes round a loop of gateways runs
// out (0: dropped). A UDP checksum covers the addresses, so it is redone
static bool deliver(bool group, uint32_t dst, uint32_t src, uint32_t srcAir, uint32_t dstAir, uint8_t ttl,
					uint8_t protocol, int length)
{
	uint32_t srcIp = ADDRESS(INDIVIDUAL, src);
	uint32_t dstIp;
	const uint8_t *dstMac;
	uint8_t multicastMac[6];

	if (ttl == 0)
	{
		return true;
	}
	if (!dmrDataIpIsId(srcAir, false, src))
	{
		// Another source than the sender's own address is an off-net host's, routed by the sender. One in the radio
		// range or the CAI radio network would pass for another radio; one of the host, the radio, the link or a group
		// isn't from the air
		srcIp = srcAir;
		if (!isOffNet(srcIp) || ((srcIp & 0xFF000000u) == dmrDataIdAddress(false, 0)))
		{
			return true;
		}
	}
	else if (!isRadioAddress(srcIp))
	{
		return true;// would look like the radio itself or the all call, dropped
	}

	if (!dmrDataIpIsId(dstAir, group, dst))
	{
		if (!literalDestination(group, dst, dstAir, &dstIp, &dstMac, multicastMac))
		{
			return true;
		}
	}
	else if (dst == IPGW_ALL_CALL_ID)
	{
		dstIp = broadcastIP();
		dstMac = BROADCAST_MAC;
	}
	else if (group)
	{
		dstIp = ADDRESS(MULTICAST, dst);
		setMulticastMac(multicastMac, dstIp);
		dstMac = multicastMac;
	}
	else
	{
		dstIp = ADDRESS(INDIVIDUAL, dst);
		if (!isRadioAddress(dstIp))
		{
			return true;
		}
		dstMac = monitorMac;
		if ((dstIp == ADDRESS(INDIVIDUAL, ipGatewayRadioId())) && (ipGatewayHostIP() != 0))
		{
			dstIp = ipGatewayHostIP();// to this radio: the host (its own address, or the fixed link's)
			dstMac = hostMac;
		}
	}

	if (protocol == IP_PROTO_UDP)
	{
		uint8_t *udp = &tx[ETH_HEADER + 20];

		put16(&udp[4], length);
		put16(&udp[6], 0);
		uint16_t sum = checksum(udp, length, pseudoHeaderSum(srcIp, dstIp, length));
		put16(&udp[6], (sum == 0) ? 0xFFFF : sum);
	}

	txTtl = ttl;
	bool ok = sendIPv4(dstMac, protocol, srcIp, dstIp, length);
	txTtl = 64;
	return ok;
}

bool ipGatewayDeliverIP(bool group, uint32_t dst, uint32_t src, uint32_t srcIp, uint32_t dstIp, uint8_t ttl,
						uint8_t protocol, const uint8_t *l4, int length)
{
	if ((protocol != IP_PROTO_ICMP) && (protocol != IP_PROTO_UDP) && (protocol != IP_PROTO_SCTP))
	{
		return true;// not forwarded
	}

	if (length < ((protocol == IP_PROTO_UDP) ? 8 : 1))
	{
		return true;
	}
	if ((ETH_HEADER + 20 + length) > IPGW_MAX_FRAME)
	{
		stats.upTooBig++;
		return true;
	}

	memmove(&tx[ETH_HEADER + 20], l4, length);
	return deliver(group, dst, src, srcIp, dstIp, ttl, protocol, length);
}

bool ipGatewayDeliverUDP(bool group, uint32_t dst, uint32_t src, uint32_t srcIp, uint32_t dstIp, uint8_t ttl,
						 uint16_t srcPort, uint16_t dstPort, const uint8_t *payload, int length)
{
	uint8_t *udp = &tx[ETH_HEADER + 20];

	if (length < 0)
	{
		return true;
	}
	if ((ETH_HEADER + 20 + 8 + length) > IPGW_MAX_FRAME)
	{
		stats.upTooBig++;
		return true;
	}

	memmove(&udp[8], payload, length);
	put16(&udp[0], srcPort);
	put16(&udp[2], dstPort);
	return deliver(group, dst, src, srcIp, dstIp, ttl, IP_PROTO_UDP, 8 + length);
}

bool ipGatewayDeliverRaw(bool group, uint32_t dst, uint32_t src, const uint8_t *data, int length)
{
	return ipGatewayDeliverUDP(group, dst, src, dmrDataIdAddress(false, src), dmrDataIdAddress(group, dst), 64,
								IPGW_RAW_PORT, IPGW_RAW_PORT, data, length);
}

bool ipGatewaySerialOut(const uint8_t *data, int length)
{
	uint32_t hostIp = ipGatewayHostIP();

	if ((serialPeerPort == 0) || (hostIp == 0))
	{
		return true;// nobody to send it to
	}
	return sendUDP(hostMac, ipGatewayRadioIP(), hostIp, IPGW_SERIAL_PORT, serialPeerPort, data, length);
}

bool ipGatewayDeliverMonitor(const uint8_t *record, int length)
{
	return sendUDP(BROADCAST_MAC, ipGatewayRadioIP(), broadcastIP(), IPGW_MONITOR_PORT, IPGW_MONITOR_PORT, record, length);
}

void ipGatewaySetTCP(ipGatewayLocalFn fn)
{
	localTCP = fn;
}

static bool bindPort(uint16_t port, ipGatewayUdpHandler handler, bool air)
{
	int free = -1;

	for (int i = 0; i < IPGW_UDP_PORTS; i++)
	{
		if ((udpPorts[i].handler != NULL) && (udpPorts[i].port == port))
		{
			udpPorts[i].handler = handler;// rebound, or unbound with NULL
			udpPorts[i].air = air;
			return true;
		}
		if ((udpPorts[i].handler == NULL) && (free < 0))
		{
			free = i;
		}
	}
	if (handler == NULL)
	{
		return true;
	}
	if (free < 0)
	{
		return false;
	}
	udpPorts[free].port = port;
	udpPorts[free].air = air;
	udpPorts[free].handler = handler;
	return true;
}

bool ipGatewayUdpBind(uint16_t port, ipGatewayUdpHandler handler)
{
	return bindPort(port, handler, false);
}

bool ipGatewayUdpBindAir(uint16_t port, ipGatewayUdpHandler handler)
{
	return bindPort(port, handler, true);
}

bool ipGatewayUdpFromAir(uint32_t srcId, uint16_t srcPort, uint16_t dstPort, const uint8_t *data, int length)
{
	for (int i = 0; i < IPGW_UDP_PORTS; i++)
	{
		if ((udpPorts[i].handler != NULL) && udpPorts[i].air && (udpPorts[i].port == dstPort))
		{
			udpPorts[i].handler(true, srcId, srcPort, ADDRESS(INDIVIDUAL, ipGatewayRadioId()), dstPort, data, length);
			return true;
		}
	}
	return false;
}

// The radio's own services, from any source on the link (a host without its lease yet uses another address; replies
// to the radio's own TCP connections come through the host): true when the packet was for one of them
static bool deliverLocal(uint8_t protocol, uint32_t src, uint32_t dst, const uint8_t *l4, int length)
{
	bool radio = (dst == ipGatewayRadioIP()) && (dst != 0);

	if (protocol == IP_PROTO_TCP)
	{
		if (radio && (localTCP != NULL))
		{
			localTCP(src, l4, length);
		}
		return true;// TCP never goes over the air
	}
	if ((protocol != IP_PROTO_UDP) || !(radio || ((dst & 0xFFFFFF00u) == IPGW_LOCAL_MULTICAST)))
	{
		return false;
	}
	uint16_t dstPort = get16(&l4[2]);
	for (int i = 0; i < IPGW_UDP_PORTS; i++)
	{
		if ((udpPorts[i].handler != NULL) && (udpPorts[i].port == dstPort))
		{
			udpPorts[i].handler(false, src, get16(&l4[0]), dst, dstPort, &l4[8], length - 8);
			return true;
		}
	}
	return (dst & 0xFFFFFF00u) == IPGW_LOCAL_MULTICAST;// link local: never over the air, bound or not
}

uint8_t *ipGatewayLocalPayload(void)
{
	return &tx[ETH_HEADER + 20];
}

bool ipGatewaySendLocal(uint8_t protocol, uint32_t dst, int length)
{
	uint32_t radioIp = ipGatewayRadioIP();
	uint8_t multicastMac[6];
	const uint8_t *dstMac = hostMac;// the host is the only neighbor, and the router

	if ((radioIp == 0) || (length < 0) || (length > IPGW_LOCAL_MAX))
	{
		return false;
	}
	if ((dst >> 28) == 0xE)
	{
		setMulticastMac(multicastMac, dst);
		dstMac = multicastMac;
	}
	else if ((dst == 0xFFFFFFFF) || (dst == IPGW_BROADCAST_IP))
	{
		dstMac = BROADCAST_MAC;
	}

	txTtl = 255;// on link (mDNS checks it, RFC 6762 11), and plenty beyond the host
	bool ok = sendIPv4(dstMac, protocol, radioIp, dst, length);
	txTtl = 64;
	return ok;
}

bool ipGatewayUdpSend(uint32_t dstIp, uint16_t srcPort, uint16_t dstPort, const uint8_t *payload, int length)
{
	uint32_t radioIp = ipGatewayRadioIP();

	if ((radioIp == 0) || (length < 0) || (length > IPGW_UDP_PAYLOAD_MAX))
	{
		return false;
	}
	return ipGatewaySendLocal(IP_PROTO_UDP, dstIp, buildUDP(radioIp, dstIp, srcPort, dstPort, payload, length));
}
