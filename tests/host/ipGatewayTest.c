// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
/*
 * Host unit test for the USB network gateway (functions/ipGateway.c)
 *
 * Every frame the gateway sends is also written to ipGatewayTest.pcap, so that an independent dissector
 * (tshark, see the Makefile) can check the headers and checksums.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fuzz.h"
#include "dmr/ipGateway.h"
#include "dmr/dmrData.h"

static int failures = 0;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static const uint8_t GW_MAC[6] = { 0x02, 0x47, 0x44, 0x37, 0x37, 0x01 };
static const uint8_t BCAST_MAC[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
static const uint8_t HOST_MAC[6] = { 0x02, 0x47, 0x44, 0x37, 0x37, 0x02 };

#define RADIO_ID		10005
#define HOST_IP			(IPGW_INDIVIDUAL_NET | RADIO_ID)		// 12.0.39.21 with the default ranges
#define RADIO_IP		ipGatewayRadioIP()

#define FITS_INDIVIDUAL(id)	(((id) & IPGW_MASK(IPGW_INDIVIDUAL_PREFIX)) == 0)

static uint32_t radioId = RADIO_ID;
#ifndef PCAP_NAME
#define PCAP_NAME		"ipGatewayTest.pcap"
#endif

static FILE *pcap;
static uint8_t lastFrame[2048];
static int lastLength;
static int framesSent;

static struct
{
	int calls;
	bool group;
	uint32_t dst;
	uint32_t srcIp, dstIp;
	uint8_t ttl;
	uint8_t protocol;
	uint16_t srcPort;
	uint16_t dstPort;
	uint8_t payload[2048];// UDP: the datagram's payload; ICMP, SCTP: the whole message
	int length;
} air;

static void pcapWrite(const uint8_t *frame, int length)
{
	uint32_t record[4] = { 0, 0, (uint32_t)length, (uint32_t)length };
	fwrite(record, sizeof(record), 1, pcap);
	fwrite(frame, length, 1, pcap);
}

bool ipGatewaySendFrame(const uint8_t *frame, int length)
{
	memcpy(lastFrame, frame, length);
	lastLength = length;
	framesSent++;
	pcapWrite(frame, length);
	return true;
}

bool ipGatewayIPToAir(bool group, uint32_t dst, uint32_t srcIp, uint32_t dstIp, uint8_t ttl, uint8_t protocol, const uint8_t *l4, int length)
{
	air.calls++;
	air.group = group;
	air.dst = dst;
	air.srcIp = srcIp;
	air.dstIp = dstIp;
	air.ttl = ttl;
	air.protocol = protocol;
	if (protocol == 17)
	{
		air.srcPort = (l4[0] << 8) | l4[1];
		air.dstPort = (l4[2] << 8) | l4[3];
		memcpy(air.payload, &l4[8], length - 8);
		air.length = length - 8;
	}
	else
	{
		memcpy(air.payload, l4, length);
		air.length = length;
	}
	return true;
}

static struct
{
	int calls;
	bool group;
	uint32_t dst;
	uint8_t data[2048];
	int length;
} raw;

bool ipGatewayRawToAir(bool group, uint32_t dst, const uint8_t *data, int length)
{
	raw.calls++;
	raw.group = group;
	raw.dst = dst;
	memcpy(raw.data, data, length);
	raw.length = length;
	return true;
}

static struct
{
	int calls;
	uint8_t data[2048];
	int length;
} serial;

void ipGatewaySerialIn(const uint8_t *data, int length)
{
	serial.calls++;
	memcpy(serial.data, data, length);
	serial.length = length;
}

uint32_t ipGatewayRadioId(void)
{
	return radioId;
}

static void in(const uint8_t *frame, int length)
{
	framesSent = 0;
	ipGatewayEthernetIn(frame, length);
}

static uint16_t sum16(const uint8_t *p, int n, uint32_t s)
{
	for (; n > 1; p += 2, n -= 2) s += (p[0] << 8) | p[1];
	if (n) s += p[0] << 8;
	while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
	return ~s;
}

// Builds Ethernet + IPv4 + UDP from the host, with correct checksums
static int hostUdp(uint8_t *f, const uint8_t *dstMac, uint32_t src, uint32_t dst, uint16_t sport, uint16_t dport, const uint8_t *payload, int n)
{
	memcpy(f, dstMac, 6); memcpy(f + 6, HOST_MAC, 6); f[12] = 0x08; f[13] = 0x00;
	uint8_t *ip = f + 14, *u = ip + 20;
	memset(ip, 0, 20);
	ip[0] = 0x45; ip[2] = (28 + n) >> 8; ip[3] = 28 + n; ip[6] = 0x40; ip[8] = 64; ip[9] = 17;
	for (int i = 0; i < 4; i++) { ip[12 + i] = src >> (24 - 8 * i); ip[16 + i] = dst >> (24 - 8 * i); }
	uint16_t c = sum16(ip, 20, 0); ip[10] = c >> 8; ip[11] = c;
	u[0] = sport >> 8; u[1] = sport; u[2] = dport >> 8; u[3] = dport; u[4] = (8 + n) >> 8; u[5] = 8 + n; u[6] = u[7] = 0;
	memcpy(u + 8, payload, n);
	uint32_t ps = (src >> 16) + (src & 0xFFFF) + (dst >> 16) + (dst & 0xFFFF) + 17 + 8 + n;
	c = sum16(u, 8 + n, ps); u[6] = c >> 8; u[7] = c;
	return 14 + 28 + n;
}

static void put32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

// Builds Ethernet + IPv4 from the host around a layer 4 message of any protocol
static int hostIp(uint8_t *f, const uint8_t *dstMac, uint32_t src, uint32_t dst, uint8_t protocol, const uint8_t *l4, int n)
{
	memcpy(f, dstMac, 6); memcpy(f + 6, HOST_MAC, 6); f[12] = 0x08; f[13] = 0x00;
	uint8_t *ip = f + 14;
	memset(ip, 0, 20);
	ip[0] = 0x45; ip[2] = (20 + n) >> 8; ip[3] = 20 + n; ip[6] = 0x40; ip[8] = 64; ip[9] = protocol;
	put32(&ip[12], src);
	put32(&ip[16], dst);
	uint16_t c = sum16(ip, 20, 0); ip[10] = c >> 8; ip[11] = c;
	memcpy(ip + 20, l4, n);
	return 14 + 20 + n;
}

// CRC-32c (RFC 4960 appendix B), stored low byte first
static void sctpChecksum(uint8_t *packet, int n)
{
	uint32_t crc = 0xFFFFFFFF;
	memset(&packet[8], 0, 4);
	for (int i = 0; i < n; i++)
	{
		crc ^= packet[i];
		for (int b = 0; b < 8; b++) crc = (crc & 1) ? ((crc >> 1) ^ 0x82F63B78) : (crc >> 1);
	}
	crc = ~crc;
	packet[8] = crc; packet[9] = crc >> 8; packet[10] = crc >> 16; packet[11] = crc >> 24;
}

static uint32_t get32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

static const uint8_t *dhcpOption(const uint8_t *bootp, int len, uint8_t code)
{
	for (int i = 240; i < len && bootp[i] != 255;)
	{
		if (bootp[i] == 0) { i++; continue; }
		if (bootp[i] == code) return &bootp[i];
		i += 2 + bootp[i + 1];
	}
	return NULL;
}

static void testDHCP(uint8_t type, uint32_t requested, uint8_t expectedReply)
{
	uint8_t bootp[300] = { 1, 1, 6, 0, 0x12, 0x34, 0x56, 0x78 };
	uint8_t f[400];

	bootp[10] = 0x80;// broadcast flag
	memcpy(&bootp[28], HOST_MAC, 6);
	bootp[236] = 0x63; bootp[237] = 0x82; bootp[238] = 0x53; bootp[239] = 0x63;
	bootp[240] = 53; bootp[241] = 1; bootp[242] = type;
	bootp[243] = 50; bootp[244] = 4;
	for (int i = 0; i < 4; i++) bootp[245 + i] = requested >> (24 - 8 * i);
	bootp[249] = 255;

	in(f, hostUdp(f, BCAST_MAC, 0, 0xFFFFFFFF, 68, 67, bootp, sizeof(bootp)));
	CHECK(framesSent == 1);

	const uint8_t *r = &lastFrame[14 + 20 + 8];
	int rlen = lastLength - 42;
	CHECK(rlen >= 300);
	CHECK(r[0] == 2);
	CHECK(get32(&r[4]) == 0x12345678);
	CHECK(memcmp(&r[28], HOST_MAC, 6) == 0);
	const uint8_t *o = dhcpOption(r, rlen, 53);
	CHECK(o && o[2] == expectedReply);
	if (expectedReply == 6)
	{
		CHECK(get32(&r[16]) == 0);
		CHECK(dhcpOption(r, rlen, 1) == NULL);
		return;
	}
	CHECK(get32(&r[16]) == HOST_IP);
	o = dhcpOption(r, rlen, 1);
	CHECK(o && get32(&o[2]) == IPGW_NETMASK);
	CHECK(RADIO_IP == (IPGW_POINT_TO_POINT ? (HOST_IP ^ 1) : (IPGW_ALL_CALL_IP - 1)));
	o = dhcpOption(r, rlen, 54);
	CHECK(o && get32(&o[2]) == RADIO_IP);
	// The multicast range through the radio, on a /31 or /32 also the individual range, on a /32 first the radio itself
	// on the link; the group range is on the link
	o = dhcpOption(r, rlen, 121);
	CHECK(o != NULL);
	int routes = 0;
	bool multicast = false, individual = false, onLink = false;
	for (int i = 2; o && i < 2 + o[1];)
	{
		int prefix = o[i], octets = (prefix + 7) / 8;
		uint32_t net = 0;
		for (int k = 0; k < octets; k++) net |= (uint32_t)o[i + 1 + k] << (24 - 8 * k);
		uint32_t router = get32(&o[i + 1 + octets]);
		if ((routes == 0) && (IPGW_LINK_PREFIX == 32))
		{
			onLink = (prefix == 32) && (net == RADIO_IP) && (router == 0);
		}
		else
		{
			CHECK(router == RADIO_IP);
		}
		multicast |= (prefix == IPGW_MULTICAST_PREFIX) && (net == IPGW_MULTICAST_NET);
		individual |= (prefix == IPGW_INDIVIDUAL_PREFIX) && (net == IPGW_INDIVIDUAL_NET);
		i += 1 + octets + 4;
		routes++;
	}
	CHECK(multicast && (individual == IPGW_POINT_TO_POINT) && (onLink == (IPGW_LINK_PREFIX == 32)));
	CHECK(routes == (1 + IPGW_POINT_TO_POINT + (IPGW_LINK_PREFIX == 32)));
	const uint8_t *o249 = dhcpOption(r, rlen, 249);
	CHECK(o && o249 && o249[1] == o[1] && memcmp(&o249[2], &o[2], o[1]) == 0);
	CHECK(dhcpOption(r, rlen, 3) == NULL);// no default route
}

static void testDHCPNoReply(void)
{
	uint8_t bootp[300] = { 1, 1, 6, 0, 0x12, 0x34, 0x56, 0x78 };
	uint8_t f[400];

	memcpy(&bootp[28], HOST_MAC, 6);
	bootp[236] = 0x63; bootp[237] = 0x82; bootp[238] = 0x53; bootp[239] = 0x63;
	bootp[240] = 53; bootp[241] = 1; bootp[242] = 1; bootp[243] = 255;
	in(f, hostUdp(f, BCAST_MAC, 0, 0xFFFFFFFF, 68, 67, bootp, sizeof(bootp)));
	CHECK(framesSent == 0);
}

static void testARP(void)
{
	uint8_t f[42] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
	memcpy(f + 6, HOST_MAC, 6); f[12] = 0x08; f[13] = 0x06;
	uint8_t a[28] = { 0, 1, 8, 0, 6, 4, 0, 1 };
	memcpy(a + 8, HOST_MAC, 6);
	put32(&a[14], HOST_IP);
	put32(&a[24], IPGW_INDIVIDUAL_NET | 9);// another radio
	memcpy(f + 14, a, 28);

	in(f, 42);
#if (IPGW_LINK_PREFIX == 31)
	CHECK(framesSent == 0);// routed through the radio, so no proxy ARP

	put32(&f[38], RADIO_IP);
	in(f, 42);
#endif
	CHECK(framesSent == 1);
	CHECK(lastFrame[21] == 2);// reply
	CHECK(memcmp(&lastFrame[22], GW_MAC, 6) == 0);
	CHECK(get32(&lastFrame[28]) == ((IPGW_LINK_PREFIX == 31) ? RADIO_IP : (IPGW_INDIVIDUAL_NET | 9)));

	// Address conflict probe (sender 0.0.0.0) for the host's own address: no reply
	memset(f + 28, 0, 4);
	put32(&f[38], HOST_IP);
	in(f, 42);
	CHECK(framesSent == 0);
}

static void testPing(void)
{
	uint8_t f[100];
	memcpy(f, GW_MAC, 6); memcpy(f + 6, HOST_MAC, 6); f[12] = 0x08; f[13] = 0x00;
	uint8_t *ip = f + 14, *ic = ip + 20;
	memset(ip, 0, 20);
	ip[0] = 0x45; ip[3] = 20 + 16; ip[8] = 64; ip[9] = 1;
	put32(&ip[12], HOST_IP);
	put32(&ip[16], RADIO_IP);
	uint16_t c = sum16(ip, 20, 0); ip[10] = c >> 8; ip[11] = c;
	memset(ic, 0, 16); ic[0] = 8; ic[5] = 1; ic[7] = 1; memcpy(ic + 8, "gd77ping", 8);
	c = sum16(ic, 16, 0); ic[2] = c >> 8; ic[3] = c;

	in(f, 14 + 36);
	CHECK(framesSent == 1);
	CHECK(lastFrame[14 + 9] == 1 && lastFrame[34] == 0);// echo reply
	CHECK(sum16(&lastFrame[34], 16, 0) == 0);
	CHECK(memcmp(&lastFrame[42], "gd77ping", 8) == 0);
}

static void testToAir(void)
{
	uint8_t f[200];
	const uint8_t lrrp[] = { 0x05, 0x08, 0x22, 0x04, 0x00, 0x00, 0x00, 0x01, 0x51, 0x40 };

	air.calls = 0;
	in(f, hostUdp(f, GW_MAC, HOST_IP, IPGW_INDIVIDUAL_NET | 235, 4001, 4001, lrrp, sizeof(lrrp)));// to 12.0.0.235
	CHECK(air.calls == 1 && !air.group && air.dst == 235 && air.srcPort == 4001 && air.dstPort == 4001);
	CHECK(air.length == sizeof(lrrp) && memcmp(air.payload, lrrp, sizeof(lrrp)) == 0);

#if defined(IPGW_GROUP_NET)
	in(f, hostUdp(f, GW_MAC, HOST_IP, IPGW_GROUP_NET | 9, 5000, 4007, lrrp, 4));// to the unicast group range
#else
	in(f, hostUdp(f, GW_MAC, HOST_IP, IPGW_MULTICAST_NET | 9, 5000, 4007, lrrp, 4));// to 225.0.0.9
#endif
	CHECK(air.calls == 2 && air.group && air.dst == 9 && air.srcPort == 5000 && air.dstPort == 4007);

	in(f, hostUdp(f, GW_MAC, HOST_IP, IPGW_MULTICAST_NET | 10, 5000, 4007, lrrp, 4));// to 225.0.0.10
	CHECK(air.calls == 3 && air.group && air.dst == 10);

	in(f, hostUdp(f, BCAST_MAC, HOST_IP, IPGW_BROADCAST_IP, 4005, 4005, lrrp, 4));// broadcast: all call
	CHECK(air.calls == 4 && air.group && air.dst == 0xFFFFFF);
	in(f, hostUdp(f, GW_MAC, HOST_IP, IPGW_ALL_CALL_IP, 4005, 4005, lrrp, 4));// the top individual address: all call
	CHECK(air.calls == 5 && air.group && air.dst == 0xFFFFFF);

#if IPGW_POINT_TO_POINT
	// On a /31 the radio's address is also the radio with the host's ID xor 1, for anything but the serial port
	in(f, hostUdp(f, GW_MAC, HOST_IP, RADIO_IP, 4001, 4001, lrrp, 4));
	CHECK(air.calls == 6 && !air.group && air.dst == (RADIO_ID ^ 1));
	air.calls = 5;
#endif

	in(f, hostUdp(f, BCAST_MAC, HOST_IP, IPGW_BROADCAST_IP, 137, 137, lrrp, 4));// NetBIOS broadcast: dropped
	in(f, hostUdp(f, BCAST_MAC, HOST_IP, 0xFFFFFFFF, 17500, 17500, lrrp, 4));// LAN sync broadcast: dropped
	in(f, hostUdp(f, GW_MAC, HOST_IP, 0x08080808, 53, 53, lrrp, 4));// elsewhere: dropped
#if !IPGW_POINT_TO_POINT
	in(f, hostUdp(f, GW_MAC, HOST_IP, RADIO_IP, 4001, 4001, lrrp, 4));// to the radio itself: dropped
#endif
	in(f, hostUdp(f, GW_MAC, IPGW_INDIVIDUAL_NET | 235, IPGW_INDIVIDUAL_NET | 236, 4001, 4001, lrrp, 4));// not from the host (forwarded): dropped
	CHECK(air.calls == 5 && framesSent == 0);

	// Corrupt IP header checksum: dropped
	int n = hostUdp(f, GW_MAC, HOST_IP, IPGW_INDIVIDUAL_NET | 235, 4001, 4001, lrrp, 4);
	f[24] ^= 0x01;
	in(f, n);
	CHECK(air.calls == 5);
}

// ICMP and SCTP go over the air and back as they are, other protocols don't
static void testOtherProtocols(void)
{
	uint8_t f[200];
	uint8_t echo[16] = { 8, 0, 0, 0, 0x12, 0x34, 0, 1, 'o', 'v', 'e', 'r', ' ', 'a', 'i', 'r' };
	uint16_t c = sum16(echo, sizeof(echo), 0); echo[2] = c >> 8; echo[3] = c;
	// SCTP: common header, then a HEARTBEAT chunk with its info parameter
	uint8_t sctp[24] = { 0x0B, 0xB8, 0x0B, 0xB9, 0xDE, 0xAD, 0xBE, 0xEF, 0, 0, 0, 0, 4, 0, 0, 12, 0, 1, 0, 8, 1, 2, 3, 4 };
	sctpChecksum(sctp, sizeof(sctp));
	const uint8_t tcp[20] = { 0x13, 0x88, 0x00, 0x50, 0, 0, 0, 1, 0, 0, 0, 0, 0x50, 0x02, 0x72, 0x10, 0, 0, 0, 0 };

	air.calls = 0;
	in(f, hostIp(f, GW_MAC, HOST_IP, IPGW_INDIVIDUAL_NET | 235, 1, echo, sizeof(echo)));// ping radio 235
	CHECK(air.calls == 1 && !air.group && air.dst == 235 && air.protocol == 1);
	CHECK(air.length == sizeof(echo) && memcmp(air.payload, echo, sizeof(echo)) == 0);

	in(f, hostIp(f, GW_MAC, HOST_IP, IPGW_MULTICAST_NET | 9, 132, sctp, sizeof(sctp)));// SCTP to talkgroup 9
	CHECK(air.calls == 2 && air.group && air.dst == 9 && air.protocol == 132);
	CHECK(air.length == sizeof(sctp) && memcmp(air.payload, sctp, sizeof(sctp)) == 0);

	in(f, hostIp(f, GW_MAC, HOST_IP, IPGW_INDIVIDUAL_NET | 235, 6, tcp, sizeof(tcp)));// TCP: dropped
	in(f, hostIp(f, BCAST_MAC, HOST_IP, IPGW_BROADCAST_IP, 1, echo, sizeof(echo)));// broadcast ping: dropped
	in(f, hostIp(f, GW_MAC, HOST_IP, IPGW_ALL_CALL_IP, 132, sctp, sizeof(sctp)));// SCTP to the all call: dropped
	CHECK(air.calls == 2 && framesSent == 0);

	// Over the air to the host: from 12.0.0.235, the message unchanged
	echo[0] = 0;// the reply
	echo[2] = echo[3] = 0; c = sum16(echo, sizeof(echo), 0); echo[2] = c >> 8; echo[3] = c;
	CHECK(ipGatewayDeliverIP(false, RADIO_ID, 235, dmrDataIdAddress(false, 235), dmrDataIdAddress(false, RADIO_ID), 1, echo, sizeof(echo)));
	CHECK(memcmp(lastFrame, HOST_MAC, 6) == 0 && lastFrame[14 + 9] == 1);
	CHECK(get32(&lastFrame[26]) == (IPGW_INDIVIDUAL_NET | 235) && get32(&lastFrame[30]) == HOST_IP);
	CHECK(sum16(&lastFrame[14], 20, 0) == 0 && sum16(&lastFrame[34], sizeof(echo), 0) == 0);
	CHECK(lastLength == 34 + (int)sizeof(echo) && memcmp(&lastFrame[34], echo, sizeof(echo)) == 0);

	CHECK(ipGatewayDeliverIP(true, 9, 235, dmrDataIdAddress(false, 235), dmrDataIdAddress(true, 9), 132, sctp, sizeof(sctp)));
	CHECK(lastFrame[0] == 0x01 && lastFrame[14 + 9] == 132 && get32(&lastFrame[30]) == (IPGW_MULTICAST_NET | 9));
	CHECK(memcmp(&lastFrame[34], sctp, sizeof(sctp)) == 0);

	framesSent = 0;
	CHECK(ipGatewayDeliverIP(false, RADIO_ID, 235, dmrDataIdAddress(false, 235), dmrDataIdAddress(false, RADIO_ID), 6, tcp, sizeof(tcp)));// TCP: dropped, not busy
	CHECK(framesSent == 0);

	// UDP through the IP path: the checksum is redone for the host side addresses
	uint8_t udp[12] = { 0x0F, 0xA1, 0x0F, 0xA1, 0, 0, 0, 0, 'l', 'r', 'r', 'p' };
	CHECK(ipGatewayDeliverIP(false, RADIO_ID, 235, dmrDataIdAddress(false, 235), dmrDataIdAddress(false, RADIO_ID), 17, udp, sizeof(udp)));
	uint32_t s = IPGW_INDIVIDUAL_NET | 235, d = HOST_IP;
	uint32_t ps = (s >> 16) + (s & 0xFFFF) + (d >> 16) + (d & 0xFFFF) + 17 + sizeof(udp);
	CHECK(lastFrame[38] == 0 && lastFrame[39] == sizeof(udp) && sum16(&lastFrame[34], sizeof(udp), ps) == 0);
}

static void testDeliver(void)
{
	const uint8_t report[] = { 0x0D, 0x0A, 0x22, 0x04, 0x00, 0x00, 0x00, 0x01, 0x66, 0x10, 0x23, 0x45 };

	CHECK(ipGatewayDeliverUDP(false, RADIO_ID, 235, dmrDataIdAddress(false, 235), dmrDataIdAddress(false, RADIO_ID), 4001, 4001, report, sizeof(report)));// to us
	CHECK(memcmp(lastFrame, HOST_MAC, 6) == 0);
	CHECK(get32(&lastFrame[26]) == (IPGW_INDIVIDUAL_NET | 235));// from 12.0.0.235
	CHECK(get32(&lastFrame[30]) == HOST_IP);
	CHECK(sum16(&lastFrame[14], 20, 0) == 0);

	CHECK(ipGatewayDeliverUDP(false, 236, 235, dmrDataIdAddress(false, 235), dmrDataIdAddress(false, 236), 4001, 4001, report, sizeof(report)));// between two other radios
	CHECK(memcmp(lastFrame, HOST_MAC, 6) != 0 && memcmp(lastFrame, GW_MAC, 6) != 0 && !(lastFrame[0] & 1));
	CHECK(get32(&lastFrame[30]) == (IPGW_INDIVIDUAL_NET | 236));

	CHECK(ipGatewayDeliverUDP(true, 9, 235, dmrDataIdAddress(false, 235), dmrDataIdAddress(true, 9), 4007, 4007, report, sizeof(report)));
	const uint32_t mip = IPGW_MULTICAST_NET | 9;
	const uint8_t mcast[6] = { 0x01, 0x00, 0x5E, (mip >> 16) & 0x7F, (mip >> 8) & 0xFF, mip & 0xFF };
	CHECK(memcmp(lastFrame, mcast, 6) == 0);
	CHECK(get32(&lastFrame[30]) == mip);
	// IDs bigger than the range are truncated: in a /16, data to radio 0x010000 | RADIO_ID is also for the host
	CHECK(ipGatewayDeliverUDP(false, 0x010000 | RADIO_ID, 0x123456, dmrDataIdAddress(false, 0x123456), dmrDataIdAddress(false, 0x010000 | RADIO_ID), 4001, 4001, report, sizeof(report)));
	CHECK((memcmp(lastFrame, HOST_MAC, 6) == 0) == !FITS_INDIVIDUAL(0x010000));// in a /8 it's another radio
	CHECK(get32(&lastFrame[26]) == (IPGW_INDIVIDUAL_NET | (0x123456 & ~IPGW_MASK(IPGW_INDIVIDUAL_PREFIX))));

	CHECK(ipGatewayDeliverUDP(false, RADIO_ID, RADIO_ID ^ 1, dmrDataIdAddress(false, RADIO_ID ^ 1), dmrDataIdAddress(false, RADIO_ID), 4001, 4001, report, sizeof(report)));
	CHECK((get32(&lastFrame[26]) == (IPGW_INDIVIDUAL_NET | (RADIO_ID ^ 1))) && (IPGW_POINT_TO_POINT == (get32(&lastFrame[26]) == RADIO_IP)));

	CHECK(ipGatewayDeliverUDP(true, 0xFFFFFF, 235, dmrDataIdAddress(false, 235), dmrDataIdAddress(true, 0xFFFFFF), 4007, 4007, report, sizeof(report)));// all call
	CHECK(memcmp(lastFrame, BCAST_MAC, 6) == 0);
	CHECK(get32(&lastFrame[30]) == IPGW_BROADCAST_IP);

	const uint8_t record[] = { 1, 1, 1, 6, 0, 3, 0xBD, 0x00, 0x80 };
	CHECK(ipGatewayDeliverMonitor(record, sizeof(record)));
	CHECK(memcmp(lastFrame, BCAST_MAC, 6) == 0);
	CHECK(get32(&lastFrame[26]) == RADIO_IP && get32(&lastFrame[30]) == IPGW_BROADCAST_IP);
	CHECK(lastFrame[36] == (IPGW_MONITOR_PORT >> 8) && lastFrame[37] == (IPGW_MONITOR_PORT & 0xFF));
}

// A frame from hostUdp / hostIp with another TTL (the IPv4 checksum redone)
static void setTtl(uint8_t *f, uint8_t ttl)
{
	uint8_t *ip = f + 14;
	ip[8] = ttl;
	ip[10] = ip[11] = 0;
	uint16_t c = sum16(ip, 20, 0); ip[10] = c >> 8; ip[11] = c;
}

// Real IP addresses (docs/spec/usb-network.md 3.1): the host's TTL kept, the off-net route by the gateway ID, and from
// the air an address that isn't the DMR ID's kept as it is
static void testOffNet(void)
{
	uint8_t f[200];
	const uint8_t text[] = { 0x00, 'h', 0x00, 'i' };
	const uint32_t OFF_NET = 0xC0A80A05u;		// 192.168.10.5
	const uint32_t GATEWAY = 3100500;
	int n;

	int calls = air.calls;// the tests after count on from the ones before

	// In the ranges: the IDs' CAI addresses on the air, the host's source as ours, its TTL kept
	ipGatewaySetOffNetId(0);
	air.calls = 0;
	n = hostUdp(f, GW_MAC, HOST_IP, IPGW_INDIVIDUAL_NET | 235, 5016, 5016, text, sizeof(text));
	setTtl(f, 9);
	in(f, n);
	CHECK(air.calls == 1 && !air.group && air.dst == 235 && air.ttl == 9);
	CHECK(air.srcIp == IPGW_AIR_SRC_OURS && air.dstIp == dmrDataIdAddress(false, 235));
	in(f, hostUdp(f, GW_MAC, HOST_IP, IPGW_MULTICAST_NET | 9, 5016, 5016, text, sizeof(text)));
	CHECK(air.calls == 2 && air.group && air.dst == 9 && air.ttl == 64 && air.dstIp == dmrDataIdAddress(true, 9));

	// Off-net without a gateway ID: dropped, as before
	in(f, hostUdp(f, GW_MAC, HOST_IP, OFF_NET, 5000, 161, text, sizeof(text)));
	CHECK(air.calls == 2 && framesSent == 0);

	// With one: an individual packet to it, the real destination, one hop less
	ipGatewaySetOffNetId(0x01000000u | GATEWAY);
	CHECK(ipGatewayOffNetId() == GATEWAY);
	in(f, hostUdp(f, GW_MAC, HOST_IP, OFF_NET, 5000, 161, text, sizeof(text)));
	CHECK(air.calls == 3 && !air.group && air.dst == GATEWAY && air.dstIp == OFF_NET && air.ttl == 63);
	CHECK(air.srcIp == IPGW_AIR_SRC_OURS && air.protocol == 17 && air.dstPort == 161 && air.length == sizeof(text));
	uint8_t echo[8] = { 8, 0, 0, 0, 0, 1, 0, 1 };
	uint16_t c = sum16(echo, sizeof(echo), 0); echo[2] = c >> 8; echo[3] = c;
	in(f, hostIp(f, GW_MAC, HOST_IP, OFF_NET, 1, echo, sizeof(echo)));
	CHECK(air.calls == 4 && air.dst == GATEWAY && air.dstIp == OFF_NET && air.protocol == 1);
	// TTL 2 leaves with 1; at 1 or 0 it is dropped (the last hop)
	n = hostUdp(f, GW_MAC, HOST_IP, OFF_NET, 5000, 161, text, sizeof(text));
	setTtl(f, 2);
	in(f, n);
	CHECK(air.calls == 5 && air.ttl == 1);
	setTtl(f, 1);
	in(f, n);
	setTtl(f, 0);
	in(f, n);
	CHECK(air.calls == 5);
	// Raw data has no header for the address; never routed: loopback, link local, 0/8, multicast
	raw.calls = 0;
	in(f, hostUdp(f, GW_MAC, HOST_IP, OFF_NET, IPGW_RAW_PORT, IPGW_RAW_PORT, text, sizeof(text)));
	in(f, hostUdp(f, GW_MAC, HOST_IP, 0x7F000001u, 5000, 161, text, sizeof(text)));
	in(f, hostUdp(f, GW_MAC, HOST_IP, 0xA9FE0101u, 5000, 161, text, sizeof(text)));
	in(f, hostUdp(f, GW_MAC, HOST_IP, 0x00000005u, 5000, 161, text, sizeof(text)));
	in(f, hostUdp(f, GW_MAC, HOST_IP, 0xEF010203u, 5000, 161, text, sizeof(text)));
	CHECK(air.calls == 5 && raw.calls == 0);
	// The ranges still go by their own IDs, not to the gateway
	in(f, hostUdp(f, GW_MAC, HOST_IP, IPGW_INDIVIDUAL_NET | 235, 5016, 5016, text, sizeof(text)));
	CHECK(air.calls == 6 && air.dst == 235 && air.ttl == 64);
	ipGatewaySetOffNetId(0);
	air.calls = calls;

	// From the air, to our ID for another address: we are its gateway, so the host gets it as it is
	const uint8_t report[] = { 0x0D, 0x0A, 0x22, 0x04 };
	const uint32_t cai235 = dmrDataIdAddress(false, 235);
	CHECK(ipGatewayDeliverUDP(false, RADIO_ID, 235, cai235, OFF_NET, 4001, 4001, report, sizeof(report)));
	CHECK(memcmp(lastFrame, HOST_MAC, 6) == 0 && get32(&lastFrame[30]) == OFF_NET);
	CHECK(get32(&lastFrame[26]) == (IPGW_INDIVIDUAL_NET | 235) && sum16(&lastFrame[14], 20, 0) == 0);
	// ... from an off-net host behind radio 235: its address kept, to the host's
	CHECK(ipGatewayDeliverUDP(false, RADIO_ID, 235, 0x0A010203u, dmrDataIdAddress(false, RADIO_ID), 4001, 4001, report,
							  sizeof(report)));
	CHECK(memcmp(lastFrame, HOST_MAC, 6) == 0 && get32(&lastFrame[26]) == 0x0A010203u && get32(&lastFrame[30]) == HOST_IP);
	// ... between two other radios: the monitor's MAC, the address kept
	CHECK(ipGatewayDeliverUDP(false, 236, 235, cai235, OFF_NET, 4001, 4001, report, sizeof(report)));
	CHECK(memcmp(lastFrame, HOST_MAC, 6) != 0 && !(lastFrame[0] & 1) && get32(&lastFrame[30]) == OFF_NET);
	// ... a multicast group of the host's: its MAC
	CHECK(ipGatewayDeliverUDP(true, 9, 235, cai235, 0xEF010203u, 4007, 4007, report, sizeof(report)));
	const uint8_t mcast[6] = { 0x01, 0x00, 0x5E, 0x01, 0x02, 0x03 };
	CHECK(memcmp(lastFrame, mcast, 6) == 0 && get32(&lastFrame[30]) == 0xEF010203u);
	// ICMP from the air on the off-net path: an echo reply from an off-net host behind radio 235 to our ID's address
	// reaches the host with the source kept and the message (checksum included) as it came; nothing goes on air
	uint8_t reply[12] = { 0, 0, 0, 0, 0x12, 0x34, 0, 7, 'p', 'o', 'n', 'g' };
	uint16_t rc = sum16(reply, sizeof(reply), 0); reply[2] = rc >> 8; reply[3] = rc;
	int airCalls = air.calls;
	framesSent = 0;
	CHECK(ipGatewayDeliverIP(false, RADIO_ID, 235, 0x0A010203u, dmrDataIdAddress(false, RADIO_ID), 1, reply,
							 sizeof(reply)));
	CHECK(framesSent == 1 && memcmp(lastFrame, HOST_MAC, 6) == 0 && lastFrame[14 + 9] == 1);
	CHECK(get32(&lastFrame[26]) == 0x0A010203u && get32(&lastFrame[30]) == HOST_IP && sum16(&lastFrame[14], 20, 0) == 0);
	CHECK(lastLength == 34 + (int)sizeof(reply) && memcmp(&lastFrame[34], reply, sizeof(reply)) == 0);
	CHECK(sum16(&lastFrame[34], sizeof(reply), 0) == 0);
	// ... and ICMP to our ID for an off-net destination: the host's MAC, the address kept (the host routes it)
	uint8_t request[12] = { 8, 0, 0, 0, 0x12, 0x34, 0, 8, 'p', 'i', 'n', 'g' };
	rc = sum16(request, sizeof(request), 0); request[2] = rc >> 8; request[3] = rc;
	framesSent = 0;
	CHECK(ipGatewayDeliverIP(false, RADIO_ID, 235, cai235, OFF_NET, 1, request, sizeof(request)));
	CHECK(framesSent == 1 && memcmp(lastFrame, HOST_MAC, 6) == 0 && get32(&lastFrame[30]) == OFF_NET);
	CHECK(get32(&lastFrame[26]) == (IPGW_INDIVIDUAL_NET | 235) && memcmp(&lastFrame[34], request, sizeof(request)) == 0);
	CHECK(air.calls == airCalls);// not answered by the radio: no echo reply on air
	// ... a source that would look like the host, the radio or loopback, or a destination of loopback: dropped
	framesSent = 0;
	CHECK(ipGatewayDeliverUDP(false, RADIO_ID, 235, ipGatewayHostIP(), dmrDataIdAddress(false, RADIO_ID), 4001, 4001, report,
							  sizeof(report)));
	CHECK(ipGatewayDeliverUDP(false, RADIO_ID, 235, RADIO_IP, dmrDataIdAddress(false, RADIO_ID), 4001, 4001, report,
							  sizeof(report)));
	CHECK(ipGatewayDeliverUDP(false, RADIO_ID, 235, 0x7F000001u, dmrDataIdAddress(false, RADIO_ID), 4001, 4001, report,
							  sizeof(report)));
	CHECK(ipGatewayDeliverUDP(false, RADIO_ID, 235, cai235, 0x7F000001u, 4001, 4001, report, sizeof(report)));
	CHECK(framesSent == 0);
}

// MMDVMHost's UDP modem protocol: datagrams to the radio's port 3334 carry the serial byte stream, replies go back
static void testSerial(void)
{
	uint8_t f[200];
	const uint8_t getVersion[] = { 0xE0, 0x03, 0x00 };

	CHECK(ipGatewaySerialOut(getVersion, 3));// nobody to send to yet: dropped, not busy
	in(f, hostUdp(f, GW_MAC, HOST_IP, RADIO_IP, 3335, IPGW_SERIAL_PORT, getVersion, sizeof(getVersion)));
	CHECK(serial.calls == 1 && serial.length == 3 && memcmp(serial.data, getVersion, 3) == 0);
	CHECK(framesSent == 0 && air.calls == 5);// not over the air

	const uint8_t reply[] = { 0xE0, 0x04, 0x70, 0x00 };
	CHECK(ipGatewaySerialOut(reply, sizeof(reply)));
	CHECK(get32(&lastFrame[26]) == RADIO_IP && get32(&lastFrame[30]) == HOST_IP);
	CHECK(lastFrame[34] == (IPGW_SERIAL_PORT >> 8) && lastFrame[35] == (IPGW_SERIAL_PORT & 0xFF));
	CHECK(lastFrame[36] == (3335 >> 8) && lastFrame[37] == (3335 & 0xFF));
	CHECK(memcmp(&lastFrame[42], reply, sizeof(reply)) == 0);

	in(f, hostUdp(f, GW_MAC, HOST_IP, RADIO_IP, 3335, 4001, getVersion, sizeof(getVersion)));// other ports: not serial
	CHECK(serial.calls == 1 && framesSent == 0);
}

// Raw DMR data: the payload of a datagram to IPGW_RAW_PORT goes over the air without the IP and UDP headers, and back
static void testRaw(void)
{
	uint8_t f[600];
	const uint8_t mqttsn[] = { 0x02, 0x16 };// MQTT-SN PINGREQ
	int airCalls = air.calls;

	framesSent = 0;
	in(f, hostUdp(f, GW_MAC, HOST_IP, IPGW_INDIVIDUAL_NET | 9990, 50000, IPGW_RAW_PORT, mqttsn, sizeof(mqttsn)));
	CHECK(!raw.group && raw.dst == 9990 && raw.length == 2 && memcmp(raw.data, mqttsn, 2) == 0);
	int calls = raw.calls;

	in(f, hostUdp(f, GW_MAC, HOST_IP, IPGW_MULTICAST_NET | 9, 50000, IPGW_RAW_PORT, mqttsn, sizeof(mqttsn)));
	CHECK(raw.calls == calls + 1 && raw.group && raw.dst == 9);
	in(f, hostUdp(f, BCAST_MAC, HOST_IP, 0xFFFFFFFF, 50000, IPGW_RAW_PORT, mqttsn, sizeof(mqttsn)));// the all call
	CHECK(raw.calls == calls + 2 && raw.group && raw.dst == IPGW_ALL_CALL_ID);
	in(f, hostUdp(f, GW_MAC, HOST_IP, IPGW_INDIVIDUAL_NET | 9990, 50000, IPGW_RAW_PORT, mqttsn, 0));// empty: dropped
	in(f, hostUdp(f, GW_MAC, HOST_IP + 2, IPGW_INDIVIDUAL_NET | 9990, 50000, IPGW_RAW_PORT, mqttsn, sizeof(mqttsn)));// not the host
	CHECK(raw.calls == calls + 2);
	CHECK(air.calls == airCalls && framesSent == 0);// never as IP

	const uint8_t reply[] = { 0x02, 0x17 };// PINGRESP
	CHECK(ipGatewayDeliverRaw(false, RADIO_ID, 9990, reply, sizeof(reply)));
	CHECK(memcmp(lastFrame, HOST_MAC, 6) == 0 && lastFrame[23] == 17);
	CHECK(get32(&lastFrame[26]) == (IPGW_INDIVIDUAL_NET | 9990) && get32(&lastFrame[30]) == HOST_IP);
	CHECK(lastFrame[34] == (IPGW_RAW_PORT >> 8) && lastFrame[35] == (IPGW_RAW_PORT & 0xFF));
	CHECK(lastFrame[36] == (IPGW_RAW_PORT >> 8) && lastFrame[37] == (IPGW_RAW_PORT & 0xFF));
	CHECK(lastFrame[39] == 10 && memcmp(&lastFrame[42], reply, 2) == 0);
	CHECK(sum16(&lastFrame[34], 10, (HOST_IP >> 16) + (HOST_IP & 0xFFFF) + (IPGW_INDIVIDUAL_NET >> 16) + 9990 + 17 + 10) == 0);
}

static void testIdChange(void)
{
	if (!FITS_INDIVIDUAL(0x010000))
	{
		radioId = 0x010000 | RADIO_ID;// a high ID gets its truncated address
		testDHCP(1, 0, 2);
		radioId = 0x010000;// truncates to 0: no lease
		framesSent = 0;
		testDHCPNoReply();
	}

	radioId = 200;
	testDHCP(3, HOST_IP, 6);// renewing 12.0.39.21 is refused
	CHECK(RADIO_IP == (IPGW_POINT_TO_POINT ? (IPGW_INDIVIDUAL_NET | 201) : (IPGW_ALL_CALL_IP - 1)));
	radioId = RADIO_ID;
}

// A DHCP request of the host's: the reply's BOOTP part (NULL when there was none)
static const uint8_t *dhcpExchange(uint8_t type, uint32_t requested, int *rlen)
{
	uint8_t bootp[300] = { 1, 1, 6, 0, 0x12, 0x34, 0x56, 0x78 };
	uint8_t f[400];

	memcpy(&bootp[28], HOST_MAC, 6);
	bootp[236] = 0x63; bootp[237] = 0x82; bootp[238] = 0x53; bootp[239] = 0x63;
	bootp[240] = 53; bootp[241] = 1; bootp[242] = type;
	bootp[243] = 50; bootp[244] = 4;
	for (int i = 0; i < 4; i++) bootp[245 + i] = requested >> (24 - 8 * i);
	bootp[249] = 255;
	in(f, hostUdp(f, BCAST_MAC, 0, 0xFFFFFFFF, 68, 67, bootp, sizeof(bootp)));
	*rlen = lastLength - 42;
	return (framesSent == 1) ? &lastFrame[42] : NULL;
}

// Control station mode's fixed link (docs/spec/usb-network.md 5.3): 192.168.10.1 the radio, .2 the host, a /24, the
// same routes through the radio; packets to our own DMR ID reach the host at .2
static void testFixedLink(void)
{
	const uint32_t radio = IPGW_FIXED_LINK_DEFAULT, host = IPGW_FIXED_LINK_DEFAULT + 1;
	uint8_t f[200];
	int rlen;

	// Addresses that can't be used: refused, the DMR ID addressing stays
	CHECK(!ipGatewaySetFixedLink(0xE0000001u) && !ipGatewaySetFixedLink(IPGW_INDIVIDUAL_NET | 5) &&
		  !ipGatewaySetFixedLink(0xC0A80AFEu) && !ipGatewaySetFixedLink(0xC0A80A00u) && !ipGatewaySetFixedLink(0x7F000001u));
	CHECK((ipGatewayFixedLink() == 0) && (ipGatewayHostIP() == HOST_IP));

	CHECK(ipGatewaySetFixedLink(radio));
	CHECK((ipGatewayRadioIP() == radio) && (ipGatewayHostIP() == host) && (ipGatewayFixedLink() == radio));

	// DHCP: the fixed address, a /24, the radio as server; the individual and multicast ranges through it, no route to
	// the radio itself (it's on the link) and no default route
	const uint8_t *r = dhcpExchange(1, 0, &rlen);
	CHECK(r != NULL);
	if (r != NULL)
	{
		const uint8_t *o = dhcpOption(r, rlen, 53);
		CHECK(o && (o[2] == 2) && (get32(&r[16]) == host));
		o = dhcpOption(r, rlen, 1);
		CHECK(o && (get32(&o[2]) == IPGW_FIXED_NETMASK));
		o = dhcpOption(r, rlen, 54);
		CHECK(o && (get32(&o[2]) == radio));
		o = dhcpOption(r, rlen, 121);
		bool individual = false, multicast = false;
		int routes = 0;
		for (int i = 2; o && (i < 2 + o[1]);)
		{
			int prefix = o[i], octets = (prefix + 7) / 8;
			uint32_t net = 0;
			for (int k = 0; k < octets; k++) net |= (uint32_t)o[i + 1 + k] << (24 - 8 * k);
			CHECK(get32(&o[i + 1 + octets]) == radio);
			individual |= (prefix == IPGW_INDIVIDUAL_PREFIX) && (net == IPGW_INDIVIDUAL_NET);
			multicast |= (prefix == IPGW_MULTICAST_PREFIX) && (net == IPGW_MULTICAST_NET);
			i += 1 + octets + 4;
			routes++;
		}
#if defined(IPGW_GROUP_NET)
		CHECK(individual && multicast && (routes == 3));
#else
		CHECK(individual && multicast && (routes == 2));
#endif
		CHECK(dhcpOption(r, rlen, 3) == NULL);
	}
	r = dhcpExchange(3, host, &rlen);
	CHECK(r && (dhcpOption(r, rlen, 53)[2] == 5));
	r = dhcpExchange(3, HOST_IP, &rlen);// renewing the address by DMR ID: refused, the host starts again
	CHECK(r && (dhcpOption(r, rlen, 53)[2] == 6));

	// ARP: the radio's address only
	uint8_t a[42] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
	memcpy(a + 6, HOST_MAC, 6); a[12] = 0x08; a[13] = 0x06;
	uint8_t arp[28] = { 0, 1, 8, 0, 6, 4, 0, 1 };
	memcpy(arp + 8, HOST_MAC, 6);
	put32(&arp[14], host);
	put32(&arp[24], radio + 4);
	memcpy(a + 14, arp, 28);
	in(a, 42);
	CHECK(framesSent == 0);
	put32(&a[38], radio);
	in(a, 42);
	CHECK((framesSent == 1) && (lastFrame[21] == 2) && (get32(&lastFrame[28]) == radio));

	// To the air from the fixed address; the old one isn't the host's any more
	const uint8_t text[] = { 0x00, 'h', 0x00, 'i' };
	air.calls = 0;
	in(f, hostUdp(f, GW_MAC, host, IPGW_INDIVIDUAL_NET | 235, 5016, 5016, text, sizeof(text)));
	CHECK((air.calls == 1) && !air.group && (air.dst == 235) && (air.dstPort == 5016));
	in(f, hostUdp(f, GW_MAC, HOST_IP, IPGW_INDIVIDUAL_NET | 235, 5016, 5016, text, sizeof(text)));
	CHECK(air.calls == 1);
	in(f, hostUdp(f, BCAST_MAC, host, radio | 0xFF, 4005, 4005, text, sizeof(text)));// the link's broadcast: all call
	CHECK((air.calls == 2) && air.group && (air.dst == 0xFFFFFF));

	// The serial protocol at the radio's fixed address
	serial.calls = 0;
	in(f, hostUdp(f, GW_MAC, host, radio, 3340, IPGW_SERIAL_PORT, text, sizeof(text)));
	CHECK(serial.calls == 1);
	CHECK(ipGatewaySerialOut(text, sizeof(text)) && (get32(&lastFrame[26]) == radio) && (get32(&lastFrame[30]) == host));

	// From the air: to our ID reaches the host at its fixed address (UDP checksum redone: tshark checks the pcap)
	const uint8_t report[] = { 0x0D, 0x0A, 0x22, 0x04, 0x00, 0x00, 0x00, 0x01, 0x66, 0x10, 0x23, 0x45 };
	CHECK(ipGatewayDeliverUDP(false, RADIO_ID, 235, dmrDataIdAddress(false, 235), dmrDataIdAddress(false, RADIO_ID), 4001, 4001, report, sizeof(report)));
	CHECK((memcmp(lastFrame, HOST_MAC, 6) == 0) && (get32(&lastFrame[30]) == host));
	CHECK(get32(&lastFrame[26]) == (IPGW_INDIVIDUAL_NET | 235));
	CHECK(ipGatewayDeliverUDP(false, 236, 235, dmrDataIdAddress(false, 235), dmrDataIdAddress(false, 236), 4001, 4001, report, sizeof(report)));// between other radios
	CHECK((memcmp(lastFrame, HOST_MAC, 6) != 0) && (get32(&lastFrame[30]) == (IPGW_INDIVIDUAL_NET | 236)));
	CHECK(ipGatewayDeliverUDP(true, 0xFFFFFF, 235, dmrDataIdAddress(false, 235), dmrDataIdAddress(true, 0xFFFFFF), 4007, 4007, report, sizeof(report)));// all call
	CHECK(get32(&lastFrame[30]) == 0xFFFFFFFFu);
	CHECK(ipGatewayDeliverMonitor(report, 4) && (get32(&lastFrame[26]) == radio) && (get32(&lastFrame[30]) == 0xFFFFFFFFu));

	// Ping the radio
	uint8_t p[100];
	memcpy(p, GW_MAC, 6); memcpy(p + 6, HOST_MAC, 6); p[12] = 0x08; p[13] = 0x00;
	uint8_t *ip = p + 14, *ic = ip + 20;
	memset(ip, 0, 20);
	ip[0] = 0x45; ip[3] = 20 + 8; ip[8] = 64; ip[9] = 1;
	put32(&ip[12], host);
	put32(&ip[16], radio);
	uint16_t c = sum16(ip, 20, 0); ip[10] = c >> 8; ip[11] = c;
	memset(ic, 0, 8); ic[0] = 8;
	c = sum16(ic, 8, 0); ic[2] = c >> 8; ic[3] = c;
	in(p, 14 + 28);
	CHECK((framesSent == 1) && (lastFrame[34] == 0) && (get32(&lastFrame[26]) == radio));

	// Back to the addressing by DMR ID
	CHECK(ipGatewaySetFixedLink(0));
	CHECK((ipGatewayFixedLink() == 0) && (ipGatewayHostIP() == HOST_IP));
}

// ARP that isn't a request for an IPv4 address over Ethernet (RFC 826: hardware type 1, protocol 0x0800, address
// lengths 6 and 4) is not answered, nor learned from
static void testARPTypes(void)
{
	uint8_t a[42] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
	memcpy(a + 6, HOST_MAC, 6); a[12] = 0x08; a[13] = 0x06;
	const uint8_t arp[28] = { 0, 1, 8, 0, 6, 4, 0, 1 };
	memcpy(a + 14, arp, 28);
	memcpy(a + 22, HOST_MAC, 6);
	put32(&a[28], HOST_IP);
	put32(&a[38], RADIO_IP);
	in(a, 42);
	CHECK(framesSent == 1);// the good one
	a[16] = 0x86; a[17] = 0xDD;// IPv6
	in(a, 42);
	CHECK(framesSent == 0);
	a[16] = 0x08; a[17] = 0x00; a[18] = 8;// 8-byte hardware addresses
	in(a, 42);
	CHECK(framesSent == 0);
	a[18] = 6; a[19] = 16;// 16-byte protocol addresses
	in(a, 42);
	CHECK(framesSent == 0);
	a[19] = 4; a[15] = 6;// hardware type 6 (IEEE 802)
	in(a, 42);
	CHECK(framesSent == 0);
	a[15] = 1; a[21] = 2;// a reply
	in(a, 42);
	CHECK(framesSent == 0);
	a[21] = 1;
	in(a, 41);// one short
	CHECK(framesSent == 0);
	in(a, 42);
	CHECK(framesSent == 1);
}


// The IP header's checksum set right (after a mutation, so the frame gets past the header check)
static void fixIpChecksum(uint8_t *f, int length)
{
	if ((length < 34) || (f[12] != 0x08) || (f[13] != 0x00))
	{
		return;
	}
	int ihl = (f[14] & 0x0F) * 4;
	if ((ihl < 20) || (14 + ihl > length))
	{
		return;
	}
	f[24] = f[25] = 0;
	uint16_t c = sum16(f + 14, ihl, 0);
	f[24] = c >> 8;
	f[25] = c;
}

// Frames from the host that must be dropped without a word: every length short of a header, bad IHL, total length
// past the frame or under the header, a bad header checksum, fragments, UDP lengths outside the datagram, DHCP short
// or without the magic cookie or with options running off the end
static void testMalformed(void)
{
	uint8_t f[1600], g[1600];
	const uint8_t data[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	int calls = air.calls, raws = raw.calls;
	int n = hostUdp(f, GW_MAC, HOST_IP, IPGW_INDIVIDUAL_NET | 9, 4000, 4001, data, 8);// a good one to cut up

	in(f, n);
	CHECK(air.calls == calls + 1);
	calls = air.calls;
	int bad = 0;
	for (int len = 0; len < n; len++)
	{
		uint8_t *p = fuzz_exact(f, len);// exactly len bytes: a read past them is an ASan report
		in(p, len);
		free(p);
		bad += (framesSent != 0);
	}
	CHECK(bad == 0 && air.calls == calls);

	// IHL 4 (16 bytes), IHL 15 with total 28, total 19, total past the frame, a bad checksum, MF set, an offset
	static const struct { int at; uint8_t v; bool fix; } edits[] = {
		{ 14, 0x44, true }, { 14, 0x4F, true }, { 17, 19, true }, { 17, 200, true }, { 25, 0x00, false },
		{ 20, 0x20, true }, { 21, 0x08, true }, { 14, 0x65, true },
	};
	for (unsigned i = 0; i < sizeof(edits) / sizeof(edits[0]); i++)
	{
		memcpy(g, f, n);
		g[edits[i].at] = edits[i].v;
		if (edits[i].fix)
		{
			fixIpChecksum(g, n);
		}
		if (edits[i].at == 25)
		{
			g[25] ^= 0x01;// the checksum off by one
		}
		in(g, n);
		CHECK(framesSent == 0 && air.calls == calls);
	}
	// DF alone is fine (hostUdp sets it): delivered
	in(f, n);
	CHECK(air.calls == calls + 1);
	calls = air.calls;
	// UDP length 7, and one past the IP payload
	memcpy(g, f, n);
	g[39] = 7;
	in(g, n);
	g[39] = 8 + 8 + 1;
	in(g, n);
	CHECK(air.calls == calls && raw.calls == raws);

	// DHCP: 239 bytes, no cookie, an option whose length runs off the end, a pad-only option field
	uint8_t bootp[300] = { 1, 1, 6, 0, 0x12, 0x34, 0x56, 0x78 };
	memcpy(&bootp[28], HOST_MAC, 6);
	bootp[236] = 0x63; bootp[237] = 0x82; bootp[238] = 0x53; bootp[239] = 0x63;
	bootp[240] = 53; bootp[241] = 1; bootp[242] = 1;
	bootp[243] = 255;
	in(g, hostUdp(g, BCAST_MAC, 0, 0xFFFFFFFF, 68, 67, bootp, 239));
	CHECK(framesSent == 0);
	bootp[236] = 0;
	in(g, hostUdp(g, BCAST_MAC, 0, 0xFFFFFFFF, 68, 67, bootp, 300));
	CHECK(framesSent == 0);
	bootp[236] = 0x63;
	bootp[240] = 50; bootp[241] = 200;// the requested address option says 200 bytes; the field ends first
	memset(bootp + 242, 0, 58);
	uint8_t *p = fuzz_exact(bootp, 244);
	in(g, hostUdp(g, BCAST_MAC, 0, 0xFFFFFFFF, 68, 67, p, 244));
	free(p);
	CHECK(framesSent == 0);// no message type: nothing answered
	memset(bootp + 240, 0, 60);// only pads, no end option
	in(g, hostUdp(g, BCAST_MAC, 0, 0xFFFFFFFF, 68, 67, bootp, 300));
	CHECK(framesSent == 0);

	// An echo request as long as a frame can be: answered; one byte longer than the gateway's frame: dropped
	uint8_t ic[1600];
	memset(ic, 0x5A, sizeof(ic));
	ic[0] = 8; ic[1] = 0; ic[2] = ic[3] = 0;
	int icmax = IPGW_MAX_FRAME - 14 - 20;
	uint16_t c = sum16(ic, icmax, 0); ic[2] = c >> 8; ic[3] = c;
	in(g, hostIp(g, GW_MAC, HOST_IP, RADIO_IP, 1, ic, icmax));
	CHECK(framesSent == 1 && lastLength == IPGW_MAX_FRAME && sum16(&lastFrame[34], icmax, 0) == 0);
	ic[2] = ic[3] = 0;
	c = sum16(ic, icmax + 1, 0); ic[2] = c >> 8; ic[3] = c;
	in(g, hostIp(g, GW_MAC, HOST_IP, RADIO_IP, 1, ic, icmax + 1));
	CHECK(framesSent == 0);
}

// Mutated frames of every kind the host sends, half with the IP checksum made right again, in exact-size buffers:
// every frame the gateway sends has a good IP header checksum and fits IPGW_MAX_FRAME, and what goes to the air is
// inside the frame it came in
static void testFuzz(void)
{
	uint8_t seeds[8][700], buf[1600];
	int seedLen[8];
	const uint8_t data[16] = "fuzzing payload";
	uint8_t bootp[300] = { 1, 1, 6, 0, 0x12, 0x34, 0x56, 0x78 };
	memcpy(&bootp[28], HOST_MAC, 6);
	bootp[236] = 0x63; bootp[237] = 0x82; bootp[238] = 0x53; bootp[239] = 0x63;
	bootp[240] = 53; bootp[241] = 1; bootp[242] = 3; bootp[243] = 50; bootp[244] = 4;
	put32(&bootp[245], HOST_IP);
	bootp[249] = 255;
	seedLen[0] = hostUdp(seeds[0], BCAST_MAC, 0, 0xFFFFFFFF, 68, 67, bootp, sizeof(bootp));
	seedLen[1] = hostUdp(seeds[1], GW_MAC, HOST_IP, IPGW_INDIVIDUAL_NET | 9, 4000, 4001, data, 16);
	seedLen[2] = hostUdp(seeds[2], GW_MAC, HOST_IP, RADIO_IP, 4000, IPGW_SERIAL_PORT, data, 16);
	seedLen[3] = hostUdp(seeds[3], GW_MAC, HOST_IP, IPGW_ALL_CALL_IP, 4000, IPGW_RAW_PORT, data, 16);
	uint8_t ic[24] = { 8, 0, 0, 0, 0, 1, 0, 1, 'p', 'i', 'n', 'g' };
	uint16_t c = sum16(ic, 24, 0); ic[2] = c >> 8; ic[3] = c;
	seedLen[4] = hostIp(seeds[4], GW_MAC, HOST_IP, RADIO_IP, 1, ic, 24);
	uint8_t sctp[28] = { 0x13, 0x88, 0x13, 0x88 };
	seedLen[5] = hostIp(seeds[5], GW_MAC, HOST_IP, IPGW_MULTICAST_NET | 91, 132, sctp, 28);
	uint8_t a[42] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
	memcpy(a + 6, HOST_MAC, 6); a[12] = 0x08; a[13] = 0x06;
	const uint8_t arp[8] = { 0, 1, 8, 0, 6, 4, 0, 1 };
	memcpy(a + 14, arp, 8);
	memcpy(a + 22, HOST_MAC, 6);
	put32(&a[28], HOST_IP);
	put32(&a[38], IPGW_INDIVIDUAL_NET | 9);
	memcpy(seeds[6], a, 42);
	seedLen[6] = 42;
	seedLen[7] = hostUdp(seeds[7], GW_MAC, HOST_IP, 0xE00000FBu, 5353, 5353, data, 16);

	fuzz_t f;
	fuzz_init(&f, 0x49504757u);
	long iters = fuzz_iters(20000), bad = 0, out = 0;
	for (long i = 0; i < iters; i++)
	{
		int s = (int)fuzz_below(&f, 8);
		memcpy(buf, seeds[s], seedLen[s]);
		int len = fuzz_mutate(&f, buf, seedLen[s], (int)sizeof(buf));
		if (fuzz_below(&f, 2) == 0)
		{
			fixIpChecksum(buf, len);
		}
		uint8_t *p = fuzz_exact(buf, len);
		int calls = air.calls + raw.calls + serial.calls;
		in(p, len);
		free(p);
		out += framesSent;
		if (framesSent > 0)
		{
			bool ok = (lastLength <= IPGW_MAX_FRAME) && (lastLength >= 14);
			if (ok && (lastFrame[12] == 0x08) && (lastFrame[13] == 0x00))
			{
				ok = (lastLength >= 34) && (sum16(&lastFrame[14], 20, 0) == 0);
			}
			if (!ok && (bad++ < 3))
			{
				printf("  fuzz #%ld: a bad frame out (%d bytes)\n", i, lastLength);
			}
		}
		if ((air.calls + raw.calls + serial.calls) > calls + 1)
		{
			bad++;// one input, at most one thing out
		}
	}
	CHECK(bad == 0 && out > 0);
	printf("# ipGateway fuzz: %ld frames in, %ld out\n", iters, out);
}

int main(void)
{
	uint32_t header[6] = { 0xA1B2C3D4, 0x00040002, 0, 0, 65535, 1 };// pcap, Ethernet

	pcap = fopen(PCAP_NAME, "wb");
	fwrite(header, sizeof(header), 1, pcap);

	ipGatewayInit(GW_MAC, HOST_MAC);
	testDHCP(1, 0, 2);
	testDHCP(3, HOST_IP, 5);
	testARP();
	testARPTypes();
	testPing();
	testToAir();
	testDeliver();
	testOffNet();
	testSerial();
	testOtherProtocols();
	testRaw();
	testIdChange();
	testFixedLink();
	testMalformed();
	testFuzz();

	fclose(pcap);
	printf("# %s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
	return failures ? 1 : 0;
}
