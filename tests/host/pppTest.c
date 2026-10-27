// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// PPP on the serial host link (core/net/ppp.c) and its glue to the IP gateway (core/hostlink/hostlink_ppp.c), against a
// scripted peer with its own table driven FCS: framing (escapes, the ACCM both ways, FCS good and bad, frames split
// across reads, back-to-back flags, aborts), LCP (Configure-Ack / -Nak / -Reject both ways, Max-Failure, the magic
// number, Echo, Code-Reject, Protocol-Reject of IPv6CP, Terminate both ways), IPCP address assignment, the ways a link
// ends (dead peer, negotiation timing out, another protocol's request), and IPv4 through the real gateway: ping, UDP
// serial, UDP to a DMR ID and back from the air. Then a seeded fuzz loop over whole sessions and over single frames.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fuzz.h"
#include "board.h"
#include "net/ppp.h"
#include "hostlink/hostlink_ppp.h"
#include "dmr/ipGateway.h"
#include "dmr/dmrData.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define RADIO_ID	10005u
#define HOST_IP		(IPGW_INDIVIDUAL_NET | RADIO_ID)	// 12.0.39.21
#define RADIO_IP	(HOST_IP ^ 1u)

enum { CONF_REQ = 1, CONF_ACK, CONF_NAK, CONF_REJ, TERM_REQ, TERM_ACK, CODE_REJ, PROTO_REJ, ECHO_REQ, ECHO_REP };
#define LCP	0xC021
#define IPCP	0x8021
#define IPV6CP	0x8057
#define IP	0x0021

static uint32_t now_ms = 1000;
static fuzz_t rng;
static uint8_t *rxbuf;		// PPP's receive buffer, exactly PPP_RX_BUFFER bytes on the heap (ASan sees overruns)
uint8_t *hostlink_net_frame(void) { return rxbuf; }

// ---- the radio's side: the serial port, the gateway's user ----

static uint8_t out[1 << 16];
static size_t out_len, out_pos;
static bool quiet;		// the fuzz loop: output decoded and checked, no frame kept

static size_t port_write(const uint8_t *b, size_t n)
{
	if (out_len + n > sizeof(out)) {
		out_len = out_pos = 0;	// the fuzz loop never reads some of it
	}
	memcpy(out + out_len, b, n);
	out_len += n;
	return n;
}
static bool port_true(void) { return true; }
static size_t port_read(uint8_t *b, size_t n) { (void)b; (void)n; return 0; }
static bool port_wait(uint32_t t) { (void)t; return false; }
static const hal_stream_t port = { port_true, port_true, port_read, port_wait, port_write };
const board_t board = { .name = "fake", .host_port = &port, .netif = &hostlink_ppp_netif };

bool ipGatewaySendFrame(const uint8_t *f, int length) { return board.netif->send(f, length); }
uint32_t ipGatewayRadioId(void) { return RADIO_ID; }

static uint8_t serial_in[600];
static int serial_in_len;
void ipGatewaySerialIn(const uint8_t *data, int length)
{
	memcpy(serial_in, data, (size_t)length);
	serial_in_len = length;
}

static uint32_t air_dst;
static uint8_t air_proto;
static int air_len, air_count;
bool ipGatewayIPToAir(bool group, uint32_t dst, uint32_t srcIp, uint32_t dstIp, uint8_t ttl, uint8_t protocol, const uint8_t *l4, int length)
{
	(void)group; (void)l4;
	air_dst = dst;
	air_proto = protocol;
	air_len = length;
	air_count++;
	return true;
}
bool ipGatewayRawToAir(bool group, uint32_t dst, const uint8_t *data, int length)
{
	(void)group; (void)dst; (void)data; (void)length;
	return true;
}

// The host link's other protocols: the debug link (0xA5), MMDVM (0xE0), CPS letters
bool ppp_foreign_start(uint8_t b)
{
	return (b == 0xA5) || (b == 0xE0) || ((b >= 'A') && (b <= 'Z'));
}

// ---- the scripted peer ----

static uint16_t fcstab[256];
static uint32_t peer_accm;	// the control characters the peer escapes outside LCP
static uint32_t radio_magic;

static void fcs_init(void)
{
	for (int b = 0; b < 256; b++) {
		uint16_t v = (uint16_t)b;

		for (int i = 0; i < 8; i++) {
			v = (v & 1) ? (uint16_t)((v >> 1) ^ 0x8408) : (uint16_t)(v >> 1);
		}
		fcstab[b] = v;
	}
}

static uint16_t fcs(uint16_t f, const uint8_t *p, int n)
{
	while (n-- > 0) {
		f = (uint16_t)((f >> 8) ^ fcstab[(f ^ *p++) & 0xFF]);
	}
	return f;
}

static uint16_t get16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t get32(const uint8_t *p) { return ((uint32_t)get16(p) << 16) | get16(p + 2); }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)(v >> 16)); put16(p + 2, (uint16_t)v); }

// A whole frame, flags included: the encoded length
static int encode(uint8_t *e, uint16_t proto, const uint8_t *info, int n, uint32_t accm)
{
	uint8_t raw[2048];
	int k = 0, m = 0;

	raw[m++] = 0xFF;
	raw[m++] = 0x03;
	put16(raw + m, proto);
	m += 2;
	memcpy(raw + m, info, (size_t)n);
	m += n;
	uint16_t f = (uint16_t)~fcs(0xFFFF, raw, m);
	raw[m++] = (uint8_t)f;
	raw[m++] = (uint8_t)(f >> 8);
	e[k++] = 0x7E;
	for (int i = 0; i < m; i++) {
		uint8_t b = raw[i];

		if ((b == 0x7E) || (b == 0x7D) || ((b < 0x20) && ((accm >> b) & 1))) {
			e[k++] = 0x7D;
			b ^= 0x20;
		}
		e[k++] = b;
	}
	e[k++] = 0x7E;
	return k;
}

