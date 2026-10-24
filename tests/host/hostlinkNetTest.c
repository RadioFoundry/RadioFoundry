// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The host link's network personality (core/hostlink/hostlink_net.c) with the real IP gateway (core/dmr/ipGateway.c)
// and packet codec (core/dmr/dmrData.c), against a fake USB network interface (board.netif) and fake air ops: frames
// from the host reach the air ops, packets from the air reach the host, UDP serial both ways; control station mode
// (passthrough follows the link, the fixed link pair end to end)
#include <stdio.h>
#include <string.h>
#include "board.h"
#include "hal_time.h"
#include "hostlink/hostlink_net.h"
#include "hostlink/hostlink_net_air.h"
#include "dmr/dmrDataService.h"
#include "dmr/dmr_data_api.h"
#include "dmr/ipGateway.h"
#include "dmr/dmrData.h"
#include "net/tcp.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define RADIO_ID	10005u
#define HOST_IP		(IPGW_INDIVIDUAL_NET | RADIO_ID)	// 12.0.39.21

static const uint8_t HOST_MAC[6] = { 0x02, 0x44, 0x01, 0x02, 0x03, 0x04 };
static const uint8_t RADIO_MAC[6] = { 0x02, 0x44, 0x01, 0x02, 0x03, 0x05 };

// ---- fake time ----
static uint32_t now_ms = 1000;
uint32_t hal_millis(void) { return now_ms; }
void hal_delay_ms(uint32_t ms) { now_ms += ms; }

// ---- fake USB network interface ----
static bool link_up = true;
static int busy_sends;				// the next n sends report the IN endpoint busy
static uint8_t rx_frame[700];
static int rx_len;
static uint8_t sent[8][700];
static int sent_len[8], sent_count;

static bool n_select(bool network) { (void)network; return true; }
static bool n_selected(void) { return true; }
static bool n_up(void) { return link_up; }
static bool n_send(const uint8_t *f, int len)
{
	if (busy_sends > 0) {
		busy_sends--;
		return false;
	}
	if (sent_count < 8) {
		memcpy(sent[sent_count], f, len);
		sent_len[sent_count] = len;
	}
	sent_count++;
	return true;
}
static int n_receive(uint8_t *buf, int max)
{
	int n = rx_len < max ? rx_len : max;
	if (n > 0) {
		memcpy(buf, rx_frame, n);
	}
	rx_len = 0;
	return n;
}
static void n_tick(uint32_t t) { (void)t; }
static void n_macs(uint8_t host[6], uint8_t radio[6]) { memcpy(host, HOST_MAC, 6); memcpy(radio, RADIO_MAC, 6); }
static const hal_netif_t fake_netif = { n_select, n_selected, n_up, n_send, n_receive, n_tick, n_macs, NULL, 600 };
const board_t board = { .name = "fake", .netif = &fake_netif };

// ---- fake air ops ----
static struct {
	int ip_calls, raw_calls;
	bool group;
	uint32_t dst;
	uint32_t src_ip, dst_ip;
	uint8_t ttl;
	uint8_t protocol;
	uint8_t data[600];
	int len;
	bool refuse;
} air;
static uint32_t radio_id(void) { return RADIO_ID; }
static bool ip_to_air(bool group, uint32_t dst, uint32_t src_ip, uint32_t dst_ip, uint8_t ttl, uint8_t protocol,
					  const uint8_t *l4, int length)
{
	if (air.refuse) {
		return false;
	}
	air.ip_calls++;
	air.group = group;
	air.dst = dst;
	air.src_ip = src_ip;
	air.dst_ip = dst_ip;
	air.ttl = ttl;
	air.protocol = protocol;
	memcpy(air.data, l4, length);
	air.len = length;
	return true;
}
static bool raw_to_air(bool group, uint32_t dst, const uint8_t *data, int length)
{
	air.raw_calls++;
	air.group = group;
	air.dst = dst;
	memcpy(air.data, data, length);
	air.len = length;
	return true;
}
static const hostlink_net_air_t air_ops = { radio_id, ip_to_air, raw_to_air };

static uint8_t serial_got[64];
static int serial_len;
static void serial_in(const uint8_t *buf, size_t len)
{
	memcpy(serial_got, buf, len);
	serial_len = (int)len;
}

