// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The minimal TCP (core/net/tcp.c) against a scripted peer: handshakes both ways, data both ways within the window and
// the MSS, retransmission with backoff, zero window probes, in-order receive, RST for unknown ports, RFC 5961
// challenge ACKs, orderly close from either side, TIME_WAIT reuse, slot exhaustion, keepalive. Every segment is also
// written to tcpTest.pcap (in Ethernet and IPv4) for tshark's independent check of headers and checksums.
#include <stdio.h>
#include <string.h>
#include "net/tcp.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define RADIO_IP	0x0C002714u	// 12.0.39.20
#define HOST_IP		0x0C002715u	// 12.0.39.21
#define PEER_PORT	40000

#define FIN	0x01
#define SYN	0x02
#define RST	0x04
#define PSH	0x08
#define ACK	0x10

static uint32_t now = 5000;
static FILE *pcap;

// ---- the IP layer ----
static uint8_t segbuf[TCP_SEGMENT_MAX];
typedef struct {
	uint32_t dst;
	uint16_t sport, dport;
	uint32_t seq, ack;
	uint8_t flags;
	uint16_t wnd, mss;
	int len;
	uint8_t data[TCP_MSS];
	bool csum_ok;
} seg_t;
static seg_t out[64];
static int nout;
static bool send_fails;

static void pcap_write(uint32_t src, uint32_t dst, const uint8_t *seg, int len)
{
	uint8_t f[14 + 20 + TCP_SEGMENT_MAX];
	static const uint8_t macs[12] = { 2, 0, 0, 0, 0, 1, 2, 0, 0, 0, 0, 2 };
	uint32_t sum = 0;

	memcpy(f, macs, 12);
	f[12] = 0x08;
	f[13] = 0x00;
	uint8_t *ip = &f[14];
	memset(ip, 0, 20);
	ip[0] = 0x45;
	ip[2] = (uint8_t)((20 + len) >> 8);
	ip[3] = (uint8_t)(20 + len);
	ip[8] = 64;
	ip[9] = 6;
	for (int k = 0; k < 4; k++) {
		ip[12 + k] = (uint8_t)(src >> (24 - 8 * k));
		ip[16 + k] = (uint8_t)(dst >> (24 - 8 * k));
	}
	for (int k = 0; k < 20; k += 2) {
		sum += (uint32_t)(ip[k] << 8) | ip[k + 1];
	}
	while (sum >> 16) {
		sum = (sum & 0xFFFF) + (sum >> 16);
	}
	ip[10] = (uint8_t)(~sum >> 8);
	ip[11] = (uint8_t)~sum;
	memcpy(&f[34], seg, len);
	uint32_t rec[4] = { now / 1000, (now % 1000) * 1000, (uint32_t)(34 + len), (uint32_t)(34 + len) };
	fwrite(rec, sizeof(rec), 1, pcap);
	fwrite(f, 34 + len, 1, pcap);
}

static uint8_t *io_buffer(void) { return segbuf; }
static uint32_t io_local(void) { return RADIO_IP; }

static bool io_send(uint32_t dst, int len)
{
	if (send_fails) {
		return false;
	}
	pcap_write(RADIO_IP, dst, segbuf, len);
	if (nout >= 64) {
		return true;
	}
	seg_t *s = &out[nout++];
	int hdr = (segbuf[12] >> 4) * 4;
	s->dst = dst;
	s->sport = (uint16_t)((segbuf[0] << 8) | segbuf[1]);
	s->dport = (uint16_t)((segbuf[2] << 8) | segbuf[3]);
	s->seq = ((uint32_t)segbuf[4] << 24) | ((uint32_t)segbuf[5] << 16) | ((uint32_t)segbuf[6] << 8) | segbuf[7];
	s->ack = ((uint32_t)segbuf[8] << 24) | ((uint32_t)segbuf[9] << 16) | ((uint32_t)segbuf[10] << 8) | segbuf[11];
	s->flags = segbuf[13];
	s->wnd = (uint16_t)((segbuf[14] << 8) | segbuf[15]);
	s->mss = (hdr >= 24 && segbuf[20] == 2) ? (uint16_t)((segbuf[22] << 8) | segbuf[23]) : 0;
	s->len = len - hdr;
	memcpy(s->data, segbuf + hdr, (size_t)s->len);
	s->csum_ok = tcp_checksum(RADIO_IP, dst, segbuf, len) == 0;
	return true;
}