// Bytes to the radio in random pieces; all of them must be taken
static void feed(const uint8_t *b, int n)
{
	while (n > 0) {
		int piece = 1 + (int)fuzz_below(&rng, 40);

		if (piece > n) {
			piece = n;
		}
		CHECK(ppp_input(b, (size_t)piece, now_ms) == (size_t)piece);
		b += piece;
		n -= piece;
	}
}

static void peer_send(uint16_t proto, const uint8_t *info, int n)
{
	uint8_t e[4200];

	feed(e, encode(e, proto, info, n, (proto == LCP) ? 0xFFFFFFFFu : peer_accm));
}

static void peer_ctl(uint16_t proto, uint8_t code, uint8_t id, const uint8_t *data, int n)
{
	uint8_t p[1600];

	p[0] = code;
	p[1] = id;
	put16(p + 2, (uint16_t)(4 + n));
	if (n > 0) memcpy(p + 4, data, (size_t)n);	// data may be NULL for an empty packet
	peer_send(proto, p, 4 + n);
}

// The radio's next frame: protocol and information field; false when none. Every frame must have a good FCS and
// the full address and control fields; bare is set when an unescaped control character was in it
static bool bare;
static bool radio_frame(uint16_t *proto, uint8_t *info, int *n)
{
	uint8_t raw[2048];
	int m = 0;
	bool esc = false, in = false;

	bare = false;
	while (out_pos < out_len) {
		uint8_t b = out[out_pos++];

		if (b == 0x7E) {
			if (in && (m > 0)) {
				CHECK(!esc);
				CHECK(m >= 6);
				CHECK(fcs(0xFFFF, raw, m) == 0xF0B8);
				CHECK((raw[0] == 0xFF) && (raw[1] == 0x03));
				*proto = get16(raw + 2);
				*n = m - 6;
				memcpy(info, raw + 4, (size_t)*n);
				return true;
			}
			in = true;
			m = 0;
			continue;
		}
		CHECK(in);
		if (b == 0x7D) {
			esc = true;
			continue;
		}
		if (b < 0x20) {
			bare = true;
		}
		if (esc) {
			b ^= 0x20;
			esc = false;
		}
		if (m < (int)sizeof(raw)) {
			raw[m++] = b;
		}
	}
	return false;
}

static void drain(void)
{
	uint16_t p;
	uint8_t i[2048];
	int n;

	while (radio_frame(&p, i, &n)) {
	}
}

// The next frame must be a control packet of this protocol and code: its id, data in d (n bytes)
static bool expect(uint16_t proto, uint8_t code, uint8_t *id, uint8_t *d, int *n)
{
	uint16_t p;
	uint8_t i[2048];
	int m;

	if (!radio_frame(&p, i, &m)) {
		printf("# expected %04x code %u, nothing came\n", proto, code);
		return false;
	}
	if ((p != proto) || (m < 4) || (i[0] != code) || (get16(i + 2) != m)) {
		printf("# expected %04x code %u, got %04x code %u len %d\n", proto, code, p, m > 0 ? i[0] : 0, m);
		return false;
	}
	if (id != NULL) {
		*id = i[1];
	}
	if (d != NULL) {
		memcpy(d, i + 4, (size_t)(m - 4));
		*n = m - 4;
	}
	return true;
}

static int opt32(uint8_t *o, uint8_t type, uint32_t v) { o[0] = type; o[1] = 6; put32(o + 2, v); return 6; }
static int opt16(uint8_t *o, uint8_t type, uint16_t v) { o[0] = type; o[1] = 4; put16(o + 2, v); return 4; }

// The option of this type in a request's data, NULL if absent
static const uint8_t *find_opt(const uint8_t *d, int n, uint8_t type)
{
	for (int i = 0; i + 2 <= n && d[i + 1] >= 2; i += d[i + 1]) {
		if (d[i] == type) {
			return d + i;
		}
	}
	return NULL;
}

// A passive start and the whole negotiation, as pppd does it; IPCP open at the end
static void open_link(void)
{
	uint8_t d[64], id;
	int n, k;

	out_len = out_pos = 0;
	ppp_start(false, 77, now_ms, rxbuf);
	peer_accm = 0;
	k = opt16(d, 1, 1500);
	k += opt32(d + k, 2, 0);
	k += opt32(d + k, 5, 0x11223344u);
	peer_ctl(LCP, CONF_REQ, 1, d, k);
	CHECK(expect(LCP, CONF_REQ, &id, d, &n));
	CHECK(find_opt(d, n, 5) != NULL);
	radio_magic = get32(find_opt(d, n, 5) + 2);
	uint8_t req[64];
	int rn = n;

	memcpy(req, d, (size_t)n);
	CHECK(expect(LCP, CONF_ACK, NULL, d, &n));
	peer_ctl(LCP, CONF_ACK, id, req, rn);
	CHECK(expect(IPCP, CONF_REQ, &id, d, &n));
	CHECK((n == 6) && (d[0] == 3) && (get32(d + 2) == RADIO_IP));
	peer_ctl(IPCP, CONF_ACK, id, d, n);
	k = opt32(d, 3, HOST_IP);
	peer_ctl(IPCP, CONF_REQ, 1, d, k);
	CHECK(expect(IPCP, CONF_ACK, NULL, d, &n));
	CHECK(ppp_ip_up());
}

// ---- IPv4 ----

static uint16_t csum(const uint8_t *p, int n, uint32_t sum)
{
	for (; n > 1; p += 2, n -= 2) {
		sum += get16(p);
	}
	if (n) {
		sum += (uint32_t)p[0] << 8;
	}
	while (sum >> 16) {
		sum = (sum & 0xFFFF) + (sum >> 16);
	}
	return (uint16_t)~sum;
}