// ---- a local UDP service (ipGatewayUdpBindAir) ----
static struct {
	int calls;
	bool from_air;
	uint32_t src;
	uint16_t port;
	uint8_t data[64];
	int len;
} local;
static void local_udp(bool from_air, uint32_t src, uint16_t port, uint32_t dst, uint16_t dport, const uint8_t *p, int len)
{
	(void)dst;
	(void)dport;
	local.calls++;
	local.from_air = from_air;
	local.src = src;
	local.port = port;
	local.len = len;
	memcpy(local.data, p, (size_t)len);
}

// ---- fake data service (hostlink_net_air.c's radio task side) ----
static bool service_busy;
static dmrBurst_t svc_bursts[DMR_DATA_MAX_BURSTS];
static int svc_count, svc_lists;
bool dmrDataServiceSendBursts(const dmrBurst_t *bursts, int count)
{
	if (service_busy) {
		return false;
	}
	memcpy(svc_bursts, bursts, (size_t)count * sizeof(dmrBurst_t));
	svc_count = count;
	svc_lists++;
	return true;
}

// The service's IP path: built by the codec as the service builds an unconfirmed packet (the compression setting
// applied; confirmed delivery is dmrDataServiceTest's), done at once unless ip_pending says otherwise
static bool ip_pending;
static uint16_t get16(const uint8_t *p);
static uint8_t ip_result = DMR_REQ_OK;
bool dmrDataServiceSendIP(bool group, uint32_t dst, uint32_t srcIp, uint32_t dstIp, uint8_t ttl, uint8_t protocol,
						  const uint8_t *l4, int length)
{
	const uint8_t *pdu;
	uint8_t sap;

	if (service_busy) {
		return false;
	}
	srcIp = (srcIp == DMR_DATA_IP_OURS) ? dmrDataIdAddress(false, RADIO_ID) : srcIp;
	dstIp = (dstIp == DMR_DATA_IP_OURS) ? dmrDataIdAddress(group, dst) : dstIp;
	bool compress = (protocol == DMR_IP_PROTO_UDP) && (length >= 8) && dmrDataUdpCompress(get16(&l4[0]), get16(&l4[2]));
	int n = dmrDataEncodeIP(group, dst, RADIO_ID, dstIp, srcIp, ttl, protocol, l4, length, compress, dmrDataNextIpId(),
							&pdu, &sap);
	svc_count = (n > 0) ? dmrDataBuildPacket(DMR_DPF_UNCONFIRMED, sap, group, dst, RADIO_ID, pdu, n, DT_RATE_12_DATA, 2,
											 svc_bursts, DMR_DATA_MAX_BURSTS) : 0;
	svc_lists++;
	return svc_count > 0;
}
bool dmrDataServiceIpBusy(void) { return ip_pending; }
uint8_t dmrDataServiceIpResult(void) { return ip_result; }

// The packet the bursts carry, decoded by the codec's receiver; NULL if none
static const dmrDataPacket_t *svc_packet(void)
{
	const dmrDataPacket_t *got = NULL;

	dmrDataRxReset();
	for (int i = 0; i < svc_count; i++) {
		if (dmrDataRxBurst(&svc_bursts[i]) == DMR_DATA_RX_PACKET) {
			got = &dmrDataRxPacket;
		}
	}
	return got;
}

// ---- frame building ----
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)(v >> 16)); put16(p + 2, (uint16_t)v); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t get32(const uint8_t *p) { return ((uint32_t)get16(p) << 16) | get16(p + 2); }

static uint16_t ip_checksum(const uint8_t *p, int len)
{
	uint32_t sum = 0;
	for (int i = 0; i + 1 < len; i += 2) {
		sum += get16(p + i);
	}
	while (sum >> 16) {
		sum = (sum & 0xFFFF) + (sum >> 16);
	}
	return (uint16_t)~sum;
}

static uint32_t host_src = HOST_IP;		// the host's address (the fixed link's in control station mode)