static const tcp_io_t io = { io_buffer, io_send, io_local };

// ---- the peer ----
static void peer_opts(uint16_t sport, uint16_t dport, uint32_t seq, uint32_t ack, uint8_t flags, uint16_t wnd,
		      const void *data, int len, uint16_t mss, bool bad_csum)
{
	uint8_t s[TCP_HEADER + 4 + 1500];
	int hdr = mss ? 24 : 20;

	memset(s, 0, sizeof(s));
	s[0] = (uint8_t)(sport >> 8);
	s[1] = (uint8_t)sport;
	s[2] = (uint8_t)(dport >> 8);
	s[3] = (uint8_t)dport;
	for (int k = 0; k < 4; k++) {
		s[4 + k] = (uint8_t)(seq >> (24 - 8 * k));
		s[8 + k] = (uint8_t)(ack >> (24 - 8 * k));
	}
	s[12] = (uint8_t)((hdr / 4) << 4);
	s[13] = flags;
	s[14] = (uint8_t)(wnd >> 8);
	s[15] = (uint8_t)wnd;
	if (mss) {
		s[20] = 2;
		s[21] = 4;
		s[22] = (uint8_t)(mss >> 8);
		s[23] = (uint8_t)mss;
	}
	if (len > 0) {
		memcpy(s + hdr, data, (size_t)len);
	}
	uint16_t c = tcp_checksum(HOST_IP, RADIO_IP, s, hdr + len);
	if (bad_csum) {
		c ^= 0x1234;
	}
	s[16] = (uint8_t)(c >> 8);
	s[17] = (uint8_t)c;
	if (!bad_csum) {
		pcap_write(HOST_IP, RADIO_IP, s, hdr + len);	// tshark would rightly flag the broken one
	}
	tcp_input(HOST_IP, s, hdr + len, now);
}

static void peer(uint16_t dport, uint32_t seq, uint32_t ack, uint8_t flags, uint16_t wnd, const void *data, int len)
{
	peer_opts(PEER_PORT, dport, seq, ack, flags, wnd, data, len, 0, false);
}

static void advance(uint32_t ms)
{
	for (uint32_t t = 0; t < ms; t += 5) {
		now += 5;
		tcp_poll(now);
	}
}

// ---- the application: an echo server that records its callbacks ----
static int opened_c = -1, closed_c = -1, closed_err = -1, peer_closed_c = -1, sent_calls;
static uint8_t rx[4096];
static int rxn;
static int space_limit = -1;
static bool echo = true;

static void a_opened(int c) { opened_c = c; }
static void a_recv(int c, const uint8_t *d, int n)
{
	memcpy(rx + rxn, d, (size_t)n);
	rxn += n;
	if (echo) {
		tcp_write(c, d, n);
	}
}
static int a_space(int c) { (void)c; return space_limit < 0 ? 1000 : space_limit; }
static void a_sent(int c) { (void)c; sent_calls++; }
static void a_peer_closed(int c) { peer_closed_c = c; }
static void a_closed(int c, bool err) { closed_c = c; closed_err = err; }

static const tcp_app_t app = { a_opened, a_recv, a_space, a_sent, a_peer_closed, a_closed };

static void reset_app(void)
{
	opened_c = closed_c = closed_err = peer_closed_c = -1;
	sent_calls = 0;
	rxn = 0;
	space_limit = -1;
	echo = true;
	nout = 0;
}

static seg_t *last(void)
{
	return nout ? &out[nout - 1] : NULL;
}

// A passive open on port 7 from the peer's ISS: returns the handle, *radio_seq the radio's next sequence number
static int handshake(uint16_t port, uint32_t iss, uint32_t *radio_seq)
{
	nout = 0;
	peer_opts(PEER_PORT, port, iss, 0, SYN, 8192, NULL, 0, 1460, false);
	CHECK(nout == 1);
	seg_t *s = last();
	CHECK(s && s->flags == (SYN | ACK) && s->ack == iss + 1 && s->mss == TCP_MSS && s->csum_ok);
	uint32_t rs = s ? s->seq + 1 : 0;
	peer(port, iss + 1, rs, ACK, 8192, NULL, 0);
	*radio_seq = rs;
	return opened_c;
}