static int ipv4(uint8_t *p, uint8_t proto, uint32_t src, uint32_t dst, const uint8_t *l4, int n)
{
	memset(p, 0, 20);
	p[0] = 0x45;
	put16(p + 2, (uint16_t)(20 + n));
	p[8] = 64;
	p[9] = proto;
	put32(p + 12, src);
	put32(p + 16, dst);
	put16(p + 10, csum(p, 20, 0));
	memcpy(p + 20, l4, (size_t)n);
	return 20 + n;
}

static int udp(uint8_t *p, uint32_t src, uint32_t dst, uint16_t sport, uint16_t dport, const void *data, int n)
{
	uint8_t u[1600];

	put16(u, sport);
	put16(u + 2, dport);
	put16(u + 4, (uint16_t)(8 + n));
	put16(u + 6, 0);
	memcpy(u + 8, data, (size_t)n);
	uint32_t ps = (src >> 16) + (src & 0xFFFF) + (dst >> 16) + (dst & 0xFFFF) + 17 + (uint32_t)(8 + n);

	put16(u + 6, csum(u, 8 + n, ps));
	return ipv4(p, 17, src, dst, u, 8 + n);
}

// ---- the cases ----

static void test_fcs(void)
{
	// The FCS-16 check value (CRC-16/X-25 of "123456789"): the peer's table is right
	CHECK((uint16_t)~fcs(0xFFFF, (const uint8_t *)"123456789", 9) == 0x906E);
}

static void test_negotiation(void)
{
	uint8_t d[64], id, rid;
	int n, k;

	out_len = out_pos = 0;
	ppp_start(false, 1, now_ms, rxbuf);
	CHECK(ppp_running() && !ppp_ip_up());
	CHECK(out_len == 0);	// passive: nothing until the peer speaks

	// pppd's first request with PFC, ACFC and an authentication protocol: rejected, and our own request goes out
	k = opt32(d, 2, 0);
	k += opt32(d + k, 5, 0x0BADCAFEu);
	d[k++] = 7; d[k++] = 2;		// PFC
	d[k++] = 8; d[k++] = 2;		// ACFC
	d[k++] = 3; d[k++] = 4; d[k++] = 0xC0; d[k++] = 0x23;	// PAP
	peer_ctl(LCP, CONF_REQ, 1, d, k);
	CHECK(expect(LCP, CONF_REQ, &rid, d, &n));
	CHECK(find_opt(d, n, 1) && get16(find_opt(d, n, 1) + 2) == PPP_MRU);
	CHECK(find_opt(d, n, 2) && get32(find_opt(d, n, 2) + 2) == 0);
	CHECK(find_opt(d, n, 5) != NULL);
	radio_magic = get32(find_opt(d, n, 5) + 2);
	CHECK(expect(LCP, CONF_REJ, &id, d, &n));
	CHECK(id == 1);
	CHECK((n == 8) && (d[0] == 7) && (d[2] == 8) && (d[4] == 3));

	// The magic number of the radio itself: a looped-back line, Nak'd with another one
	k = opt32(d, 5, radio_magic);
	peer_ctl(LCP, CONF_REQ, 2, d, k);
	CHECK(expect(LCP, CONF_NAK, &id, d, &n));
	CHECK((id == 2) && (n == 6) && (get32(d + 2) != radio_magic) && (get32(d + 2) != 0));

	// The peer Naks our ACCM and rejects our magic number: the next request has its map and no magic number
	k = opt32(d, 2, 0x000A0000u);
	peer_ctl(LCP, CONF_NAK, rid, d, k);
	CHECK(expect(LCP, CONF_REQ, &rid, d, &n));
	CHECK(find_opt(d, n, 2) && get32(find_opt(d, n, 2) + 2) == 0x000A0000u);
	k = opt32(d, 5, radio_magic);
	peer_ctl(LCP, CONF_REJ, rid, d, k);
	CHECK(expect(LCP, CONF_REQ, &rid, d, &n));
	CHECK(find_opt(d, n, 5) == NULL);
	CHECK(find_opt(d, n, 1) != NULL);
	uint8_t req[64];
	int rn = n;

	memcpy(req, d, (size_t)n);

	// A stale Ack (wrong id) is ignored
	peer_ctl(LCP, CONF_ACK, (uint8_t)(rid - 1), req, rn);
	CHECK(out_pos == out_len);

	// A good request now: Ack; then the peer's Ack opens LCP and IPCP starts
	k = opt16(d, 1, 1500);
	k += opt32(d + k, 2, 0);
	k += opt32(d + k, 5, 0x0BADCAFEu);
	peer_ctl(LCP, CONF_REQ, 3, d, k);
	CHECK(expect(LCP, CONF_ACK, &id, d, &n));
	CHECK((id == 3) && (n == k));
	peer_ctl(LCP, CONF_ACK, rid, req, rn);
	CHECK(expect(IPCP, CONF_REQ, &rid, d, &n));
	CHECK((n == 6) && (d[0] == 3) && (get32(d + 2) == RADIO_IP));
	ppp_stats_t s;

	ppp_stats(&s);
	CHECK((s.lcp == PPP_OPENED) && (s.ipcp == PPP_REQ_SENT));

	// IPCP: VJ compression and the DNS options rejected, 0.0.0.0 Nak'd to the host's address, then Ack'd
	peer_accm = 0x000A0000u;
	k = opt32(d, 3, 0);
	d[k++] = 2; d[k++] = 6; d[k++] = 0x00; d[k++] = 0x2D; d[k++] = 15; d[k++] = 1;	// VJ
	k += opt32(d + k, 129, 0);
	peer_ctl(IPCP, CONF_REQ, 1, d, k);
	CHECK(expect(IPCP, CONF_REJ, &id, d, &n));
	CHECK((id == 1) && (n == 12) && (d[0] == 2) && (d[6] == 129));
	k = opt32(d, 3, 0);
	peer_ctl(IPCP, CONF_REQ, 2, d, k);
	CHECK(expect(IPCP, CONF_NAK, &id, d, &n));
	CHECK((id == 2) && (n == 6) && (get32(d + 2) == HOST_IP));
	peer_ctl(IPCP, CONF_REQ, 3, d, n);
	CHECK(expect(IPCP, CONF_ACK, &id, d, &n));
	CHECK(!ppp_ip_up());	// our request isn't Ack'd yet
	k = opt32(d, 3, RADIO_IP);
	peer_ctl(IPCP, CONF_ACK, rid, d, k);
	CHECK(ppp_ip_up());
	CHECK(board.netif->up() && board.netif->selected());
	ppp_stats(&s);
	CHECK((s.lcp == PPP_OPENED) && (s.ipcp == PPP_OPENED));
}

