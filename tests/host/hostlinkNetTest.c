// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The host link's network personality (core/hostlink/hostlink_net.c) with the real IP gateway (core/dmr/ipGateway.c)
// and packet codec (core/dmr/dmrData.c), against a fake USB network interface (board.netif) and fake air ops: frames
// from the host reach the air ops, packets from the air reach the host, UDP serial both ways
#include <stdio.h>
#include <string.h>
#include "board.h"
#include "hal_time.h"
#include "hostlink/hostlink_net.h"
#include "dmr/ipGateway.h"
#include "dmr/dmrData.h"

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
static const hal_netif_t fake_netif = { n_select, n_selected, n_up, n_send, n_receive, n_tick, n_macs, 600 };
const board_t board = { .name = "fake", .netif = &fake_netif };

// ---- fake air ops ----
static struct {
	int ip_calls, raw_calls;
	bool group;
	uint32_t dst;
	uint8_t protocol;
	uint8_t data[600];
	int len;
	bool refuse;
} air;
static uint32_t radio_id(void) { return RADIO_ID; }
static bool ip_to_air(bool group, uint32_t dst, uint8_t protocol, const uint8_t *l4, int length)
{
	if (air.refuse) {
		return false;
	}
	air.ip_calls++;
	air.group = group;
	air.dst = dst;
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
	put32(ip + 12, HOST_IP);
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

	printf("# hostlinkNetTest: %d checks, %s\n", checks, failures ? "FAILED" : "PASSED");
	return failures != 0;
}