static void test_passive_echo_close(void)
{
	uint32_t rs;
	reset_app();
	int c = handshake(7, 1000, &rs);
	CHECK(c >= 0 && tcp_state(c) == TCP_ESTABLISHED && tcp_remote_ip(c) == HOST_IP);

	nout = 0;
	peer(7, 1001, rs, ACK | PSH, 8192, "hello", 5);
	CHECK(rxn == 5 && memcmp(rx, "hello", 5) == 0);
	CHECK(nout == 1 && out[0].len == 5 && out[0].seq == rs && out[0].ack == 1006 && memcmp(out[0].data, "hello", 5) == 0);
	CHECK(out[0].flags == (ACK | PSH) && out[0].csum_ok);

	// The same segment again (the peer missed our ACK): acknowledged, not delivered twice
	nout = 0;
	peer(7, 1001, rs + 5, ACK | PSH, 8192, "hello", 5);
	CHECK(rxn == 5 && nout == 1 && out[0].len == 0 && out[0].ack == 1006);

	// A retransmission overlapping new data: only the new part is delivered
	nout = 0;
	peer(7, 1003, rs + 5, ACK | PSH, 8192, "llo world", 9);
	CHECK(rxn == 11 && memcmp(rx + 5, " world", 6) == 0 && out[0].ack == 1012);

	// Out of order: dropped, the ACK asks for RCV.NXT
	nout = 0;
	peer(7, 1020, rs + 11, ACK | PSH, 8192, "later", 5);
	CHECK(rxn == 11 && nout == 1 && out[0].ack == 1012 && out[0].len == 0);

	// The peer closes: peer_closed, then our close sends FIN, the peer's ACK ends it (LAST_ACK)
	nout = 0;
	peer(7, 1012, rs + 11, ACK | FIN, 8192, NULL, 0);
	CHECK(peer_closed_c == c && tcp_state(c) == TCP_CLOSE_WAIT);
	CHECK(nout == 1 && out[0].ack == 1013);
	tcp_close(c);
	nout = 0;
	tcp_poll(now);
	CHECK(nout == 1 && (out[0].flags & FIN) && out[0].seq == rs + 11 && tcp_state(c) == TCP_LAST_ACK);
	peer(7, 1013, rs + 12, ACK, 8192, NULL, 0);
	CHECK(tcp_state(c) == TCP_CLOSED && closed_c == -1);
}

static void test_reset_unknown(void)
{
	reset_app();
	peer(9, 5000, 0, SYN, 8192, NULL, 0);
	CHECK(nout == 1 && out[0].flags == (RST | ACK) && out[0].ack == 5001 && out[0].seq == 0);
	nout = 0;
	peer(9, 5000, 777, ACK, 8192, "x", 1);
	CHECK(nout == 1 && out[0].flags == RST && out[0].seq == 777);
	nout = 0;
	peer(9, 5000, 777, RST, 8192, NULL, 0);
	CHECK(nout == 0);	// never a RST for a RST
	nout = 0;
	peer_opts(PEER_PORT, 7, 1, 0, SYN, 8192, NULL, 0, 0, true);
	CHECK(nout == 0);	// bad checksum: ignored
	tcp_stats_t st;
	tcp_get_stats(&st);
	CHECK(st.bad_checksum == 1 && st.refused >= 1);
}