static void test_max_failure(void)
{
	uint8_t d[64], id;
	int n, k;

	open_link();
	// A peer that insists on its own address: Nak'd PPP_MAX_FAILURE times, then rejected
	for (int i = 0; i < PPP_MAX_FAILURE + 1; i++) {
		k = opt32(d, 3, 0x01020304u);
		peer_ctl(IPCP, CONF_REQ, (uint8_t)(10 + i), d, k);
		if (i == 0) {
			CHECK(expect(IPCP, CONF_REQ, &id, d, &n));	// IPCP was open: our own request goes again first
			CHECK(!ppp_ip_up());
		}
		CHECK(expect(IPCP, (i < PPP_MAX_FAILURE) ? CONF_NAK : CONF_REJ, &id, d, &n));
		CHECK((id == 10 + i) && (n == 6));
		CHECK((i == PPP_MAX_FAILURE) ? (get32(d + 2) == 0x01020304u) : (get32(d + 2) == HOST_IP));
		drain();
	}
	CHECK(!ppp_ip_up());
}

static void test_echo_and_rejects(void)
{
	uint8_t d[64], id;
	int n;

	open_link();
	// Echo-Request: the same data back with our magic number
	put32(d, 0x11223344u);
	memcpy(d + 4, "ping", 4);
	peer_ctl(LCP, ECHO_REQ, 9, d, 8);
	CHECK(expect(LCP, ECHO_REP, &id, d, &n));
	CHECK((id == 9) && (n == 8) && (get32(d) == radio_magic) && (memcmp(d + 4, "ping", 4) == 0));

	// IPv6CP: Protocol-Rejected, the rejected protocol and its packet inside
	uint8_t v6[8] = { 1, 1, 0, 8, 1, 4, 0xAA, 0xBB };
	peer_send(IPV6CP, v6, 8);
	CHECK(expect(LCP, PROTO_REJ, NULL, d, &n));
	CHECK((n == 10) && (get16(d) == IPV6CP) && (memcmp(d + 2, v6, 8) == 0));

	// An unknown LCP code: Code-Rejected with the packet inside
	peer_ctl(LCP, 0x0E, 4, (const uint8_t *)"xy", 2);
	CHECK(expect(LCP, CODE_REJ, NULL, d, &n));
	CHECK((n == 6) && (d[0] == 0x0E) && (d[1] == 4));
	CHECK(ppp_ip_up());

	// Echo-Reply and Discard-Request are taken quietly; a short or overlong length field is discarded
	peer_ctl(LCP, ECHO_REP, 5, d, 4);
	peer_ctl(LCP, 11, 5, d, 4);
	uint8_t bad[6] = { ECHO_REQ, 1, 0, 40, 0, 0 };
	peer_send(LCP, bad, 6);
	CHECK(out_pos == out_len);
}

static void test_ipv4(void)
{
	uint8_t pkt[1600], d[2048];
	uint16_t proto;
	int n;

	open_link();
	// Ping the radio
	uint8_t icmp[16] = { 8, 0, 0, 0, 0x12, 0x34, 0, 1, 'a', 'b', 'c', 'd', 0x7E, 0x7D, 0x11, 0x13 };
	put16(icmp + 2, csum(icmp, 16, 0));
	n = ipv4(pkt, 1, HOST_IP, RADIO_IP, icmp, 16);
	peer_send(IP, pkt, n);
	CHECK(radio_frame(&proto, d, &n));
	CHECK(bare);	// the peer asked for ACCM 0: its control characters go bare (0x7E and 0x7D escaped, or nothing parses)
	CHECK((proto == IP) && (n == 36));
	CHECK((get32(d + 12) == RADIO_IP) && (get32(d + 16) == HOST_IP) && (d[9] == 1) && (d[20] == 0));
	CHECK(csum(d, 20, 0) == 0);
	CHECK(csum(d + 20, 16, 0) == 0);
	CHECK(memcmp(d + 28, icmp + 8, 8) == 0);

	// UDP serial (the debug link's way in network mode): in, and the answer back to the sender's port
	n = udp(pkt, HOST_IP, RADIO_IP, 50000, IPGW_SERIAL_PORT, "\xA5\x5A\x01", 3);
	peer_send(IP, pkt, n);
	CHECK((serial_in_len == 3) && (serial_in[0] == 0xA5));
	CHECK(ipGatewaySerialOut((const uint8_t *)"reply", 5));
	CHECK(radio_frame(&proto, d, &n));
	CHECK((proto == IP) && (get16(d + 20) == IPGW_SERIAL_PORT) && (get16(d + 22) == 50000) &&
	      (memcmp(d + 28, "reply", 5) == 0));

	// UDP to a DMR ID goes over the air; a datagram from the air comes back over PPP
	air_count = 0;
	n = udp(pkt, HOST_IP, IPGW_INDIVIDUAL_NET | 5u, 4001, 4001, "hello", 5);
	peer_send(IP, pkt, n);
	CHECK((air_count == 1) && (air_dst == 5) && (air_proto == 17) && (air_len == 13));
	CHECK(ipGatewayDeliverUDP(false, RADIO_ID, 5, 0x0C000005u, 0x0C000000u | RADIO_ID, 64, 4001, 4001, (const uint8_t *)"back", 4));
	CHECK(radio_frame(&proto, d, &n));
	CHECK((proto == IP) && (get32(d + 12) == (IPGW_INDIVIDUAL_NET | 5u)) && (get32(d + 16) == HOST_IP));

	// Nothing that isn't IPv4 leaves through the netif; a packet longer than the peer's MRU is dropped
	uint8_t arp[42] = { 0 };
	arp[12] = 0x08; arp[13] = 0x06;
	CHECK(board.netif->send(arp, 42));
	CHECK(out_pos == out_len);
	CHECK(board.netif->receive(d, 600) == 0);
}