// A UDP datagram from the host (no UDP checksum) in an Ethernet frame to the radio's MAC: its length
static int host_udp(uint8_t *f, uint32_t dst_ip, uint16_t sport, uint16_t dport, const uint8_t *data, int len)
{
	memcpy(f, RADIO_MAC, 6);
	memcpy(f + 6, HOST_MAC, 6);
	put16(f + 12, 0x0800);
	uint8_t *ip = f + 14;
	memset(ip, 0, 20);
	ip[0] = 0x45;
	put16(ip + 2, (uint16_t)(20 + 8 + len));
	ip[8] = 64;
	ip[9] = 17;
	put32(ip + 12, host_src);
	put32(ip + 16, dst_ip);
	put16(ip + 10, ip_checksum(ip, 20));
	uint8_t *udp = ip + 20;
	put16(udp, sport);
	put16(udp + 2, dport);
	put16(udp + 4, (uint16_t)(8 + len));
	put16(udp + 6, 0);
	memcpy(udp + 8, data, len);
	return 14 + 20 + 8 + len;
}

// A TCP segment (20 byte header, no options) from src in an Ethernet frame to the radio's MAC: its length
static int host_tcp(uint8_t *f, uint32_t src_ip, uint32_t dst_ip, uint16_t sport, uint16_t dport, uint32_t seq,
		    uint32_t ack, uint8_t flags)
{
	memcpy(f, RADIO_MAC, 6);
	memcpy(f + 6, HOST_MAC, 6);
	put16(f + 12, 0x0800);
	uint8_t *ip = f + 14;
	memset(ip, 0, 40);
	ip[0] = 0x45;
	put16(ip + 2, 40);
	ip[8] = 64;
	ip[9] = 6;
	put32(ip + 12, src_ip);
	put32(ip + 16, dst_ip);
	put16(ip + 10, ip_checksum(ip, 20));
	uint8_t *t = ip + 20;
	put16(t, sport);
	put16(t + 2, dport);
	put32(t + 4, seq);
	put32(t + 8, ack);
	t[12] = 0x50;
	t[13] = flags;
	put16(t + 14, 8192);
	put16(t + 16, tcp_checksum(src_ip, dst_ip, t, 20));
	return 14 + 40;
}

static int tcp_opened = -1;
static void t_opened(int c) { tcp_opened = c; }
static const tcp_app_t tcp_app = { .opened = t_opened };

static void from_host(const uint8_t *f, int len)
{
	memcpy(rx_frame, f, len);
	rx_len = len;
	hostlink_net_poll(now_ms);
}