static void test_retransmit_and_window(void)
{
	uint32_t rs;
	reset_app();
	echo = false;
	int c = handshake(7, 2000, &rs);
	uint8_t big[TCP_TXBUF + 10];
	for (int k = 0; k < (int)sizeof(big); k++) {
		big[k] = (uint8_t)k;
	}
	CHECK(tcp_write(c, big, sizeof(big)) == TCP_TXBUF);
	CHECK(tcp_space(c) == 0);
	nout = 0;
	tcp_poll(now);
	CHECK(nout == 2 && out[0].len == TCP_MSS && out[1].len == TCP_TXBUF - TCP_MSS && out[1].seq == rs + TCP_MSS);
	CHECK(memcmp(out[1].data, big + TCP_MSS, (size_t)out[1].len) == 0);

	// No ACK: after the RTO the first segment goes again, from SND.UNA
	nout = 0;
	advance(TCP_RTO_MIN_MS - 10);
	CHECK(nout == 0);
	advance(20);	// the RTO is the minimum: the handshake measured a round trip under 5 ms
	CHECK(nout == 1 && out[0].seq == rs && out[0].len == TCP_MSS);	// one segment after a timeout
	advance(2 * TCP_RTO_MIN_MS - 30);
	CHECK(nout == 1);
	advance(40);
	CHECK(nout == 2 && out[1].seq == rs);	// backed off: the next one twice as late

	// The peer acknowledges the first segment only, with a small window: the second goes within it
	nout = 0;
	peer(7, 2001, rs + TCP_MSS, ACK, 100, NULL, 0);
	CHECK(sent_calls == 1 && tcp_space(c) == TCP_MSS);
	tcp_poll(now);
	CHECK(nout == 1 && out[0].seq == rs + TCP_MSS && out[0].len == 100);

	// Zero window: nothing goes, the persist timer probes with one byte
	nout = 0;
	peer(7, 2001, rs + TCP_MSS + 100, ACK, 0, NULL, 0);
	tcp_poll(now);
	CHECK(nout == 0);
	advance(4000);
	CHECK(nout >= 1 && out[0].len == 1 && out[0].seq == rs + TCP_MSS + 100);
	// The window opens: the rest goes
	nout = 0;
	peer(7, 2001, rs + TCP_MSS + 101, ACK, 8192, NULL, 0);
	tcp_poll(now);
	CHECK(nout == 1 && out[0].len == TCP_TXBUF - TCP_MSS - 101);
	peer(7, 2001, rs + TCP_TXBUF, ACK, 8192, NULL, 0);
	CHECK(tcp_unacked(c) == 0 && tcp_space(c) == TCP_TXBUF);

	// Retries exhausted: RST and closed(error)
	tcp_write(c, "abc", 3);
	nout = 0;
	advance(120000);
	CHECK(closed_c == c && closed_err == 1 && tcp_state(c) == TCP_CLOSED);
	CHECK(nout == TCP_RETRIES + 2 && (last()->flags & RST));
	// Backoff: each retransmission waits twice as long as the one before
	tcp_stats_t st;
	tcp_get_stats(&st);
	CHECK(st.timeouts >= 1 && st.retransmits >= TCP_RETRIES);
}

static void test_receive_window(void)
{
	uint32_t rs;
	reset_app();
	echo = false;
	space_limit = 4;
	int c = handshake(7, 3000, &rs);
	CHECK(out[0].wnd == 2 * TCP_MSS);	// the SYN-ACK: the default (the application has no handle yet)
	nout = 0;
	peer(7, 3001, rs, ACK, 8192, "abcdefgh", 8);
	CHECK(rxn == 4 && memcmp(rx, "abcd", 4) == 0 && out[0].ack == 3005 && out[0].wnd == 4);
	space_limit = 1000;
	peer(7, 3005, rs, ACK, 8192, "efgh", 4);
	CHECK(rxn == 8);
	tcp_abort(c);
	CHECK(tcp_state(c) == TCP_CLOSED && (last()->flags & RST));
}

static void test_active_close_time_wait(void)
{
	uint32_t rs;
	reset_app();
	int c = handshake(7, 4000, &rs);
	tcp_write(c, "bye", 3);
	tcp_close(c);
	nout = 0;
	tcp_poll(now);
	CHECK(nout == 1 && out[0].len == 3 && (out[0].flags & FIN) && tcp_state(c) == TCP_FIN_WAIT_1);
	CHECK(tcp_write(c, "x", 1) == 0);
	peer(7, 4001, rs + 4, ACK, 8192, NULL, 0);
	CHECK(tcp_state(c) == TCP_FIN_WAIT_2);
	nout = 0;
	peer(7, 4001, rs + 4, ACK | FIN, 8192, NULL, 0);
	CHECK(tcp_state(c) == TCP_TIME_WAIT && nout == 1 && out[0].ack == 4002);
	// A retransmitted FIN is acknowledged again
	nout = 0;
	peer(7, 4001, rs + 4, ACK | FIN, 8192, NULL, 0);
	CHECK(nout == 1 && out[0].ack == 4002);
	advance(TCP_TIME_WAIT_MS + 10);
	CHECK(tcp_state(c) == TCP_CLOSED && closed_c == -1);

	// Simultaneous close: FIN crosses FIN (CLOSING), then the ACK (TIME_WAIT)
	reset_app();
	c = handshake(7, 4500, &rs);
	tcp_close(c);
	tcp_poll(now);
	peer(7, 4501, rs, ACK | FIN, 8192, NULL, 0);
	CHECK(tcp_state(c) == TCP_CLOSING);
	peer(7, 4502, rs + 1, ACK, 8192, NULL, 0);
	CHECK(tcp_state(c) == TCP_TIME_WAIT);
	advance(TCP_TIME_WAIT_MS + 10);
}