// Our MRU (RFC 1661 6.1): PPP_MRU, what the gateway's frames take. A Nak to a smaller one (not under PPP_MIN_MRU) is
// asked for next; a bigger one isn't (our buffer can't take it); a Reject drops the option (the peer may then send
// 1500, and longer frames are dropped as too long). ppp_stats reports both directions
static void test_mru(void)
{
	uint8_t d[64], id, rid;
	int n, k;
	ppp_stats_t s;

	out_len = out_pos = 0;
	ppp_start(false, 5, now_ms, rxbuf);
	k = opt16(d, 1, 1000);
	peer_ctl(LCP, CONF_REQ, 1, d, k);
	CHECK(expect(LCP, CONF_REQ, &rid, d, &n));
	CHECK(find_opt(d, n, 1) && get16(find_opt(d, n, 1) + 2) == PPP_MRU);
	CHECK(expect(LCP, CONF_ACK, &id, d, &n));
	ppp_stats(&s);
	CHECK((s.mru == PPP_MRU) && (s.peer_mru == 1000));

	k = opt16(d, 1, 296);
	peer_ctl(LCP, CONF_NAK, rid, d, k);
	CHECK(expect(LCP, CONF_REQ, &rid, d, &n));
	CHECK(find_opt(d, n, 1) && get16(find_opt(d, n, 1) + 2) == 296);
	k = opt16(d, 1, PPP_MRU + 1);	// more than a frame of ours holds: ours again
	peer_ctl(LCP, CONF_NAK, rid, d, k);
	CHECK(expect(LCP, CONF_REQ, &rid, d, &n));
	CHECK(find_opt(d, n, 1) && get16(find_opt(d, n, 1) + 2) == 296);
	k = opt16(d, 1, PPP_MIN_MRU - 1);
	peer_ctl(LCP, CONF_NAK, rid, d, k);
	CHECK(expect(LCP, CONF_REQ, &rid, d, &n));
	CHECK(find_opt(d, n, 1) && get16(find_opt(d, n, 1) + 2) == 296);
	peer_ctl(LCP, CONF_ACK, rid, d, n);
	CHECK(expect(IPCP, CONF_REQ, &rid, d, &n));
	ppp_stats(&s);
	CHECK((s.lcp == PPP_OPENED) && (s.mru == 296) && (s.peer_mru == 1000));

	// A new link starts from PPP_MRU again; a Reject of the option: not asked for, the peer may send the default
	out_len = out_pos = 0;
	ppp_start(true, 6, now_ms, rxbuf);
	CHECK(expect(LCP, CONF_REQ, &rid, d, &n));
	CHECK(find_opt(d, n, 1) && get16(find_opt(d, n, 1) + 2) == PPP_MRU);
	k = opt16(d, 1, PPP_MRU);
	peer_ctl(LCP, CONF_REJ, rid, d, k);
	CHECK(expect(LCP, CONF_REQ, &rid, d, &n));
	CHECK(find_opt(d, n, 1) == NULL);
	peer_ctl(LCP, CONF_ACK, rid, d, n);
	peer_ctl(LCP, CONF_REQ, 1, NULL, 0);
	CHECK(expect(LCP, CONF_ACK, &id, d, &n));
	ppp_stats(&s);
	CHECK((s.lcp == PPP_OPENED) && (s.mru == 1500) && (s.peer_mru == 1500));
}

// The air MTU on the PPP path (the same ipGatewayEthernetIn; usb-network.md 3.2): the longest datagram PPP takes
// (PPP_MRU, below the link's MTU) with the full header, DF set, goes to the air whole, and nothing comes back
static void test_air_mtu(void)
{
	static uint8_t payload[PPP_MRU];
	uint8_t pkt[1600];

	open_link();
	air_count = 0;
	memset(payload, 0x5A, sizeof(payload));
	int n = udp(pkt, HOST_IP, IPGW_INDIVIDUAL_NET | 5u, 4001, 4001, payload, PPP_MRU - 28);	// the full header
	pkt[6] = 0x40;	// DF
	put16(pkt + 10, 0);
	put16(pkt + 10, csum(pkt, 20, 0));
	peer_send(IP, pkt, n);
	CHECK((n == PPP_MRU) && (PPP_MRU <= IPGW_LINK_MTU) && (air_count == 1) && (air_len == PPP_MRU - 20));
	CHECK(out_pos == out_len);
}

