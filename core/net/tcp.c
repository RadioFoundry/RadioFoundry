// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// A minimal TCP (see tcp.h), written from RFC 9293 (the state machine and segment arrival rules of 3.10), RFC 6298
// (the retransmission timer), RFC 5961 (RST / SYN checks), RFC 1122 4.2.3.6 (keepalive) and RFC 6528 (initial
// sequence numbers, here a counter and the clock rather than a keyed hash: a local link).
#include <stddef.h>
#include <string.h>
#include "config.h"
#include "net/tcp.h"

#define FIN	0x01u
#define SYN	0x02u
#define RST	0x04u
#define PSH	0x08u
#define ACK	0x10u

#define SEQ_LT(a, b)	((int32_t)((a) - (b)) < 0)
#define SEQ_LE(a, b)	((int32_t)((a) - (b)) <= 0)
#define SEQ_GT(a, b)	((int32_t)((a) - (b)) > 0)
#define SEQ_GE(a, b)	((int32_t)((a) - (b)) >= 0)
#define DUE(now, t)	(((t) != 0) && ((int32_t)((now) - (t)) >= 0))

#define F_ACK_NOW	0x01u	// a pure ACK is owed
#define F_FIN_PENDING	0x02u	// the application closed: FIN after the buffered data
#define F_FIN_SENT	0x04u	// FIN sent (it holds the sequence number SND.NXT - 1)
#define F_TIMING	0x08u	// an RTT sample is running
#define F_OWNED		0x10u	// the application holds the handle (callbacks go to it)
#define F_OUTPUT	0x20u	// tcp_write added data: output at the next pass
#define F_PROBE		0x40u	// a zero window probe may go out once
#define F_ONE		0x80u	// after a timeout one segment at a time until new data is acknowledged (RFC 5681 3.1)

typedef struct {
	uint8_t state;
	uint8_t flags;
	uint8_t retries;
	uint8_t probes;		// keepalive probes unanswered
	uint8_t gen;		// bumped on every allocation (a handle reused inside a callback is noticed)
	const tcp_app_t *app;
	uint32_t rip;
	uint16_t rport, lport;
	uint32_t iss, snd_una, snd_nxt, rcv_nxt;
	uint16_t snd_wnd, mss;
	uint16_t txhead, txlen, txsent;	// the ring: data from SND.UNA on, of which txsent bytes were sent
	uint16_t rto, srtt, rttvar;	// ms; srtt 0 = no sample yet
	uint32_t timer;			// retransmission / persist / TIME_WAIT / FIN_WAIT_2 deadline, 0 = off
	uint32_t rtt_seq, rtt_start;
	uint32_t last_rx;		// the last segment from the peer (keepalive)
	uint32_t ka_next;		// the next keepalive probe
	uint8_t txbuf[TCP_TXBUF];
} conn_t;

static ODR_CPU_RAM conn_t conns[TCP_CONNS];
static struct {
	uint16_t port;
	const tcp_app_t *app;
} listeners[TCP_LISTENERS];
static const tcp_io_t *io;
static uint32_t isn_salt, isn_count;
static uint16_t next_port;
static tcp_stats_t stats;