static void test_slots(void)
{
	reset_app();
	int c[TCP_CONNS];
	for (int k = 0; k < TCP_CONNS; k++) {
		nout = 0;
		peer_opts((uint16_t)(PEER_PORT + 10 + k), 7, 100u * k, 0, SYN, 8192, NULL, 0, 0, false);
		CHECK(nout == 1);
		peer_opts((uint16_t)(PEER_PORT + 10 + k), 7, 100u * k + 1, out[0].seq + 1, ACK, 8192, NULL, 0, 0, false);
		c[k] = opened_c;
	}
	CHECK(tcp_open_count() == TCP_CONNS);
	nout = 0;
	peer_opts(PEER_PORT + 20, 7, 9, 0, SYN, 8192, NULL, 0, 0, false);
	CHECK(nout == 0);	// no slot: silently dropped (the client tries again)
	// One goes to TIME_WAIT: its slot is taken by the next SYN
	tcp_close(c[0]);
	tcp_poll(now);
	peer_opts(PEER_PORT + 10, 7, 1, out[nout - 1].seq + 1, ACK | FIN, 8192, NULL, 0, 0, false);
	CHECK(tcp_state(c[0]) == TCP_TIME_WAIT);
	nout = 0;
	peer_opts(PEER_PORT + 20, 7, 9, 0, SYN, 8192, NULL, 0, 0, false);
	CHECK(nout == 1 && out[0].flags == (SYN | ACK));
	tcp_abort_all();
	CHECK(tcp_open_count() == 0 && closed_err == 1);
	(void)c;
}

static void test_rfc5961(void)
{
	uint32_t rs;
	reset_app();
	int c = handshake(7, 6000, &rs);
	// RST not at RCV.NXT but in the window: challenge ACK, the connection stays
	nout = 0;
	peer(7, 6100, 0, RST, 0, NULL, 0);
	CHECK(tcp_state(c) == TCP_ESTABLISHED && nout == 1 && out[0].flags == ACK && out[0].ack == 6001);
	// A SYN on an established connection: challenge ACK
	nout = 0;
	peer(7, 7000, 0, SYN, 8192, NULL, 0);
	CHECK(tcp_state(c) == TCP_ESTABLISHED && nout == 1 && out[0].flags == ACK);
	// An ACK for data never sent: ACK back, nothing else
	nout = 0;
	peer(7, 6001, rs + 500, ACK, 8192, NULL, 0);
	CHECK(tcp_state(c) == TCP_ESTABLISHED && nout == 1);
	// The exact RST: closed(error)
	peer(7, 6001, 0, RST, 0, NULL, 0);
	CHECK(tcp_state(c) == TCP_CLOSED && closed_c == c && closed_err == 1);
}

static void test_active_open(void)
{
	reset_app();
	int c = tcp_connect(HOST_IP, 14580, &app, now);
	CHECK(c >= 0 && tcp_state(c) == TCP_SYN_SENT);
	tcp_poll(now);
	CHECK(nout == 1 && out[0].flags == SYN && out[0].mss == TCP_MSS && out[0].dport == 14580 && out[0].sport >= 49152);
	uint16_t lport = out[0].sport;
	uint32_t iss = out[0].seq;
	// The peer answers with a smaller MSS
	nout = 0;
	peer_opts(14580, lport, 90000, iss + 1, SYN | ACK, 4000, NULL, 0, 300, false);
	CHECK(opened_c == c && tcp_state(c) == TCP_ESTABLISHED && nout == 1 && out[0].flags == ACK && out[0].ack == 90001);
	uint8_t data[700];
	memset(data, 'a', sizeof(data));
	tcp_write(c, data, sizeof(data));
	nout = 0;
	tcp_poll(now);
	CHECK(nout == 3 && out[0].len == 300 && out[1].len == 300 && out[2].len == 100);
	tcp_abort(c);

	// Refused
	reset_app();
	c = tcp_connect(HOST_IP, 14581, &app, now);
	tcp_poll(now);
	lport = out[0].sport;
	peer_opts(14581, lport, 0, out[0].seq + 1, RST | ACK, 0, NULL, 0, 0, false);
	CHECK(closed_c == c && closed_err == 1 && tcp_state(c) == TCP_CLOSED);

	// No answer: SYN retries with backoff, then closed(error)
	reset_app();
	c = tcp_connect(HOST_IP, 14582, &app, now);
	advance(60000);
	CHECK(nout == TCP_SYN_RETRIES + 1 && closed_c == c && closed_err == 1);
	for (int k = 1; k < nout; k++) {
		CHECK(out[k].flags == SYN && out[k].seq == out[0].seq);
	}
}

