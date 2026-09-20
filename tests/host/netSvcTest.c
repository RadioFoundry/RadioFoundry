// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The TCP services (core/net: KISS over TCP, HTTP, rigctld, APRS-IS) through the real IP gateway and TCP, against a
// small scripted TCP peer on the host side of a fake USB network interface, with fakes for the APRS engine and the
// radio core. Every frame is written to netSvcTest.pcap for tshark.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "board.h"
#include "hal_time.h"
#include "dmr/ipGateway.h"
#include "net/net.h"
#include "net/tcp.h"
#include "net/net_svc.h"
#include "netSvcFakes.h"

int failures, checks;

#define RADIO_ID	10005u
#define HOST_IP		(IPGW_INDIVIDUAL_NET | RADIO_ID)	// 12.0.39.21
#define RADIO_IP	(HOST_IP ^ 1u)

static const uint8_t HOST_MAC[6] = { 0x02, 0x44, 0x01, 0x02, 0x03, 0x04 };
static const uint8_t RADIO_MAC[6] = { 0x02, 0x44, 0x01, 0x02, 0x03, 0x05 };

// ---- time ----
uint32_t now_ms = 10000;
uint32_t hal_millis(void) { return now_ms; }
void hal_delay_ms(uint32_t ms) { now_ms += ms; }

// ---- pcap ----
static FILE *pcap;
static void pcap_write(const uint8_t *f, int len)
{
	uint32_t rec[4] = { now_ms / 1000, (now_ms % 1000) * 1000, (uint32_t)len, (uint32_t)len };
	fwrite(rec, sizeof(rec), 1, pcap);
	fwrite(f, (size_t)len, 1, pcap);
}

// ---- the fake USB network interface: frames from the radio queue up for the peer ----
#define QMAX 256
static uint8_t q[QMAX][600];
static int qlen[QMAX], qn;
static bool n_select(bool network) { (void)network; return true; }
static bool n_selected(void) { return true; }
static bool n_up(void) { return true; }
static bool n_send(const uint8_t *f, int len)
{
	pcap_write(f, len);
	if (qn < QMAX) {
		memcpy(q[qn], f, (size_t)len);
		qlen[qn++] = len;
	}
	return true;
}
static int n_receive(uint8_t *buf, int max) { (void)buf; (void)max; return 0; }
static void n_tick(uint32_t t) { (void)t; }
static void n_macs(uint8_t host[6], uint8_t radio[6]) { memcpy(host, HOST_MAC, 6); memcpy(radio, RADIO_MAC, 6); }
static const hal_netif_t fake_netif = { n_select, n_selected, n_up, n_send, n_receive, n_tick, n_macs, 600 };
const board_t board = { .name = "fake", .netif = &fake_netif };

// ---- what the gateway needs from its user ----
bool ipGatewaySendFrame(const uint8_t *f, int length) { return board.netif->send(f, length); }
uint32_t ipGatewayRadioId(void) { return RADIO_ID; }
void ipGatewaySerialIn(const uint8_t *data, int length) { (void)data; (void)length; }
bool ipGatewayIPToAir(bool group, uint32_t dst, uint8_t protocol, const uint8_t *l4, int length)
{
	(void)group; (void)dst; (void)protocol; (void)l4; (void)length;
	return false;
}
bool ipGatewayRawToAir(bool group, uint32_t dst, const uint8_t *data, int length)
{
	(void)group; (void)dst; (void)data; (void)length;
	return false;
}

// ---- the scripted peer ----
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)(v >> 16)); put16(p + 2, (uint16_t)v); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t get32(const uint8_t *p) { return ((uint32_t)get16(p) << 16) | get16(p + 2); }

static void peer_segment(peer_t *p, uint8_t flags, const void *data, int len)
{
	uint8_t f[14 + 20 + 20 + 1500];
	memcpy(f, RADIO_MAC, 6);
	memcpy(f + 6, HOST_MAC, 6);
	put16(f + 12, 0x0800);
	uint8_t *ip = f + 14;
	memset(ip, 0, 40);
	ip[0] = 0x45;
	put16(ip + 2, (uint16_t)(40 + len));
	ip[8] = 64;
	ip[9] = 6;
	put32(ip + 12, p->ip);
	put32(ip + 16, RADIO_IP);
	uint32_t sum = 0;
	for (int k = 0; k < 20; k += 2) {
		sum += get16(ip + k);
	}
	while (sum >> 16) {
		sum = (sum & 0xFFFF) + (sum >> 16);
	}
	put16(ip + 10, (uint16_t)~sum);
	uint8_t *t = ip + 20;
	put16(t, p->port);
	put16(t + 2, p->rport);
	put32(t + 4, p->snd_nxt);
	put32(t + 8, (flags & 0x10) ? p->rcv_nxt : 0);
	t[12] = 0x50;
	t[13] = flags;
	put16(t + 14, p->wnd);
	put16(t + 16, 0);
	put16(t + 18, 0);
	if (len > 0) {
		memcpy(t + 20, data, (size_t)len);
	}
	put16(t + 16, tcp_checksum(p->ip, RADIO_IP, t, 20 + len));
	pcap_write(f, 54 + len);
	p->snd_nxt += (uint32_t)len + ((flags & 0x03) ? 1u : 0u);
	ipGatewayEthernetIn(f, 54 + len);
}