static void test_accm(void)
{
	uint8_t pkt[200], d[2048], id;
	uint16_t proto;
	int n, k;

	// The peer leaves its ACCM out (all control characters escaped towards it) and rejects ours (it escapes everything,
	// so bare control characters from it are line noise and dropped)
	out_len = out_pos = 0;
	peer_accm = 0xFFFFFFFFu;
	ppp_start(true, 5, now_ms, rxbuf);
	CHECK(expect(LCP, CONF_REQ, &id, d, &n));
	k = opt32(d, 2, 0);
	peer_ctl(LCP, CONF_REJ, id, d, k);
	CHECK(expect(LCP, CONF_REQ, &id, d, &n));
	CHECK(find_opt(d, n, 2) == NULL);
	uint8_t req[64];
	int rn = n;

	memcpy(req, d, (size_t)n);
	peer_ctl(LCP, CONF_ACK, id, req, rn);
	k = opt16(d, 1, 1500);
	peer_ctl(LCP, CONF_REQ, 1, d, k);
	CHECK(expect(LCP, CONF_ACK, NULL, d, &n));
	CHECK(expect(IPCP, CONF_REQ, &id, d, &n));
	peer_ctl(IPCP, CONF_ACK, id, d, n);
	k = opt32(d, 3, HOST_IP);
	peer_accm = 0xFFFFFFFFu;
	peer_ctl(IPCP, CONF_REQ, 1, d, k);
	CHECK(expect(IPCP, CONF_ACK, NULL, d, &n));
	CHECK(ppp_ip_up());

	uint8_t icmp[12] = { 8, 0, 0, 0, 0, 1, 0, 1, 0x01, 0x02, 0x1F, 0x20 };
	put16(icmp + 2, csum(icmp, 12, 0));
	n = ipv4(pkt, 1, HOST_IP, RADIO_IP, icmp, 12);
	// Line noise: bare XON / XOFF in the frame are dropped by the radio
	uint8_t e[400], noisy[420];
	int en = encode(e, IP, pkt, n, 0xFFFFFFFFu), m = 0;

	for (int i = 0; i < en; i++) {
		noisy[m++] = e[i];
		if (i == en / 2) {
			noisy[m++] = 0x11;
			noisy[m++] = 0x13;
		}
	}
	feed(noisy, m);
	CHECK(radio_frame(&proto, d, &n));
	CHECK((proto == IP) && !bare);	// default ACCM towards the peer: control characters escaped
	CHECK((n == 32) && (d[28] == 0x01) && (d[29] == 0x02));
}

static void test_framing(void)
{
	uint8_t pkt[200], e[600], d[2048];
	uint16_t proto;
	int n, en;
	ppp_stats_t s0, s1;

	open_link();
	uint8_t icmp[8] = { 8, 0, 0, 0, 0, 7, 0, 1 };
	put16(icmp + 2, csum(icmp, 8, 0));
	n = ipv4(pkt, 1, HOST_IP, RADIO_IP, icmp, 8);
	en = encode(e, IP, pkt, n, 0);

	// Byte by byte, then back-to-back flags and two frames sharing one flag
	for (int i = 0; i < en; i++) {
		CHECK(ppp_input(e + i, 1, now_ms) == 1);
	}
	CHECK(radio_frame(&proto, d, &n) && (proto == IP));
	uint8_t two[1300];
	int m = 0;

	two[m++] = 0x7E;
	two[m++] = 0x7E;
	memcpy(two + m, e, (size_t)en);
	m += en;
	memcpy(two + m, e + 1, (size_t)(en - 1));	// shares the closing flag of the first
	m += en - 1;
	two[m++] = 0x7E;
	feed(two, m);
	CHECK(radio_frame(&proto, d, &n) && (proto == IP));
	CHECK(radio_frame(&proto, d, &n) && (proto == IP));
	CHECK(!radio_frame(&proto, d, &n));

	// Bad FCS, an aborted frame (0x7D 0x7E), a frame without address and control (ACFC was rejected), and one longer
	// than the MRU: all dropped and counted
	ppp_stats(&s0);
	memcpy(two, e, (size_t)en);
	two[en - 3] ^= 0x01;
	feed(two, en);
	uint8_t abort_seq[] = { 0x7E, 0xFF, 0x03, 0x00, 0x21, 0x45, 0x7D, 0x7E };
	feed(abort_seq, sizeof(abort_seq));
	uint8_t raw[1600];
	memcpy(raw, pkt, (size_t)n);
	uint8_t noacfc[300];
	int nn = 0;
	{
		uint8_t f[300];
		int fm = 0;

		f[fm++] = 0x00;
		f[fm++] = 0x21;
		memcpy(f + fm, pkt, 28);
		fm += 28;
		uint16_t c = (uint16_t)~fcs(0xFFFF, f, fm);
		f[fm++] = (uint8_t)c;
		f[fm++] = (uint8_t)(c >> 8);
		noacfc[nn++] = 0x7E;
		for (int i = 0; i < fm; i++) {
			if ((f[i] == 0x7E) || (f[i] == 0x7D) || (f[i] < 0x20)) {
				noacfc[nn++] = 0x7D;
				noacfc[nn++] = f[i] ^ 0x20;
			} else {
				noacfc[nn++] = f[i];
			}
		}
		noacfc[nn++] = 0x7E;
	}
	feed(noacfc, nn);
	uint8_t big[1200];
	memset(big, 0x41, sizeof(big));
	uint8_t eb[2600];
	feed(eb, encode(eb, IP, big, PPP_MRU + 1, 0));
	ppp_stats(&s1);
	CHECK(s1.rx_errors == s0.rx_errors + 4);
	CHECK(out_pos == out_len);
	CHECK(ppp_ip_up());

	// A frame that stops half way and resumes after the gap: dropped, the next frame is fine
	CHECK(ppp_input(e, (size_t)(en / 2), now_ms) == (size_t)(en / 2));
	now_ms += 600;
	CHECK(ppp_input(e + en / 2, (size_t)(en - en / 2), now_ms) == (size_t)(en - en / 2));
	CHECK(!radio_frame(&proto, d, &n));
	feed(e, en);
	CHECK(radio_frame(&proto, d, &n) && (proto == IP));
}