static void test_keepalive(void)
{
	uint32_t rs;
	reset_app();
	int c = handshake(7, 8000, &rs);
	nout = 0;
	advance(TCP_KEEPALIVE_MS - 100);
	CHECK(nout == 0);
	advance(200);
	CHECK(nout == 1 && out[0].seq == rs - 1 && out[0].len == 0);
	// Answered: the connection lives on, the next probe is a full idle time away
	peer(7, 8001, rs, ACK, 8192, NULL, 0);
	nout = 0;
	advance(TCP_KEEPALIVE_MS / 2);
	CHECK(nout == 0);
	advance(TCP_KEEPALIVE_MS + TCP_KEEPALIVE_INTVL_MS * (TCP_KEEPALIVE_PROBES + 1));
	CHECK(nout == TCP_KEEPALIVE_PROBES + 1 && (last()->flags & RST) && closed_c == c && closed_err == 1);
}

static void test_lost_synack_and_send_failure(void)
{
	reset_app();
	peer_opts(PEER_PORT, 7, 100, 0, SYN, 8192, NULL, 0, 0, false);
	uint32_t s1 = out[0].seq;
	// The SYN again (our SYN-ACK was lost): the same SYN-ACK
	nout = 0;
	peer_opts(PEER_PORT, 7, 100, 0, SYN, 8192, NULL, 0, 0, false);
	CHECK(nout == 1 && out[0].flags == (SYN | ACK) && out[0].seq == s1);
	// The IP layer can't send (USB busy): the timer sends it again later
	send_fails = true;
	advance(1100);
	send_fails = false;
	nout = 0;
	advance(2100);
	CHECK(nout >= 1 && out[0].flags == (SYN | ACK) && out[0].seq == s1);
	peer_opts(PEER_PORT, 7, 101, s1 + 1, ACK, 8192, NULL, 0, 0, false);
	CHECK(opened_c >= 0 && tcp_state(opened_c) == TCP_ESTABLISHED);
	tcp_abort(opened_c);
}

// The peer had both segments, only its ACK was lost: after the timeout one segment goes again, and the peer's ACK for
// everything (beyond SND.NXT after the go back, within what was sent) is taken
static void test_ack_after_go_back(void)
{
	uint32_t rs;
	reset_app();
	echo = false;
	int c = handshake(7, 9000, &rs);
	uint8_t d[TCP_TXBUF];
	memset(d, 'z', sizeof(d));
	tcp_write(c, d, sizeof(d));
	tcp_poll(now);
	nout = 0;
	advance(TCP_RTO_MIN_MS + 10);
	CHECK(nout == 1 && out[0].seq == rs);
	peer(7, 9001, rs + TCP_TXBUF, ACK, 8192, NULL, 0);
	CHECK(tcp_unacked(c) == 0 && sent_calls >= 1);
	nout = 0;
	advance(3000);
	CHECK(nout == 0);	// nothing left to resend
	tcp_abort(c);
}

int main(void)
{
	pcap = fopen("tcpTest.pcap", "wb");
	uint32_t hdr[6] = { 0xA1B2C3D4u, 0x00040002u, 0, 0, 65535, 1 };
	fwrite(hdr, sizeof(hdr), 1, pcap);

	tcp_init(&io, 0x12345678u);
	CHECK(tcp_listen(7, &app));

	test_passive_echo_close();
	test_reset_unknown();
	test_retransmit_and_window();
	test_receive_window();
	test_active_close_time_wait();
	test_slots();
	test_rfc5961();
	test_active_open();
	test_keepalive();
	test_lost_synack_and_send_failure();
	test_ack_after_go_back();

	fclose(pcap);
	printf("# tcpTest: %d checks, %d failures\n", checks, failures);
	printf(failures ? "# FAILED\n" : "# PASSED\n");
	return failures ? 1 : 0;
}