static peer_t *peers[8];
static int npeers;

void peer_add(peer_t *p)
{
	for (int i = 0; i < npeers; i++) {
		if (peers[i] == p) {
			return;
		}
	}
	peers[npeers++] = p;
}

// The radio's segments to the peers: data collected and acknowledged, SYN / FIN / RST noted
static bool peer_take(const uint8_t *f, int len)
{
	if (len < 54 || get16(f + 12) != 0x0800 || f[14 + 9] != 6) {
		return false;
	}
	const uint8_t *ip = f + 14, *t = ip + 20;
	uint32_t dst = get32(ip + 16);
	int hdr = (t[12] >> 4) * 4;
	int dlen = get16(ip + 2) - 20 - hdr;
	uint8_t flags = t[13];
	CHECK(tcp_checksum(RADIO_IP, dst, t, hdr + dlen) == 0);
	for (int i = 0; i < npeers; i++) {
		peer_t *p = peers[i];
		if (p->ip != dst || p->port != get16(t + 2)) {
			continue;
		}
		if (p->listening && (flags & 0x02) && !(flags & 0x10)) {
			// The radio's active open: answer SYN-ACK
			p->rport = get16(t);
			p->rcv_nxt = get32(t + 4) + 1;
			p->snd_nxt = 77000;
			p->listening = false;
			p->syns++;
			if (!p->refuse) {
				peer_segment(p, 0x12, NULL, 0);
				p->established = true;
			} else {
				peer_segment(p, 0x14, NULL, 0);
			}
			return true;
		}
		if (p->rport != get16(t)) {
			continue;
		}
		if (flags & 0x04) {
			p->reset = true;
			p->established = false;
			return true;
		}
		if ((flags & 0x02) && (flags & 0x10)) {
			p->rcv_nxt = get32(t + 4) + 1;
			p->established = true;
			peer_segment(p, 0x10, NULL, 0);
			return true;
		}
		uint32_t seq = get32(t + 4);
		bool ack = false;
		if (dlen > 0 && seq == p->rcv_nxt) {
			if (p->rxn + dlen <= (int)sizeof(p->rx)) {
				memcpy(p->rx + p->rxn, t + hdr, (size_t)dlen);
				p->rxn += dlen;
			}
			p->rcv_nxt += (uint32_t)dlen;
			ack = true;
		} else if (dlen > 0) {
			ack = true;
		}
		if ((flags & 0x01) && seq + (uint32_t)dlen == p->rcv_nxt) {
			p->rcv_nxt++;
			p->fin = true;
			ack = true;
		}
		if (ack && !p->no_ack) {
			peer_segment(p, 0x10, NULL, 0);
		}
		return true;
	}
	return false;
}

// Deliver the radio's queued frames to the peers until nothing more comes, polling the services in between
void pump(void)
{
	for (int round = 0; round < 200; round++) {
		net_poll(now_ms, true);
		if (qn == 0) {
			return;
		}
		static uint8_t f[QMAX][600];
		static int fl[QMAX];
		int n = qn;
		memcpy(f, q, sizeof(q[0]) * (size_t)n);
		memcpy(fl, qlen, sizeof(int) * (size_t)n);
		qn = 0;
		for (int i = 0; i < n; i++) {
			peer_take(f[i], fl[i]);
		}
	}
}

void run_for(uint32_t ms)
{
	for (uint32_t t = 0; t < ms; t += 5) {
		now_ms += 5;
		pump();
	}
}

bool peer_connect(peer_t *p, uint32_t ip, uint16_t port, uint16_t rport)
{
	memset(p, 0, sizeof(*p));
	p->ip = ip;
	p->port = port;
	p->rport = rport;
	p->snd_nxt = 1000u * port;
	p->wnd = 8192;
	peer_add(p);
	peer_segment(p, 0x02, NULL, 0);
	pump();
	return p->established;
}

void peer_listen(peer_t *p, uint32_t ip, uint16_t port)
{
	memset(p, 0, sizeof(*p));
	p->ip = ip;
	p->port = port;
	p->wnd = 8192;
	p->listening = true;
	peer_add(p);
}

void peer_send(peer_t *p, const void *data, int len)
{
	peer_segment(p, 0x18, data, len);
	pump();
}

void peer_close(peer_t *p)
{
	peer_segment(p, 0x11, NULL, 0);
	pump();
}

void peer_clear(peer_t *p)
{
	p->rxn = 0;
}

void net_test_start(void)
{
	pcap = fopen("netSvcTest.pcap", "wb");
	uint32_t hdr[6] = { 0xA1B2C3D4u, 0x00040002u, 0, 0, 65535, 1 };
	fwrite(hdr, sizeof(hdr), 1, pcap);
	ipGatewayInit(RADIO_MAC, HOST_MAC);
	net_start(0x01020304u);
}

uint32_t net_test_host_ip(void)
{
	return HOST_IP;
}

int main(void)
{
	net_test_start();
	tests_run();
	fclose(pcap);
	printf("# netSvcTest: %d checks, %d failures\n", checks, failures);
	printf(failures ? "# FAILED\n" : "# PASSED\n");
	return failures ? 1 : 0;
}