static void test_terminate(void)
{
	uint8_t d[64], id;
	int n;

	// The peer terminates: Terminate-Ack, IP down at once, finished one restart time later
	open_link();
	peer_ctl(LCP, TERM_REQ, 33, NULL, 0);
	CHECK(expect(LCP, TERM_ACK, &id, d, &n));
	CHECK(id == 33);
	CHECK(ppp_running() && !ppp_ip_up() && !board.netif->up());
	now_ms += PPP_RESTART_MS;
	ppp_poll(now_ms);
	CHECK(!ppp_running() && !board.netif->selected());

	// We terminate (the debug link's PPP op): Terminate-Request, finished on the peer's Terminate-Ack
	open_link();
	ppp_close(now_ms);
	CHECK(expect(LCP, TERM_REQ, &id, d, &n));
	CHECK(ppp_running() && !ppp_ip_up());
	peer_ctl(LCP, TERM_ACK, id, NULL, 0);
	CHECK(!ppp_running());

	// ... or after PPP_MAX_TERMINATE requests without an answer
	open_link();
	ppp_close(now_ms);
	for (int i = 0; i < PPP_MAX_TERMINATE; i++) {
		CHECK(expect(LCP, TERM_REQ, &id, d, &n));
		now_ms += PPP_RESTART_MS;
		ppp_poll(now_ms);
	}
	CHECK(!ppp_running());
}

static void test_leaving(void)
{
	uint8_t d[64], e[64], id;
	int n;

	// Active start with nobody there: PPP_MAX_CONFIGURE requests, PPP_RESTART_MS apart, then finished
	out_len = out_pos = 0;
	ppp_start(true, 9, now_ms, rxbuf);
	for (int i = 0; i < PPP_MAX_CONFIGURE; i++) {
		CHECK(expect(LCP, CONF_REQ, &id, d, &n));
		now_ms += PPP_RESTART_MS;
		ppp_poll(now_ms);
	}
	CHECK(!ppp_running());

	// Another protocol's request where a frame starts: the bytes from there on are not PPP's
	open_link();
	uint8_t mixed[] = { 0x7E, 0x7E, 0xA5, 0x5A, 0x01 };
	CHECK(ppp_input(mixed, sizeof(mixed), now_ms) == 2);
	CHECK(!ppp_running());
	open_link();
	CHECK(ppp_input((const uint8_t *)"PSEARCH", 7, now_ms) == 0);	// the open-firmware CPS after pppd went away
	CHECK(!ppp_running());
	// ... but not inside a frame, and not an LCP frame's first byte
	open_link();
	int en = encode(e, LCP, (const uint8_t *)"\x0B\x01\x00\x08\x50\x50\x50\x50", 8, 0xFFFFFFFFu);
	feed(e, en);
	CHECK(ppp_running());

	// A dead peer: Echo-Requests after PPP_ECHO_MS of quiet, finished after PPP_DEAD_MS
	open_link();
	int echoes = 0;
	for (uint32_t t = 0; t <= PPP_DEAD_MS; t += 1000) {
		now_ms += 1000;
		ppp_poll(now_ms);
		uint16_t p;
		uint8_t i[64];
		while (radio_frame(&p, i, &n)) {
			echoes += (p == LCP) && (i[0] == ECHO_REQ);
		}
	}
	CHECK(echoes >= 3);
	CHECK(!ppp_running());

	// A live peer answering the echoes keeps the link
	open_link();
	for (uint32_t t = 0; t <= 2 * PPP_DEAD_MS; t += 1000) {
		now_ms += 1000;
		ppp_poll(now_ms);
		uint16_t p;
		uint8_t i[64];
		while (radio_frame(&p, i, &n)) {
			if ((p == LCP) && (i[0] == ECHO_REQ)) {
				put32(d, 0x11223344u);
				peer_ctl(LCP, ECHO_REP, i[1], d, 4);
			}
		}
	}
	CHECK(ppp_ip_up());

	// A passive start that hears nothing: finished after PPP_DEAD_MS
	out_len = out_pos = 0;
	ppp_start(false, 3, now_ms, rxbuf);
	now_ms += PPP_DEAD_MS;
	ppp_poll(now_ms);
	CHECK(!ppp_running());
}

// The renegotiation the peer may start on an open link: IP goes down until IPCP is open again
static void test_renegotiate(void)
{
	uint8_t d[64], id;
	int n, k;

	open_link();
	k = opt16(d, 1, 1500);
	peer_ctl(LCP, CONF_REQ, 50, d, k);
	CHECK(!ppp_ip_up());
	CHECK(expect(LCP, CONF_REQ, &id, d, &n));
	CHECK(expect(LCP, CONF_ACK, NULL, d, &n));
	peer_ctl(LCP, CONF_ACK, id, d, n);
	CHECK(expect(IPCP, CONF_REQ, &id, d, &n));
}

// ---- fuzz ----

static void check_output(void)
{
	uint16_t p;
	uint8_t i[2048];
	int n;
	int before = failures;

	while (radio_frame(&p, i, &n)) {
		CHECK(n <= 1500);
	}
	if (failures != before) {
		printf("# fuzz: bad output frame\n");
	}
	out_len = out_pos = 0;
}

