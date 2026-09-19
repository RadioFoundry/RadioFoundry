// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
/*
 * Minimal IPv4 gateway between a USB network link and DMR packet data
 *
 * Ethernet, ARP (on a /31 for the radio's address, on a shared link for every address but the host's), ICMP echo to the
 * radio's address,
 * a DHCP server with a single lease, ICMP, UDP and SCTP between the host and DMR IDs / talkgroups (addressing in
 * ipGateway.h), and raw DMR data on IPGW_RAW_PORT. TCP only to the radio itself, for its own services (ipGatewaySetTCP);
 * no other protocols, no fragments.
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
static ipGatewayLocalFn localTCP;			// the radio's own TCP, NULL without it

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

uint32_t ipGatewayHostIP(void)
{
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
	ip[8] = 64;
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

	// Requests only, and not the host's own probes and announcements
	if ((get16(&arp[6]) != 1) || (targetIp == senderIp) || (targetIp == ipGatewayHostIP()) || (senderIp == 0))
	{
		return;
	}
#if (IPGW_LINK_PREFIX == 31)
	if (targetIp != ipGatewayRadioIP())
	{
		return;// everything else is routed through the radio
	}
#endif

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
#if (IPGW_LINK_PREFIX == 32)
	o = putRoute(o, router, 32, 0);
#endif
#if IPGW_POINT_TO_POINT
	o = putRoute(o, IPGW_INDIVIDUAL_NET, IPGW_INDIVIDUAL_PREFIX, router);
#endif
	o = putRoute(o, IPGW_MULTICAST_NET, IPGW_MULTICAST_PREFIX, router);
#if defined(IPGW_GROUP_NET)
	if ((IPGW_LINK_PREFIX > IPGW_GROUP_PREFIX) || ((IPGW_GROUP_NET & IPGW_NETMASK) != (IPGW_INDIVIDUAL_NET & IPGW_NETMASK)))
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
		o = putOption(o, 1, 4, IPGW_NETMASK);
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

	if (ip[9] == IP_PROTO_TCP)
	{
		if ((localTCP != NULL) && (dst == ipGatewayRadioIP()) && (dst != 0))
		{
			localTCP(src, l4, l4Length);// any source: replies to the radio's own connections come through the host
		}
		return;// never over the air
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
	else if ((ip[9] != IP_PROTO_ICMP) && (ip[9] != IP_PROTO_SCTP))
	{
		return;// ICMP, UDP and SCTP only
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
	uint32_t id;

	if ((dst == IPGW_ALL_CALL_IP) || (dst == IPGW_BROADCAST_IP) || (dst == 0xFFFFFFFF))
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
	else
	{
		return;
	}

	if ((ip[9] == IP_PROTO_UDP) && (dstPort == IPGW_RAW_PORT))
	{
		if (l4Length > 8)
		{
			ipGatewayRawToAir(group, id, &l4[8], l4Length - 8);
		}
		return;
	}

	ipGatewayIPToAir(group, id, ip[9], l4, l4Length);
}

static bool sendUDP(const uint8_t *dstMac, uint32_t srcIp, uint32_t dstIp, uint16_t srcPort, uint16_t dstPort, const uint8_t *payload, int length)
{
	uint8_t *udp = &tx[ETH_HEADER + 20];

	if ((length < 0) || ((ETH_HEADER + 20 + 8 + length) > IPGW_MAX_FRAME))
	{
		return false;
	}

	memmove(&udp[8], payload, length);
	put16(&udp[0], srcPort);
	put16(&udp[2], dstPort);
	put16(&udp[4], 8 + length);
	put16(&udp[6], 0);
	uint16_t sum = checksum(udp, 8 + length, pseudoHeaderSum(srcIp, dstIp, 8 + length));
	put16(&udp[6], (sum == 0) ? 0xFFFF : sum);

	return sendIPv4(dstMac, IP_PROTO_UDP, srcIp, dstIp, 8 + length);
}

// Layer 4 (length bytes) is already in tx after the IPv4 header. A UDP checksum covers the addresses, so it is redone
static bool deliver(bool group, uint32_t dst, uint32_t src, uint8_t protocol, int length)
{
	uint32_t srcIp = ADDRESS(INDIVIDUAL, src);
	uint32_t dstIp;
	const uint8_t *dstMac;
	uint8_t multicastMac[6];

	if (!isRadioAddress(srcIp))
	{
		return true;// would look like the radio itself or the all call, dropped
	}

	if (dst == IPGW_ALL_CALL_ID)
	{
		dstIp = IPGW_BROADCAST_IP;
		dstMac = BROADCAST_MAC;
	}
	else if (group)
	{
		dstIp = ADDRESS(MULTICAST, dst);
		// IPv4 multicast MAC, 01:00:5E and the low 23 bits of the address
		multicastMac[0] = 0x01;
		multicastMac[1] = 0x00;
		multicastMac[2] = 0x5E;
		multicastMac[3] = (dstIp >> 16) & 0x7F;
		multicastMac[4] = dstIp >> 8;
		multicastMac[5] = dstIp;
		dstMac = multicastMac;
	}
	else
	{
		dstIp = ADDRESS(INDIVIDUAL, dst);
		if (!isRadioAddress(dstIp))
		{
			return true;
		}
		dstMac = (dstIp == ipGatewayHostIP()) ? hostMac : monitorMac;
	}

	if (protocol == IP_PROTO_UDP)
	{
		uint8_t *udp = &tx[ETH_HEADER + 20];

		put16(&udp[4], length);
		put16(&udp[6], 0);
		uint16_t sum = checksum(udp, length, pseudoHeaderSum(srcIp, dstIp, length));
		put16(&udp[6], (sum == 0) ? 0xFFFF : sum);
	}

	return sendIPv4(dstMac, protocol, srcIp, dstIp, length);
}

bool ipGatewayDeliverIP(bool group, uint32_t dst, uint32_t src, uint8_t protocol, const uint8_t *l4, int length)
{
	if ((protocol != IP_PROTO_ICMP) && (protocol != IP_PROTO_UDP) && (protocol != IP_PROTO_SCTP))
	{
		return true;// not forwarded
	}

	if ((length < ((protocol == IP_PROTO_UDP) ? 8 : 1)) || ((ETH_HEADER + 20 + length) > IPGW_MAX_FRAME))
	{
		return true;
	}

	memmove(&tx[ETH_HEADER + 20], l4, length);
	return deliver(group, dst, src, protocol, length);
}

bool ipGatewayDeliverUDP(bool group, uint32_t dst, uint32_t src, uint16_t srcPort, uint16_t dstPort, const uint8_t *payload, int length)
{
	uint8_t *udp = &tx[ETH_HEADER + 20];

	if ((length < 0) || ((ETH_HEADER + 20 + 8 + length) > IPGW_MAX_FRAME))
	{
		return true;
	}

	memmove(&udp[8], payload, length);
	put16(&udp[0], srcPort);
	put16(&udp[2], dstPort);
	return deliver(group, dst, src, IP_PROTO_UDP, 8 + length);
}

bool ipGatewayDeliverRaw(bool group, uint32_t dst, uint32_t src, const uint8_t *data, int length)
{
	return ipGatewayDeliverUDP(group, dst, src, IPGW_RAW_PORT, IPGW_RAW_PORT, data, length);
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
	return sendUDP(BROADCAST_MAC, ipGatewayRadioIP(), IPGW_BROADCAST_IP, IPGW_MONITOR_PORT, IPGW_MONITOR_PORT, record, length);
}

void ipGatewaySetTCP(ipGatewayLocalFn fn)
{
	localTCP = fn;
}

uint8_t *ipGatewayLocalPayload(void)
{
	return &tx[ETH_HEADER + 20];
}

bool ipGatewaySendLocal(uint8_t protocol, uint32_t dst, int length)
{
	uint32_t radioIp = ipGatewayRadioIP();

	if ((radioIp == 0) || (length < 0) || (length > IPGW_LOCAL_MAX))
	{
		return false;
	}
	return sendIPv4(hostMac, protocol, radioIp, dst, length);// the host is the only neighbor, and the router
}