static uint16_t get16(const uint8_t *p)
{
	return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t get32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

uint16_t tcp_checksum(uint32_t src, uint32_t dst, const uint8_t *seg, int len)
{
	uint32_t sum = (src >> 16) + (src & 0xFFFFu) + (dst >> 16) + (dst & 0xFFFFu) + 6u + (uint32_t)len;

	for (; len > 1; seg += 2, len -= 2) {
		sum += get16(seg);
	}
	if (len > 0) {
		sum += (uint32_t)seg[0] << 8;
	}
	while (sum >> 16) {
		sum = (sum & 0xFFFFu) + (sum >> 16);
	}
	return (uint16_t)~sum;
}

static bool valid(int c)
{
	return (c >= 0) && (c < TCP_CONNS) && (conns[c].state != TCP_CLOSED);
}

static uint16_t window(int i)
{
	conn_t *c = &conns[i];
	int w = 2 * TCP_MSS;

	if ((c->flags & F_OWNED) && (c->app->space != NULL)) {
		w = c->app->space(i);
	}
	if ((c->state != TCP_ESTABLISHED) && (c->state != TCP_FIN_WAIT_1) && (c->state != TCP_FIN_WAIT_2) &&
	    (c->state != TCP_SYN_RCVD) && (c->state != TCP_SYN_SENT)) {
		w = 0;	// nothing more is taken after the peer's FIN
	}
	return (uint16_t)((w < 0) ? 0 : ((w > 0xFFFF) ? 0xFFFF : w));
}

// One segment from connection i: flags, seq, the data at txbuf offset off (len bytes), the MSS option on a SYN
static void send_segment(int i, uint8_t flags, uint32_t seq, int off, int len)
{
	conn_t *c = &conns[i];
	uint8_t *s = io->buffer();
	int hdr = (flags & SYN) ? (TCP_HEADER + 4) : TCP_HEADER;
	uint32_t local = io->local_ip();

	if (local == 0) {
		return;
	}
	put16(&s[0], c->lport);
	put16(&s[2], c->rport);
	put32(&s[4], seq);
	put32(&s[8], (flags & ACK) ? c->rcv_nxt : 0);
	s[12] = (uint8_t)((hdr / 4) << 4);
	s[13] = flags;
	put16(&s[14], window(i));
	put16(&s[16], 0);
	put16(&s[18], 0);
	if (flags & SYN) {
		s[20] = 2;	// MSS
		s[21] = 4;
		put16(&s[22], TCP_MSS);
	}
	for (int k = 0; k < len; k++) {
		s[hdr + k] = c->txbuf[(c->txhead + off + k) % TCP_TXBUF];
	}
	put16(&s[16], tcp_checksum(local, c->rip, s, hdr + len));
	if (io->send(c->rip, hdr + len)) {
		stats.segs_out++;
	}
	c->flags &= (uint8_t)~F_ACK_NOW;	// any segment carries the ACK
}

// A RST answering a segment that belongs to no connection (RFC 9293 3.10.7.1)
static void send_reset(uint32_t dst, uint16_t sport, uint16_t dport, uint32_t seq, uint32_t ack, bool with_ack)
{
	uint8_t *s = io->buffer();
	uint32_t local = io->local_ip();

	if (local == 0) {
		return;
	}
	put16(&s[0], sport);
	put16(&s[2], dport);
	put32(&s[4], seq);
	put32(&s[8], with_ack ? ack : 0);
	s[12] = (TCP_HEADER / 4) << 4;
	s[13] = (uint8_t)(RST | (with_ack ? ACK : 0));
	put16(&s[14], 0);
	put16(&s[16], 0);
	put16(&s[18], 0);
	put16(&s[16], tcp_checksum(local, dst, s, TCP_HEADER));
	if (io->send(dst, TCP_HEADER)) {
		stats.segs_out++;
		stats.rst_out++;
	}
}

static void arm(conn_t *c, uint32_t now)
{
	c->timer = now + c->rto;
	if (c->timer == 0) {
		c->timer = 1;
	}
}

// Free a slot. The application hears closed() when it still holds the handle
static void drop(int i, bool error)
{
	conn_t *c = &conns[i];
	bool tell = (c->flags & F_OWNED) != 0;
	const tcp_app_t *app = c->app;

	c->state = TCP_CLOSED;
	c->flags = 0;
	if (tell && (app->closed != NULL)) {
		app->closed(i, error);
	}
}

static void time_wait(conn_t *c, uint32_t now)
{
	c->state = TCP_TIME_WAIT;
	c->timer = now + TCP_TIME_WAIT_MS;
	if (c->timer == 0) {
		c->timer = 1;
	}
	c->flags &= (uint8_t)~F_OWNED;	// the application is told nothing more
}

// The slot for a new connection: a free one, else the oldest TIME_WAIT (RFC 6191 allows reuse; ours is short anyway)
static int alloc(void)
{
	int tw = -1;

	for (int i = 0; i < TCP_CONNS; i++) {
		if (conns[i].state == TCP_CLOSED) {
			return i;
		}
		if ((conns[i].state == TCP_TIME_WAIT) && ((tw < 0) || SEQ_LT(conns[i].timer, conns[tw].timer))) {
			tw = i;
		}
	}
	return tw;
}

static void setup(int i, const tcp_app_t *app, uint32_t now)
{
	conn_t *c = &conns[i];
	uint8_t gen = (uint8_t)(c->gen + 1u);

	memset(c, 0, offsetof(conn_t, txbuf));
	c->gen = gen;
	c->app = app;
	c->mss = TCP_MSS;
	c->rto = TCP_RTO_INIT_MS;
	c->iss = isn_salt + (now * 250u) + (isn_count++ * 0x01000193u);
	c->snd_una = c->iss;
	c->snd_nxt = c->iss;
	c->last_rx = now;
}

// Send what may go now (RFC 9293 3.10.4 / 3.8.6.2: window, MSS; no Nagle, the applications write whole messages)
static void output(int i, uint32_t now)
{
	conn_t *c = &conns[i];

	c->flags &= (uint8_t)~F_OUTPUT;
	if ((c->state == TCP_SYN_SENT) || (c->state == TCP_SYN_RCVD)) {
		if (c->snd_nxt == c->iss) {
			send_segment(i, (c->state == TCP_SYN_RCVD) ? (SYN | ACK) : SYN, c->iss, 0, 0);
			c->snd_nxt = c->iss + 1u;
			if (c->timer == 0) {
				arm(c, now);
			}
		}
		return;
	}
	bool can_send = (c->state == TCP_ESTABLISHED) || (c->state == TCP_CLOSE_WAIT) ||
			(c->state == TCP_FIN_WAIT_1) || (c->state == TCP_CLOSING) || (c->state == TCP_LAST_ACK);

	while (can_send) {
		uint32_t inflight = c->snd_nxt - c->snd_una;
		int unsent = c->txlen - c->txsent;
		int usable = (c->snd_wnd > inflight) ? (int)(c->snd_wnd - inflight) : 0;
		int n = unsent;

		if (n > c->mss) {
			n = c->mss;
		}
		if (n > usable) {
			n = usable;
		}
		if ((c->flags & F_ONE) && (inflight > 0)) {
			n = 0;
		}
		if ((n == 0) && (unsent > 0) && (c->flags & F_PROBE)) {
			n = 1;	// zero window probe (RFC 9293 3.8.6.1)
			c->flags &= (uint8_t)~F_PROBE;
		}
		bool fin = (c->flags & F_FIN_PENDING) && !(c->flags & F_FIN_SENT) && ((c->txsent + n) == c->txlen);

		if ((n == 0) && !fin) {
			if ((unsent > 0) && (inflight == 0) && (c->timer == 0)) {
				arm(c, now);	// the peer's window is shut: the persist timer probes it
			}
			break;
		}
		send_segment(i, (uint8_t)(ACK | ((n > 0) ? PSH : 0) | (fin ? FIN : 0)), c->snd_nxt, c->txsent, n);
		if (!(c->flags & F_TIMING) && (n > 0)) {
			c->flags |= F_TIMING;
			c->rtt_seq = c->snd_nxt + (uint32_t)n;
			c->rtt_start = now;
		}
		c->txsent = (uint16_t)(c->txsent + n);
		c->snd_nxt += (uint32_t)n + (fin ? 1u : 0u);
		if (c->timer == 0) {
			arm(c, now);
		}
		if (fin) {
			c->flags |= F_FIN_SENT;
			if (c->state == TCP_ESTABLISHED) {
				c->state = TCP_FIN_WAIT_1;
			} else if (c->state == TCP_CLOSE_WAIT) {
				c->state = TCP_LAST_ACK;
			}
			break;
		}
	}
	if (c->flags & F_ACK_NOW) {
		send_segment(i, ACK, c->snd_nxt, 0, 0);
	}
}

// RFC 6298 2.2 / 2.3, in ms (alpha 1/8, beta 1/4, K = 4; G is the poll period, folded into the minimum)
static void rtt_sample(conn_t *c, uint32_t r)
{
	if (r > 0xFFFFu) {
		r = 0xFFFFu;
	}
	if (c->srtt == 0) {
		c->srtt = (uint16_t)((r > 0) ? r : 1u);
		c->rttvar = (uint16_t)(r / 2u);
	} else {
		uint32_t d = (r > c->srtt) ? (r - c->srtt) : (c->srtt - r);

		c->rttvar = (uint16_t)((3u * c->rttvar + d) / 4u);
		c->srtt = (uint16_t)((7u * c->srtt + r) / 8u);
	}
	uint32_t rto = c->srtt + 4u * c->rttvar;

	c->rto = (uint16_t)((rto < TCP_RTO_MIN_MS) ? TCP_RTO_MIN_MS : ((rto > TCP_RTO_MAX_MS) ? TCP_RTO_MAX_MS : rto));
}

static void parse_mss(conn_t *c, const uint8_t *seg, int hdr)
{
	for (int k = TCP_HEADER; k < hdr;) {
		uint8_t kind = seg[k];

		if (kind == 0) {
			break;
		}
		if (kind == 1) {
			k++;
			continue;
		}
		if ((k + 1 >= hdr) || (seg[k + 1] < 2) || (k + seg[k + 1] > hdr)) {
			break;
		}
		if ((kind == 2) && (seg[k + 1] == 4)) {
			uint16_t m = get16(&seg[k + 2]);

			if ((m >= 64) && (m < c->mss)) {
				c->mss = m;
			}
		}
		k += seg[k + 1];
	}
}

// The application's callbacks may close, abort or even reuse the slot: true while it is still the same connection
static bool same(int i, uint8_t gen)
{
	return (conns[i].state != TCP_CLOSED) && (conns[i].gen == gen);
}

static void segment_for(int i, uint8_t flags, uint32_t seq, uint32_t ackn, uint16_t wnd, const uint8_t *seg, int hdr,
			const uint8_t *data, int len, uint32_t now)
{
	conn_t *c = &conns[i];
	uint8_t gen = c->gen;

	c->last_rx = now;
	c->probes = 0;
	c->ka_next = 0;

	if (c->state == TCP_SYN_SENT) {
		if ((flags & ACK) && (ackn != c->iss + 1u)) {
			if (!(flags & RST)) {
				send_reset(c->rip, c->lport, c->rport, ackn, 0, false);
			}
			return;
		}
		if (flags & RST) {
			if (flags & ACK) {
				stats.refused++;
				drop(i, true);	// connection refused
			}
			return;
		}
		if ((flags & SYN) && (flags & ACK)) {
			c->rcv_nxt = seq + 1u;
			c->snd_una = ackn;
			c->snd_wnd = wnd;
			c->timer = 0;
			c->retries = 0;
			if (c->srtt == 0) {
				rtt_sample(c, now - c->rtt_start);
			}
			parse_mss(c, seg, hdr);
			c->state = TCP_ESTABLISHED;
			c->flags |= F_ACK_NOW;
			if (c->app->opened != NULL) {
				c->app->opened(i);
			}
			if (same(i, gen)) {
				output(i, now);
			}
		}
		return;
	}

	// TIME_WAIT: a retransmitted FIN is acknowledged again (and restarts the wait)
	if (c->state == TCP_TIME_WAIT) {
		if ((flags & RST) && (seq == c->rcv_nxt)) {
			c->state = TCP_CLOSED;
		} else if (flags & FIN) {
			time_wait(c, now);
			send_segment(i, ACK, c->snd_nxt, 0, 0);
		}
		return;
	}

	// The sequence number check (3.10.7.4), simplified: in order or old data; RST must match exactly (RFC 5961 3.2)
	if (flags & RST) {
		if (seq == c->rcv_nxt) {
			if (c->state == TCP_SYN_RCVD) {
				c->state = TCP_CLOSED;	// back to listening: the application never saw it
				c->flags = 0;
			} else {
				drop(i, true);
			}
		} else if (SEQ_GT(seq, c->rcv_nxt) && SEQ_LT(seq, c->rcv_nxt + 0xFFFFu)) {
			send_segment(i, ACK, c->snd_nxt, 0, 0);	// challenge ACK
		}
		return;
	}
	if (flags & SYN) {
		if ((c->state == TCP_SYN_RCVD) && (seq + 1u == c->rcv_nxt)) {
			c->snd_nxt = c->iss;	// our SYN-ACK was lost: send it again
			output(i, now);
		} else {
			send_segment(i, ACK, c->snd_nxt, 0, 0);	// RFC 5961 4.2 challenge ACK
		}
		return;
	}
	if (!(flags & ACK)) {
		return;
	}

	// ACK (3.10.7.4, fifth check)
	if (c->state == TCP_SYN_RCVD) {
		if (ackn != c->iss + 1u) {
			send_reset(c->rip, c->lport, c->rport, ackn, 0, false);
			return;
		}
		c->snd_una = ackn;
		c->snd_wnd = wnd;
		c->timer = 0;
		c->retries = 0;
		rtt_sample(c, now - c->rtt_start);
		c->state = TCP_ESTABLISHED;
		c->flags |= F_OWNED;
		stats.accepted++;
		if (c->app->opened != NULL) {
			c->app->opened(i);
		}
		if (!same(i, gen)) {
			return;
		}
	}
	if (SEQ_GT(ackn, c->snd_nxt)) {
		send_segment(i, ACK, c->snd_nxt, 0, 0);	// acknowledges something not sent yet
		return;
	}
	bool acked_data = false;

	if (SEQ_GE(ackn, c->snd_una)) {
		c->snd_wnd = wnd;
	}
	if (SEQ_GT(ackn, c->snd_una)) {
		uint32_t n = ackn - c->snd_una;
		bool fin_acked = (c->flags & F_FIN_SENT) && (ackn == c->snd_nxt);
		uint32_t d = fin_acked ? (n - 1u) : n;

		if (d > c->txlen) {
			d = c->txlen;
		}
		c->txhead = (uint16_t)((c->txhead + d) % TCP_TXBUF);
		c->txlen = (uint16_t)(c->txlen - d);
		c->txsent = (uint16_t)((c->txsent > d) ? (c->txsent - d) : 0u);
		c->snd_una = ackn;
		if ((c->flags & F_TIMING) && SEQ_GE(ackn, c->rtt_seq)) {
			c->flags &= (uint8_t)~F_TIMING;
			rtt_sample(c, now - c->rtt_start);
		}
		c->retries = 0;
		c->timer = 0;
		c->flags &= (uint8_t)~F_ONE;
		if (c->snd_nxt != c->snd_una) {
			arm(c, now);	// RFC 6298 5.3
		}
		acked_data = d > 0;
		if (fin_acked) {
			if (c->state == TCP_FIN_WAIT_1) {
				c->state = TCP_FIN_WAIT_2;
				c->timer = now + TCP_FIN_WAIT_2_MS;
			} else if (c->state == TCP_CLOSING) {
				time_wait(c, now);
				return;
			} else if (c->state == TCP_LAST_ACK) {
				c->state = TCP_CLOSED;
				c->flags = 0;
				return;
			}
		}
	}
	if (acked_data && (c->flags & F_OWNED) && (c->app->sent != NULL)) {
		c->app->sent(i);
		if (!same(i, gen)) {
			return;
		}
	}

	// The data (3.10.7.4, seventh check): in order only
	bool fin = (flags & FIN) != 0;
	bool receiving = (c->state == TCP_ESTABLISHED) || (c->state == TCP_FIN_WAIT_1) || (c->state == TCP_FIN_WAIT_2);

	if ((len > 0) || fin) {
		if (SEQ_LT(seq, c->rcv_nxt)) {
			uint32_t skip = c->rcv_nxt - seq;

			if (skip > (uint32_t)len) {
				len = 0;	// all old, a FIN too (it sits at seq + len)
				fin = false;
			} else {
				data += skip;
				len -= (int)skip;
			}
			seq = c->rcv_nxt;
		}
		c->flags |= F_ACK_NOW;
		if (seq != c->rcv_nxt) {
			len = 0;	// out of order: dropped, the ACK asks for RCV.NXT again
			fin = false;
		}
	}
	if (receiving && (len > 0)) {
		int take = window(i);

		if (take > len) {
			take = len;
		}
		if (take < len) {
			fin = false;	// the FIN lies beyond what we took
		}
		c->rcv_nxt += (uint32_t)take;
		if ((take > 0) && (c->flags & F_OWNED) && (c->app->recv != NULL)) {
			c->app->recv(i, data, take);
			if (!same(i, gen)) {
				return;
			}
		}
	} else if (len > 0) {
		fin = false;	// nothing more is taken after the peer's FIN: acknowledged, not delivered
	}
	if (fin && receiving) {
		c->rcv_nxt++;
		c->flags |= F_ACK_NOW;
		if (c->state == TCP_ESTABLISHED) {
			c->state = TCP_CLOSE_WAIT;
			if ((c->flags & F_OWNED) && (c->app->peer_closed != NULL)) {
				c->app->peer_closed(i);
				if (!same(i, gen)) {
					return;
				}
			}
		} else if (c->state == TCP_FIN_WAIT_1) {
			c->state = TCP_CLOSING;	// our FIN not acknowledged yet (else we'd be in FIN_WAIT_2)
		} else {
			time_wait(c, now);
			send_segment(i, ACK, c->snd_nxt, 0, 0);
			return;
		}
	}
	output(i, now);
}

void tcp_input(uint32_t src, const uint8_t *seg, int len, uint32_t now_ms)
{
	if ((io == NULL) || (len < TCP_HEADER)) {
		return;
	}
	int hdr = (seg[12] >> 4) * 4;

	if ((hdr < TCP_HEADER) || (hdr > len)) {
		return;
	}
	if (tcp_checksum(src, io->local_ip(), seg, len) != 0) {
		stats.bad_checksum++;
		return;
	}
	stats.segs_in++;

	uint16_t sport = get16(&seg[0]);
	uint16_t dport = get16(&seg[2]);
	uint32_t seq = get32(&seg[4]);
	uint32_t ackn = get32(&seg[8]);
	uint8_t flags = seg[13];
	uint16_t wnd = get16(&seg[14]);
	int dlen = len - hdr;

	for (int i = 0; i < TCP_CONNS; i++) {
		conn_t *c = &conns[i];

		if ((c->state != TCP_CLOSED) && (c->rip == src) && (c->rport == sport) && (c->lport == dport)) {
			if ((c->state == TCP_TIME_WAIT) && (flags & SYN) && !(flags & ACK)) {
				c->state = TCP_CLOSED;	// a new incarnation (RFC 9293 3.6.1): take it as a fresh SYN
				break;
			}
			segment_for(i, flags, seq, ackn, wnd, seg, hdr, &seg[hdr], dlen, now_ms);
			return;
		}
	}
	if (flags & RST) {
		return;
	}
	const tcp_app_t *app = NULL;

	for (int l = 0; l < TCP_LISTENERS; l++) {
		if ((listeners[l].app != NULL) && (listeners[l].port == dport)) {
			app = listeners[l].app;
		}
	}
	if ((app == NULL) || !(flags & SYN) || (flags & ACK)) {
		// No such connection or port (3.10.7.1)
		if (flags & ACK) {
			send_reset(src, dport, sport, ackn, 0, false);
		} else {
			send_reset(src, dport, sport, 0, seq + (uint32_t)dlen + ((flags & SYN) ? 1u : 0u) + ((flags & FIN) ? 1u : 0u), true);
		}
		if (flags & SYN) {
			stats.refused++;
		}
		return;
	}
	int i = alloc();

	if (i < 0) {
		stats.no_slot++;	// no RST: the client tries its SYN again a second later
		return;
	}
	setup(i, app, now_ms);
	conn_t *c = &conns[i];

	c->rip = src;
	c->rport = sport;
	c->lport = dport;
	c->rcv_nxt = seq + 1u;
	c->snd_wnd = wnd;
	parse_mss(c, seg, hdr);
	c->state = TCP_SYN_RCVD;
	c->rtt_start = now_ms;
	output(i, now_ms);
}

static void expire(int i, uint32_t now)
{
	conn_t *c = &conns[i];

	switch (c->state) {
	case TCP_TIME_WAIT:
		c->state = TCP_CLOSED;
		return;
	case TCP_FIN_WAIT_2:
		c->state = TCP_CLOSED;	// the peer never closed its side; the application is long gone
		c->flags = 0;
		return;
	default:
		break;
	}
	c->timer = 0;
	bool syn = (c->state == TCP_SYN_SENT) || (c->state == TCP_SYN_RCVD);
	bool persist = !syn && (c->snd_wnd == 0) && (c->txlen > 0);

	if (!syn && !persist && (c->snd_nxt == c->snd_una)) {
		return;	// nothing outstanding
	}
	if (persist && ((now - c->last_rx) < c->rto)) {
		c->retries = 0;	// the peer answers the probes: its window stays shut as long as it likes
	} else if (++c->retries > (syn ? TCP_SYN_RETRIES : TCP_RETRIES)) {
		stats.timeouts++;
		if (!syn) {
			send_reset(c->rip, c->lport, c->rport, c->snd_nxt, 0, false);
		}
		if (c->state == TCP_SYN_RCVD) {
			c->state = TCP_CLOSED;	// the application never saw it
			c->flags = 0;
		} else {
			drop(i, true);
		}
		return;
	}
	c->rto = (uint16_t)(((uint32_t)c->rto * 2u > TCP_RTO_MAX_MS) ? TCP_RTO_MAX_MS : (uint32_t)c->rto * 2u);
	c->flags &= (uint8_t)~F_TIMING;	// Karn: no sample from a retransmitted segment
	stats.retransmits++;
	c->snd_nxt = syn ? c->iss : c->snd_una;	// go back to the oldest unacknowledged byte
	c->txsent = 0;
	c->flags = (uint8_t)((c->flags & ~F_FIN_SENT) | (persist ? F_PROBE : F_ONE));
	output(i, now);
	if (c->timer == 0) {
		arm(c, now);
	}
}

void tcp_poll(uint32_t now_ms)
{
	if (io == NULL) {
		return;
	}
	for (int i = 0; i < TCP_CONNS; i++) {
		conn_t *c = &conns[i];

		if (c->state == TCP_CLOSED) {
			continue;
		}
		if (DUE(now_ms, c->timer)) {
			expire(i, now_ms);
			if (c->state == TCP_CLOSED) {
				continue;
			}
		}
		// Keepalive (RFC 1122 4.2.3.6): a segment one byte behind, which the peer must acknowledge
		if (((c->state == TCP_ESTABLISHED) || (c->state == TCP_CLOSE_WAIT)) && (c->snd_nxt == c->snd_una) &&
		    ((now_ms - c->last_rx) >= TCP_KEEPALIVE_MS) && ((c->ka_next == 0) || DUE(now_ms, c->ka_next))) {
			if (c->probes >= TCP_KEEPALIVE_PROBES) {
				stats.timeouts++;
				send_reset(c->rip, c->lport, c->rport, c->snd_nxt, 0, false);
				drop(i, true);
				continue;
			}
			c->probes++;
			c->ka_next = now_ms + TCP_KEEPALIVE_INTVL_MS;
			if (c->ka_next == 0) {
				c->ka_next = 1;
			}
			send_segment(i, ACK, c->snd_nxt - 1u, 0, 0);
		}
		if (c->flags & (F_OUTPUT | F_ACK_NOW)) {
			output(i, now_ms);
		}
	}
}

void tcp_init(const tcp_io_t *ops, uint32_t salt)
{
	io = ops;
	isn_salt = salt;
	next_port = (uint16_t)(49152u + (salt % 8192u));
	for (int i = 0; i < TCP_CONNS; i++) {
		conns[i].state = TCP_CLOSED;
		conns[i].flags = 0;
	}
	memset(listeners, 0, sizeof(listeners));
	memset(&stats, 0, sizeof(stats));
}

bool tcp_listen(uint16_t port, const tcp_app_t *app)
{
	for (int l = 0; l < TCP_LISTENERS; l++) {
		if ((listeners[l].app == NULL) || (listeners[l].port == port)) {
			listeners[l].port = port;
			listeners[l].app = app;
			return true;
		}
	}
	return false;
}

int tcp_connect(uint32_t ip, uint16_t port, const tcp_app_t *app, uint32_t now_ms)
{
	int i = (io != NULL) ? alloc() : -1;

	if (i < 0) {
		return -1;
	}
	setup(i, app, now_ms);
	conn_t *c = &conns[i];

	c->rip = ip;
	c->rport = port;
	c->lport = next_port;
	next_port = (uint16_t)((next_port >= 65535u) ? 49152u : (next_port + 1u));
	c->state = TCP_SYN_SENT;
	c->flags = F_OWNED | F_OUTPUT;
	c->snd_wnd = 0;
	c->rtt_start = now_ms;
	return i;
}

int tcp_space(int c)
{
	if (!valid(c) || !(conns[c].flags & F_OWNED) || (conns[c].flags & F_FIN_PENDING)) {
		return 0;
	}
	uint8_t s = conns[c].state;

	if ((s != TCP_ESTABLISHED) && (s != TCP_CLOSE_WAIT)) {
		return 0;
	}
	return TCP_TXBUF - conns[c].txlen;
}

int tcp_unacked(int c)
{
	return valid(c) ? conns[c].txlen : 0;
}

int tcp_write(int i, const void *data, int len)
{
	int room = tcp_space(i);

	if (len > room) {
		len = room;
	}
	if (len <= 0) {
		return 0;
	}
	conn_t *c = &conns[i];
	const uint8_t *p = data;

	for (int k = 0; k < len; k++) {
		c->txbuf[(c->txhead + c->txlen + k) % TCP_TXBUF] = p[k];
	}
	c->txlen = (uint16_t)(c->txlen + len);
	c->flags |= F_OUTPUT;
	return len;
}

void tcp_close(int i)
{
	if (!valid(i)) {
		return;
	}
	conn_t *c = &conns[i];

	c->flags &= (uint8_t)~F_OWNED;
	switch (c->state) {
	case TCP_SYN_SENT:
		c->state = TCP_CLOSED;
		c->flags = 0;
		break;
	case TCP_SYN_RCVD:
	case TCP_ESTABLISHED:
	case TCP_CLOSE_WAIT:
		c->flags |= F_FIN_PENDING | F_OUTPUT;
		break;
	default:
		break;
	}
}

void tcp_abort(int i)
{
	if (!valid(i)) {
		return;
	}
	conn_t *c = &conns[i];

	if ((c->state != TCP_SYN_SENT) && (c->state != TCP_TIME_WAIT)) {
		send_reset(c->rip, c->lport, c->rport, c->snd_nxt, 0, false);
	}
	c->state = TCP_CLOSED;
	c->flags = 0;
}

void tcp_abort_all(void)
{
	for (int i = 0; i < TCP_CONNS; i++) {
		if (conns[i].state != TCP_CLOSED) {
			drop(i, true);
		}
	}
}

uint8_t tcp_state(int c)
{
	return valid(c) ? conns[c].state : TCP_CLOSED;
}

uint32_t tcp_remote_ip(int c)
{
	return valid(c) ? conns[c].rip : 0;
}

uint16_t tcp_remote_port(int c)
{
	return valid(c) ? conns[c].rport : 0;
}

uint16_t tcp_local_port(int c)
{
	return valid(c) ? conns[c].lport : 0;
}

void tcp_get_stats(tcp_stats_t *s)
{
	*s = stats;
}

int tcp_open_count(void)
{
	int n = 0;

	for (int i = 0; i < TCP_CONNS; i++) {
		n += (conns[i].state != TCP_CLOSED) && (conns[i].state != TCP_TIME_WAIT);
	}
	return n;
}