static void test_fuzz(void)
{
	fuzz_t f;
	uint8_t stream[4096], buf[4200], d[64], pkt[200];
	int sn = 0, k, n;
	long iters = fuzz_iters(3000);

	// A whole session as the peer sends it: LCP and IPCP requests and acks, an echo, a ping, UDP, IPv6CP, terminate
	k = opt16(d, 1, 1500);
	k += opt32(d + k, 2, 0);
	k += opt32(d + k, 5, 0x11223344u);
	uint8_t p[64];
	p[0] = CONF_REQ; p[1] = 1; put16(p + 2, (uint16_t)(4 + k)); memcpy(p + 4, d, (size_t)k);
	sn += encode(stream + sn, LCP, p, 4 + k, 0xFFFFFFFFu);
	uint8_t ack[24] = { CONF_ACK, 1, 0, 20, 1, 4, 0x02, 0x4A, 2, 6, 0, 0, 0, 0, 5, 6, 0, 0, 0, 0 };
	sn += encode(stream + sn, LCP, ack, 20, 0xFFFFFFFFu);
	uint8_t ipcp[10] = { CONF_REQ, 1, 0, 10, 3, 6, 0, 0, 0, 0 };
	sn += encode(stream + sn, IPCP, ipcp, 10, 0);
	put32(ipcp + 6, HOST_IP);
	ipcp[1] = 2;
	sn += encode(stream + sn, IPCP, ipcp, 10, 0);
	ipcp[0] = CONF_ACK;
	ipcp[1] = 1;
	put32(ipcp + 6, RADIO_IP);
	sn += encode(stream + sn, IPCP, ipcp, 10, 0);
	uint8_t echo[12] = { ECHO_REQ, 3, 0, 12, 0x11, 0x22, 0x33, 0x44, 1, 2, 3, 4 };
	sn += encode(stream + sn, LCP, echo, 12, 0);
	uint8_t icmp[8] = { 8, 0, 0, 0, 0, 1, 0, 1 };
	put16(icmp + 2, csum(icmp, 8, 0));
	n = ipv4(pkt, 1, HOST_IP, RADIO_IP, icmp, 8);
	sn += encode(stream + sn, IP, pkt, n, 0);
	n = udp(pkt, HOST_IP, RADIO_IP, 40000, IPGW_SERIAL_PORT, "abc", 3);
	sn += encode(stream + sn, IP, pkt, n, 0);
	n = udp(pkt, HOST_IP, IPGW_INDIVIDUAL_NET | 7u, 4001, 4001, "air", 3);
	sn += encode(stream + sn, IP, pkt, n, 0);
	uint8_t v6[8] = { 1, 1, 0, 8, 1, 4, 0, 0 };
	sn += encode(stream + sn, IPV6CP, v6, 8, 0);
	uint8_t term[4] = { TERM_REQ, 9, 0, 4 };
	sn += encode(stream + sn, LCP, term, 4, 0);

	// The unmutated session ends in Stopping (the Terminate-Request) with IP having been up
	out_len = out_pos = 0;
	ppp_start(false, 11, now_ms, rxbuf);
	air_count = 0;
	feed(stream, sn);
	CHECK(air_count == 1);
	ppp_stats_t s;
	ppp_stats(&s);
	CHECK(s.lcp == PPP_STOPPING);
	check_output();

	// Whole sessions, mutated (bytes, lengths, flags, escapes): the radio must never misbehave and every frame it
	// sends must be well formed
	fuzz_init(&f, 0x5050);
	for (long i = 0; i < iters; i++) {
		memcpy(buf, stream, (size_t)sn);
		int m = fuzz_mutate(&f, buf, sn, (int)sizeof(buf));
		uint8_t *x = fuzz_exact(buf, m);

		ppp_start(fuzz_below(&f, 2) != 0, (uint32_t)i, now_ms, rxbuf);
		for (int at = 0; at < m && ppp_running();) {
			int piece = 1 + (int)fuzz_below(&f, 64);

			if (piece > m - at) {
				piece = m - at;
			}
			size_t took = ppp_input(x + at, (size_t)piece, now_ms);

			CHECK(took <= (size_t)piece);
			at += (int)took;
			if (took < (size_t)piece) {
				CHECK(!ppp_running());
			}
			if (fuzz_below(&f, 8) == 0) {
				now_ms += fuzz_below(&f, 4000);
				ppp_poll(now_ms);
			}
		}
		ppp_stats(&s);
		CHECK((s.lcp == PPP_INITIAL) || ((s.lcp >= PPP_CLOSED) && (s.lcp <= PPP_OPENED)));
		CHECK((s.ipcp == PPP_INITIAL) || ((s.ipcp >= PPP_CLOSED) && (s.ipcp <= PPP_OPENED)));
		check_output();
		free(x);
	}

	// Single packets with a good FCS, mutated inside: every control and network parser behind the framing
	static const uint16_t protos[] = { LCP, IPCP, IP, IPV6CP };
	for (long i = 0; i < iters; i++) {
		if ((i % 64) == 0) {
			quiet = true;
			open_link();
			quiet = false;
			out_len = out_pos = 0;
		}
		uint16_t proto = protos[fuzz_below(&f, 4)];
		uint8_t seed[64];
		int sl;

		if (proto == IP) {
			sl = ipv4(seed, 1, HOST_IP, RADIO_IP, icmp, 8);
		} else {
			sl = 4 + k;
			seed[0] = (uint8_t)(1 + fuzz_below(&f, 11));
			seed[1] = (uint8_t)i;
			put16(seed + 2, (uint16_t)sl);
			memcpy(seed + 4, d, (size_t)k);
		}
		memcpy(buf, seed, (size_t)sl);
		int m = fuzz_mutate(&f, buf, sl, 1400);
		uint8_t e[3000];
		int en = encode(e, proto, buf, m, fuzz_below(&f, 2) ? 0 : 0xFFFFFFFFu);
		uint8_t *x = fuzz_exact(e, en);

		CHECK(ppp_input(x, (size_t)en, now_ms) == (size_t)en || !ppp_running());
		check_output();
		free(x);
		if (!ppp_running()) {
			i |= 63;	// a fresh link next
		}
	}
	printf("# fuzz: %ld sessions, %ld packets\n", iters, iters);
}

int main(void)
{
	fcs_init();
	fuzz_init(&rng, 0x0DD);
	rxbuf = malloc(PPP_RX_BUFFER);
	ipGatewayInit((const uint8_t *)"\x02\x4F\x44\x52\x50\x01", (const uint8_t *)"\x02\x4F\x44\x52\x50\x00");

	test_fcs();
	test_negotiation();
	test_max_failure();
	test_echo_and_rejects();
	test_ipv4();
	test_mru();
	test_air_mtu();
	test_accm();
	test_framing();
	test_terminate();
	test_leaving();
	test_renegotiate();
	test_fuzz();

	free(rxbuf);
	printf("# %s (%d checks, %d failures)\n", failures ? "FAILED" : "PASSED", checks, failures);
	return failures ? 1 : 0;
}