int main(void)
{
	uint8_t f[700];
	static const uint8_t hello[] = "hello DMR";

	hostlink_net_set_air(&air_ops);
	hostlink_net_init(serial_in);
	CHECK(ipGatewayHostIP() == HOST_IP && ipGatewayRadioIP() == (HOST_IP ^ 1u));

	// UDP from the host to radio 100's address: the air op gets it as IP (layer 4 on)
	from_host(f, host_udp(f, IPGW_INDIVIDUAL_NET | 100u, 5000, 5001, hello, sizeof(hello)));
	CHECK(air.ip_calls == 1 && !air.group && air.dst == 100 && air.protocol == 17 && air.len == 8 + (int)sizeof(hello));
	CHECK(get16(air.data) == 5000 && get16(air.data + 2) == 5001 && memcmp(air.data + 8, hello, sizeof(hello)) == 0);

	// To a talkgroup's multicast address
	from_host(f, host_udp(f, IPGW_MULTICAST_NET | 9u, 5000, 5001, hello, sizeof(hello)));
	CHECK(air.ip_calls == 2 && air.group && air.dst == 9);

	// To the raw port: the payload alone
	from_host(f, host_udp(f, IPGW_INDIVIDUAL_NET | 100u, IPGW_RAW_PORT, IPGW_RAW_PORT, hello, sizeof(hello)));
	CHECK(air.raw_calls == 1 && air.dst == 100 && air.len == (int)sizeof(hello) && memcmp(air.data, hello, air.len) == 0);

	// Refused by the air side: counted
	air.refuse = true;
	from_host(f, host_udp(f, IPGW_INDIVIDUAL_NET | 100u, 5000, 5001, hello, sizeof(hello)));
	air.refuse = false;
	hostlink_net_stats_t st;
	hostlink_net_stats(&st);
	CHECK(st.to_air == 3 && st.to_air_refused == 1 && st.frames_in == 4);

	// UDP serial: bytes to the radio's own address on 3334 reach the host link's dispatch ...
	static const uint8_t version_req[] = { 0xE0, 0x03, 0x00 };
	from_host(f, host_udp(f, HOST_IP ^ 1u, 40123, IPGW_SERIAL_PORT, version_req, sizeof(version_req)));
	CHECK(serial_len == 3 && memcmp(serial_got, version_req, 3) == 0);
	// ... and replies go back to that host port as datagrams of at most 512 bytes, waiting out a busy IN endpoint
	static uint8_t reply[1100];
	for (int i = 0; i < (int)sizeof(reply); i++) {
		reply[i] = (uint8_t)i;
	}
	sent_count = 0;
	busy_sends = 3;
	CHECK(hostlink_net_write(reply, sizeof(reply)) == sizeof(reply));
	CHECK(sent_count == 3);
	const uint8_t *ip = sent[0] + 14, *udp = ip + 20;
	CHECK(memcmp(sent[0], HOST_MAC, 6) == 0 && get32(ip + 16) == HOST_IP && get32(ip + 12) == (HOST_IP ^ 1u));
	CHECK(get16(udp) == IPGW_SERIAL_PORT && get16(udp + 2) == 40123 && get16(udp + 4) == 8 + 512);
	CHECK(memcmp(udp + 8, reply, 512) == 0);
	CHECK(get16(sent[2] + 14 + 20 + 4) == 8 + (1100 - 1024) && memcmp(sent[2] + 42, reply + 1024, 76) == 0);
	// A host that stops reading: the write gives up
	busy_sends = 1000;
	CHECK(hostlink_net_write(reply, 10) == 0);
	busy_sends = 0;

	// A packet from the air to us (IP, SAP 4): the host gets the UDP datagram from the sender's address
	dmrDataPacket_t p;
	memset(&p, 0, sizeof(p));
	p.sap = DMR_SAP_IP;
	p.dst = RADIO_ID;
	p.src = 3101234;
	uint8_t *pip = p.data;
	pip[0] = 0x45;
	put16(pip + 2, (uint16_t)(20 + 8 + sizeof(hello)));
	pip[8] = 64;
	pip[9] = 17;
	put32(pip + 12, IPGW_INDIVIDUAL_NET | 3101234u);
	put32(pip + 16, IPGW_INDIVIDUAL_NET | RADIO_ID);
	put16(pip + 10, ip_checksum(pip, 20));
	put16(pip + 20, 4000);
	put16(pip + 22, 4001);
	put16(pip + 24, (uint16_t)(8 + sizeof(hello)));
	memcpy(pip + 28, hello, sizeof(hello));
	p.length = (uint16_t)(28 + sizeof(hello));
	sent_count = 0;
	busy_sends = 1;			// the first try finds the IN endpoint busy: delivered on the next poll
	hostlink_net_packet(&p);
	hostlink_net_poll(now_ms);
	CHECK(sent_count == 0);
	hostlink_net_poll(now_ms);
	CHECK(sent_count == 1);
	ip = sent[0] + 14;
	udp = ip + 20;
	CHECK(memcmp(sent[0], HOST_MAC, 6) == 0 && ip[9] == 17 && get32(ip + 12) == (IPGW_INDIVIDUAL_NET | 3101234u) &&
	      get32(ip + 16) == HOST_IP && get16(udp) == 4000 && get16(udp + 2) == 4001);
	CHECK(memcmp(udp + 8, hello, sizeof(hello)) == 0 && ip_checksum(ip, 20) == 0);

	// Short data (SAP 10) to us: the raw port
	memset(&p, 0, sizeof(p));
	p.sap = DMR_SAP_SHORT_DATA;
	p.dst = RADIO_ID;
	p.src = 3101234;
	memcpy(p.data, hello, sizeof(hello));
	p.length = sizeof(hello);
	sent_count = 0;
	hostlink_net_packet(&p);
	hostlink_net_poll(now_ms);
	udp = sent[0] + 14 + 20;
	CHECK(sent_count == 1 && get16(udp) == IPGW_RAW_PORT && get16(udp + 2) == IPGW_RAW_PORT &&
	      memcmp(udp + 8, hello, sizeof(hello)) == 0);

	// Link down: packets from the air are dropped, not held
	link_up = false;
	hostlink_net_packet(&p);
	link_up = true;
	sent_count = 0;
	hostlink_net_poll(now_ms);
	hostlink_net_stats(&st);
	CHECK(sent_count == 0 && st.packets_dropped == 1 && st.packets_up == 2 && st.packets_air == 3);

	// Local UDP services: port 161 on the radio's own address from the host goes to the bound service, not to the air
	CHECK(ipGatewayUdpBindAir(161, local_udp));
	int ip_before = air.ip_calls;
	from_host(f, host_udp(f, HOST_IP ^ 1u, 40000, 161, hello, sizeof(hello)));
	CHECK(local.calls == 1 && !local.from_air && local.src == HOST_IP && local.port == 40000 &&
	      local.len == (int)sizeof(hello) && memcmp(local.data, hello, sizeof(hello)) == 0 && air.ip_calls == ip_before);
	// Another port of the radio's address still goes over the air (to the radio with that ID)
	from_host(f, host_udp(f, HOST_IP ^ 1u, 40000, 162, hello, sizeof(hello)));
	CHECK(local.calls == 1 && air.ip_calls == ip_before + 1 && air.dst == (RADIO_ID ^ 1u));
	// UDP over the air (SAP 4) to our ID and port 161: the service, from the radio task, with the link down too;
	// nothing goes to the host
	memset(&p, 0, sizeof(p));
	p.sap = DMR_SAP_IP;
	p.dst = RADIO_ID;
	p.src = 3101234;
	pip[0] = 0x45;
	put16(pip + 2, (uint16_t)(20 + 8 + sizeof(hello)));
	pip[8] = 64;
	pip[9] = 17;
	put32(pip + 12, IPGW_INDIVIDUAL_NET | 3101234u);
	put32(pip + 16, IPGW_INDIVIDUAL_NET | RADIO_ID);
	put16(pip + 10, ip_checksum(pip, 20));
	put16(pip + 20, 4321);
	put16(pip + 22, 161);
	put16(pip + 24, (uint16_t)(8 + sizeof(hello)));
	memcpy(pip + 28, hello, sizeof(hello));
	p.length = (uint16_t)(28 + sizeof(hello));
	link_up = false;
	sent_count = 0;
	hostlink_net_packet(&p);
	link_up = true;
	hostlink_net_poll(now_ms);
	CHECK(local.calls == 2 && local.from_air && local.src == 3101234 && local.port == 4321 &&
	      local.len == (int)sizeof(hello) && sent_count == 0);
	// ... but not when it is for a talkgroup, or for another ID: those go to the host as before
	p.group = true;
	hostlink_net_packet(&p);
	hostlink_net_poll(now_ms);
	p.group = false;
	p.dst = RADIO_ID + 1;
	hostlink_net_packet(&p);
	hostlink_net_poll(now_ms);
	CHECK(local.calls == 2 && sent_count == 2);
	// ... nor when its IP destination is another address (we are the gateway to it): to the host, the address kept
	p.dst = RADIO_ID;
	put32(pip + 16, 0xC0A80A05u);
	put16(pip + 10, 0);
	put16(pip + 10, ip_checksum(pip, 20));
	sent_count = 0;
	hostlink_net_packet(&p);
	hostlink_net_poll(now_ms);
	CHECK(local.calls == 2 && sent_count == 1 && memcmp(sent[0], HOST_MAC, 6) == 0 &&
	      get32(sent[0] + 14 + 16) == 0xC0A80A05u);
	put32(pip + 16, IPGW_INDIVIDUAL_NET | RADIO_ID);
	put16(pip + 10, 0);
	put16(pip + 10, ip_checksum(pip, 20));
	// A port bound for the link only (TFTP, mDNS) never takes datagrams from the air: they go to the host
	CHECK(ipGatewayUdpBind(69, local_udp));
	p.dst = RADIO_ID;
	put16(pip + 22, 69);
	sent_count = 0;
	hostlink_net_packet(&p);
	hostlink_net_poll(now_ms);
	CHECK(local.calls == 2 && sent_count == 1);
	from_host(f, host_udp(f, HOST_IP ^ 1u, 40000, 69, hello, sizeof(hello)));	// from the link it does
	CHECK(local.calls == 3 && !local.from_air);
	CHECK(ipGatewayUdpBind(69, NULL));
	put16(pip + 22, 161);
	// The service's answer to the host: from the radio's address and port 161
	sent_count = 0;
	CHECK(ipGatewayUdpSend(HOST_IP, 161, 40000, hello, sizeof(hello)) && sent_count == 1);
	ip = sent[0] + 14;
	udp = ip + 20;
	CHECK(memcmp(sent[0], HOST_MAC, 6) == 0 && get32(ip + 12) == (HOST_IP ^ 1u) && get32(ip + 16) == HOST_IP &&
	      get16(udp) == 161 && get16(udp + 2) == 40000 && memcmp(udp + 8, hello, sizeof(hello)) == 0);

	// The default air side (hostlink_net_air.c): the host link task's packet waits in the mailbox for the radio
	// task's tick, which builds the bursts and hands them to the data service
	hostlink_net_air_start();
	hostlink_net_air_tick(RADIO_ID, now_ms);
	CHECK(ipGatewayRadioId() == RADIO_ID);
	from_host(f, host_udp(f, IPGW_INDIVIDUAL_NET | 100u, 5000, 5001, hello, sizeof(hello)));
	CHECK(svc_lists == 0);		// nothing from the host link task itself
	from_host(f, host_udp(f, IPGW_INDIVIDUAL_NET | 101u, 5000, 5001, hello, sizeof(hello)));
	hostlink_net_stats(&st);
	CHECK(st.to_air_refused == 2);	// one packet at a time
	hostlink_net_air_tick(RADIO_ID, now_ms);
	CHECK(svc_lists == 1 && svc_count > 2);
	const dmrDataPacket_t *pk = svc_packet();
	dmrDataUDP_t u;
	// Ports outside 4000-4099: the compressed UDP/IPv4 header (SAP 3), read back by the receive decoder
	CHECK(pk != NULL && pk->sap == DMR_SAP_UDPIP_COMPRESSION && pk->dst == 100 && pk->src == RADIO_ID && !pk->group);
	CHECK(pk != NULL && dmrDataGetUDP(pk, &u) && u.srcPort == 5000 && u.dstPort == 5001 &&
	      u.length == (int)sizeof(hello) && memcmp(u.payload, hello, sizeof(hello)) == 0);
	// A DMR application port (TMS 4007) keeps the full header the MOTOTRBO side expects; the service holds the
	// mailbox until it is done (a confirmed packet waits for its response)
	hostlink_net_air_tick(RADIO_ID, now_ms);	// the service is done with the first: the mailbox is free
	ip_pending = true;
	from_host(f, host_udp(f, IPGW_INDIVIDUAL_NET | 100u, 4007, 4007, hello, sizeof(hello)));
	hostlink_net_air_tick(RADIO_ID, now_ms);
	CHECK(svc_lists == 2);
	pk = svc_packet();
	CHECK(pk != NULL && pk->sap == DMR_SAP_IP && dmrDataGetUDP(pk, &u) && u.dstPort == 4007);
	from_host(f, host_udp(f, IPGW_INDIVIDUAL_NET | 101u, 5000, 5001, hello, sizeof(hello)));
	hostlink_net_stats(&st);
	CHECK(st.to_air_refused == 3);
	ip_pending = false;
	ip_result = DMR_REQ_FAILED;
	hostlink_net_air_tick(RADIO_ID, now_ms);
	ip_result = DMR_REQ_OK;
	svc_lists = 1;

	// Raw data to a talkgroup: an unconfirmed short data packet (SAP 10); a busy service is tried again
	service_busy = true;
	from_host(f, host_udp(f, IPGW_MULTICAST_NET | 9u, IPGW_RAW_PORT, IPGW_RAW_PORT, hello, sizeof(hello)));
	hostlink_net_air_tick(RADIO_ID, now_ms);
	CHECK(svc_lists == 1);
	service_busy = false;
	now_ms += 100;
	hostlink_net_air_tick(RADIO_ID, now_ms);
	CHECK(svc_lists == 2);
	pk = svc_packet();
	CHECK(pk != NULL && pk->sap == DMR_SAP_SHORT_DATA && pk->group && pk->dst == 9 &&
	      pk->length == (int)sizeof(hello) && memcmp(pk->data, hello, sizeof(hello)) == 0);

	// Still busy after HOSTLINK_NET_AIR_GIVE_UP_MS: dropped
	service_busy = true;
	from_host(f, host_udp(f, IPGW_INDIVIDUAL_NET | 100u, 5000, 5001, hello, sizeof(hello)));
	hostlink_net_air_tick(RADIO_ID, now_ms);
	now_ms += HOSTLINK_NET_AIR_GIVE_UP_MS;
	hostlink_net_air_tick(RADIO_ID, now_ms);
	service_busy = false;
	hostlink_net_air_stats_t as;
	hostlink_net_air_stats(&as);
	CHECK(as.queued == 4 && as.sent == 2 && as.failed == 1 && as.dropped == 1 && as.refused_busy == 3);
	// Off-net through the gateway ID: the mailbox carries the real destination and the TTL to the service, which
	// sends the full header to the gateway radio from our ID's address
	ipGatewaySetOffNetId(3100500);
	from_host(f, host_udp(f, 0xC0A80A05u, 5000, 5001, hello, sizeof(hello)));
	hostlink_net_air_tick(RADIO_ID, now_ms);
	CHECK(svc_lists == 3);
	pk = svc_packet();
	CHECK(pk != NULL && pk->sap == DMR_SAP_IP && pk->dst == 3100500 && !pk->group && pk->data[8] == 63);
	CHECK(pk != NULL && dmrDataGetUDP(pk, &u) && u.dstIp == 0xC0A80A05u && u.srcIp == dmrDataIdAddress(false, RADIO_ID));
	ipGatewaySetOffNetId(0);
	hostlink_net_air_tick(RADIO_ID, now_ms);
	// The packet hook passes what the service hears on to the host
	memset(&p, 0, sizeof(p));
	p.sap = DMR_SAP_SHORT_DATA;
	p.dst = RADIO_ID;
	p.src = 3101234;
	memcpy(p.data, hello, sizeof(hello));
	p.length = sizeof(hello);
	sent_count = 0;
	hostlink_net_air_packet(&p, NULL);
	hostlink_net_poll(now_ms);
	CHECK(sent_count == 1);

	// TCP to the radio itself (CONFIG_NET_TCP): a port nobody listens on is refused with a RST from the radio's
	// address, also from a source beyond the host (the radio's own connections get their replies that way)
	uint32_t radio_ip = HOST_IP ^ 1u;
	sent_count = 0;
	from_host(f, host_tcp(f, HOST_IP, radio_ip, 40000, 9, 1000, 0, 0x02));
	CHECK(sent_count == 1 && sent[0][14 + 9] == 6 && get32(sent[0] + 14 + 12) == radio_ip);
	CHECK(get32(sent[0] + 14 + 16) == HOST_IP && sent[0][14 + 20 + 13] == 0x14 && get32(sent[0] + 14 + 28) == 1001);
	CHECK(memcmp(sent[0], HOST_MAC, 6) == 0 && ip_checksum(sent[0] + 14, 20) == 0);
	CHECK(tcp_checksum(radio_ip, HOST_IP, sent[0] + 34, 20) == 0);
	sent_count = 0;
	from_host(f, host_tcp(f, 0xC0000201u, radio_ip, 40000, 9, 1000, 0, 0x02));
	CHECK(sent_count == 1 && get32(sent[0] + 14 + 16) == 0xC0000201u);
	// TCP to another radio's address never goes over the air
	int before = air.ip_calls;
	sent_count = 0;
	from_host(f, host_tcp(f, HOST_IP, IPGW_INDIVIDUAL_NET | 100u, 40000, 80, 1000, 0, 0x02));
	CHECK(air.ip_calls == before && sent_count == 0);
	// A listener: SYN, SYN-ACK, ACK opens the connection; the link going down drops it
	CHECK(tcp_listen(7, &tcp_app));
	sent_count = 0;
	from_host(f, host_tcp(f, HOST_IP, radio_ip, 40001, 7, 5000, 0, 0x02));
	CHECK(sent_count == 1 && sent[0][14 + 20 + 13] == 0x12 && sent_len[0] == 14 + 20 + 24);
	uint32_t iss = get32(sent[0] + 34 + 4);
	from_host(f, host_tcp(f, HOST_IP, radio_ip, 40001, 7, 5001, iss + 1, 0x10));
	CHECK(tcp_opened >= 0 && tcp_state(tcp_opened) == TCP_ESTABLISHED);
	link_up = false;
	hostlink_net_poll(now_ms);
	link_up = true;
	CHECK(tcp_state(tcp_opened) == TCP_CLOSED);
	// Control station mode (docs/spec/usb-network.md 5). Passthrough: active only while the link is up
	CHECK(!hostlink_net_passthrough_active());
	hostlink_net_set_control(true, 0);
	CHECK(hostlink_net_passthrough_active() == (bool)CONFIG_DMR_CONTROL_STATION);
	link_up = false;
	CHECK(!hostlink_net_passthrough_active());
	link_up = true;

	// The fixed link: applied at the next poll; an ETSI IP text to our ID reaches the PC at 192.168.10.2 ...
	hostlink_net_set_control(true, IPGW_FIXED_LINK_DEFAULT);
	hostlink_net_poll(now_ms);
	CHECK(ipGatewayRadioIP() == IPGW_FIXED_LINK_DEFAULT && ipGatewayHostIP() == IPGW_FIXED_LINK_DEFAULT + 1);
	dmrBurst_t tb[DMR_DATA_MAX_BURSTS];
	int tn = dmrDataBuildIPText(false, RADIO_ID, 3101234, "to the dispatcher", DT_RATE_12_DATA, 0, tb, DMR_DATA_MAX_BURSTS);
	const dmrDataPacket_t *rx = NULL;
	dmrDataRxReset();
	for (int i = 0; i < tn; i++) {
		if (dmrDataRxBurst(&tb[i]) == DMR_DATA_RX_PACKET) {
			rx = &dmrDataRxPacket;
		}
	}
	CHECK(rx != NULL);
	sent_count = 0;
	if (rx != NULL) {
		hostlink_net_packet(rx);
	}
	hostlink_net_poll(now_ms);
	ip = sent[0] + 14;
	udp = ip + 20;
	CHECK(sent_count == 1 && memcmp(sent[0], HOST_MAC, 6) == 0 && get32(ip + 16) == IPGW_FIXED_LINK_DEFAULT + 1 &&
	      get32(ip + 12) == (IPGW_INDIVIDUAL_NET | 3101234u) && get16(udp + 2) == DMR_UDP_PORT_ETSI_TEXT);
	// ... and the PC's answer from there goes over the air from the radio's own ID
	static const uint8_t answer[] = { 0x00, 'O', 0x00, 'K' };
	host_src = IPGW_FIXED_LINK_DEFAULT + 1;
	svc_lists = 0;
	from_host(f, host_udp(f, IPGW_INDIVIDUAL_NET | 3101234u, DMR_UDP_PORT_ETSI_TEXT, DMR_UDP_PORT_ETSI_TEXT, answer,
			      sizeof(answer)));
	hostlink_net_air_tick(RADIO_ID, now_ms);
	pk = svc_packet();
	CHECK(svc_lists == 1 && pk != NULL && pk->src == RADIO_ID && pk->dst == 3101234 && !pk->group);
	CHECK(pk != NULL && dmrDataGetUDP(pk, &u) && u.dstPort == DMR_UDP_PORT_ETSI_TEXT && u.length == 4 &&
	      memcmp(u.payload, answer, 4) == 0);
	// The serial protocols at the fixed address
	serial_len = 0;
	from_host(f, host_udp(f, IPGW_FIXED_LINK_DEFAULT, 40123, IPGW_SERIAL_PORT, version_req, sizeof(version_req)));
	CHECK(serial_len == 3);

	// Off: the addressing by DMR ID again
	hostlink_net_set_control(false, 0);
	hostlink_net_poll(now_ms);
	host_src = HOST_IP;
	CHECK(!hostlink_net_passthrough_active() && ipGatewayHostIP() == HOST_IP);

	printf("# hostlinkNetTest: %d checks, %s\n", checks, failures ? "FAILED" : "PASSED");
	return failures != 0;
}
