// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A minimal PPP with IPCP over HDLC-like framing (see ppp.h). Written from RFC 1661, RFC 1662 and RFC 1332.
#include <string.h>
#include "net/ppp.h"

#define FLAG		0x7E
#define ESC		0x7D
#define PROTO_IP	0x0021
#define PROTO_LCP	0xC021
#define PROTO_IPCP	0x8021
#define FCS_INIT	0xFFFFu
#define FCS_GOOD	0xF0B8u		// RFC 1662 C.2: the FCS over the frame and its own FCS
#define GAP_MS		500u		// a frame that stops this long is abandoned: the next byte starts a new one
#define DEFAULT_MRU	1500
#define ACCM_ALL	0xFFFFFFFFu

enum { CONF_REQ = 1, CONF_ACK, CONF_NAK, CONF_REJ, TERM_REQ, TERM_ACK, CODE_REJ, PROTO_REJ, ECHO_REQ, ECHO_REP,
       DISCARD_REQ };
enum { LCP_MRU = 1, LCP_ACCM = 2, LCP_MAGIC = 5, IPCP_ADDRESS = 3 };
enum { CHECK, NAK_WRITE, APPLY };	// what an option check does besides its verdict

typedef struct {
	uint16_t protocol;
	uint8_t state;
	uint8_t id;		// of our last Configure- or Terminate-Request
	uint8_t restarts;
	uint8_t failures;	// Configure-Naks sent in a row
	uint8_t ask;		// the options we still ask for: bit n = type n
	bool timer;
	uint32_t timer_ms;
} fsm_t;

static fsm_t lcp, ipcp;
// The frame being received: address, control and protocol at rx[RX_FIRST], the information field at rx[PPP_HEADROOM]
// (so a reply or a header can be built in place in front of it), the FCS after it
#define RX_FIRST	(PPP_HEADROOM - 4)
static uint8_t *rx;
static uint16_t rx_len;
static uint16_t rx_fcs;
static bool rx_esc, rx_bad;
static uint32_t rx_accm;	// control characters the peer escapes: bare ones are line noise (RFC 1662 7.1)
static uint32_t tx_accm;	// the peer's map, for everything but LCP
static uint32_t want_accm;	// the map we ask the peer for
static uint16_t peer_mru;
static uint32_t magic;
static uint8_t other_id;	// Code-Reject, Protocol-Reject, Echo-Request
static bool running;
static uint32_t clock_ms, last_rx_ms, last_echo_ms, last_byte_ms;
static ppp_stats_t stats;

static uint16_t get16(const uint8_t *p)
{
	return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t get32(const uint8_t *p)
{
	return ((uint32_t)get16(p) << 16) | get16(p + 2);
}

static void put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

static void put32(uint8_t *p, uint32_t v)
{
	put16(p, (uint16_t)(v >> 16));
	put16(p + 2, (uint16_t)v);
}

// RFC 1662 C.2, bit by bit (x^16 + x^12 + x^5 + 1, reflected)
static uint16_t fcs_byte(uint16_t fcs, uint8_t b)
{
	fcs ^= b;
	for (int i = 0; i < 8; i++) {
		fcs = (fcs & 1u) ? (uint16_t)((fcs >> 1) ^ 0x8408u) : (uint16_t)(fcs >> 1);
	}
	return fcs;
}

// ---- sending ----

typedef struct {
	uint8_t buf[48];
	uint8_t n;
	uint16_t fcs;
	uint32_t accm;
} tx_t;

static void tx_raw(tx_t *t, uint8_t b)
{
	if (t->n == sizeof(t->buf)) {
		ppp_write(t->buf, t->n);
		t->n = 0;
	}
	t->buf[t->n++] = b;
}

static void tx_byte(tx_t *t, uint8_t b)
{
	if ((b == FLAG) || (b == ESC) || ((b < 0x20) && ((t->accm >> b) & 1u))) {
		tx_raw(t, ESC);
		b ^= 0x20;
	}
	tx_raw(t, b);
}

static void tx_data(tx_t *t, uint8_t b)
{
	t->fcs = fcs_byte(t->fcs, b);
	tx_byte(t, b);
}

static void send_frame(uint16_t protocol, const uint8_t *info, int len)
{
	tx_t t;

	t.n = 0;
	t.fcs = FCS_INIT;
	// LCP goes as if nothing were negotiated: every control character escaped (RFC 1662 7.1)
	t.accm = (protocol == PROTO_LCP) ? ACCM_ALL : tx_accm;
	tx_raw(&t, FLAG);
	tx_data(&t, 0xFF);
	tx_data(&t, 0x03);
	tx_data(&t, (uint8_t)(protocol >> 8));
	tx_data(&t, (uint8_t)protocol);
	for (int i = 0; i < len; i++) {
		tx_data(&t, info[i]);
	}
	uint16_t fcs = (uint16_t)~t.fcs;

	tx_byte(&t, (uint8_t)fcs);	// least significant octet first
	tx_byte(&t, (uint8_t)(fcs >> 8));
	tx_raw(&t, FLAG);
	ppp_write(t.buf, t.n);
	stats.tx_frames++;
}

// A control packet: the 4 byte header goes in p[0..3], the data is already at p[4]
static void send_packet(uint16_t protocol, uint8_t code, uint8_t id, uint8_t *p, int len)
{
	p[0] = code;
	p[1] = id;
	put16(p + 2, (uint16_t)len);
	send_frame(protocol, p, len);
}

// ---- the option negotiation automaton (RFC 1661 4), one per protocol ----

static void finish(void)
{
	running = false;
	lcp.state = PPP_INITIAL;
	ipcp.state = PPP_INITIAL;
	lcp.timer = false;
	ipcp.timer = false;
}

static void timer_start(fsm_t *f)
{
	f->timer = true;
	f->timer_ms = clock_ms;
}

static void irc(fsm_t *f, uint8_t n)
{
	f->restarts = n;
}

static void scr(fsm_t *f)
{
	uint8_t p[4 + 4 + 6 + 6];
	uint8_t *o = p + 4;

	if (f == &lcp) {
		if (f->ask & (1u << LCP_MRU)) {
			o[0] = LCP_MRU;
			o[1] = 4;
			put16(o + 2, PPP_MRU);
			o += 4;
		}
		if (f->ask & (1u << LCP_ACCM)) {
			o[0] = LCP_ACCM;
			o[1] = 6;
			put32(o + 2, want_accm);
			o += 6;
		}
		if (f->ask & (1u << LCP_MAGIC)) {
			o[0] = LCP_MAGIC;
			o[1] = 6;
			put32(o + 2, magic);
			o += 6;
		}
	} else if ((f->ask & (1u << IPCP_ADDRESS)) && (ppp_local_ip() != 0)) {
		o[0] = IPCP_ADDRESS;
		o[1] = 6;
		put32(o + 2, ppp_local_ip());
		o += 6;
	}
	if (f->restarts > 0) {
		f->restarts--;
	}
	timer_start(f);
	send_packet(f->protocol, CONF_REQ, ++f->id, p, (int)(o - p));
}

static void str(fsm_t *f)
{
	uint8_t p[4];

	if (f->restarts > 0) {
		f->restarts--;
	}
	timer_start(f);
	send_packet(f->protocol, TERM_REQ, ++f->id, p, 4);
}

static void sta(fsm_t *f, uint8_t id)
{
	uint8_t p[4];

	send_packet(f->protocol, TERM_ACK, id, p, 4);
}

static void tlu(fsm_t *f)
{
	if (f == &lcp) {
		rx_accm = (lcp.ask & (1u << LCP_ACCM)) ? want_accm : ACCM_ALL;
		// The network layer starts (IPCP's Up event in its Initial state is irc, scr)
		ipcp.state = PPP_REQ_SENT;
		ipcp.ask = 1u << IPCP_ADDRESS;
		ipcp.failures = 0;
		irc(&ipcp, PPP_MAX_CONFIGURE);
		scr(&ipcp);
	}
}

static void tld(fsm_t *f)
{
	if (f == &lcp) {
		rx_accm = ACCM_ALL;
		ipcp.state = PPP_INITIAL;
		ipcp.timer = false;
	}
}

static void lcp_close(void);

static void tlf(fsm_t *f)
{
	f->timer = false;
	if (f == &lcp) {
		finish();
	} else {
		lcp_close();	// IPCP gave up: nothing left to carry
	}
}

static void opened(fsm_t *f)
{
	f->state = PPP_OPENED;
	f->timer = false;
	f->failures = 0;
	tlu(f);
}

static void ev_close(fsm_t *f)
{
	switch (f->state) {
	case PPP_OPENED:
		tld(f);
		/* fall through */
	case PPP_REQ_SENT:
	case PPP_ACK_RCVD:
	case PPP_ACK_SENT:
		irc(f, PPP_MAX_TERMINATE);
		str(f);
		f->state = PPP_CLOSING;
		break;
	case PPP_STOPPING:
		f->state = PPP_CLOSING;
		break;
	case PPP_CLOSING:
		break;
	default:
		f->state = PPP_CLOSED;
		tlf(f);
		break;
	}
}

static void lcp_close(void)
{
	ev_close(&lcp);
}

static void ev_timeout(fsm_t *f)
{
	if (f->restarts > 0) {
		switch (f->state) {
		case PPP_CLOSING:
		case PPP_STOPPING:
			str(f);
			break;
		case PPP_REQ_SENT:
		case PPP_ACK_RCVD:
			scr(f);
			f->state = PPP_REQ_SENT;
			break;
		case PPP_ACK_SENT:
			scr(f);
			break;
		default:
			f->timer = false;
			break;
		}
		return;
	}
	f->state = (f->state == PPP_CLOSING) ? PPP_CLOSED : PPP_STOPPED;
	tlf(f);
}

// One option of the peer's Configure-Request (o[1] >= 2 checked): CONF_ACK, CONF_NAK (with NAK_WRITE: the value we
// want written over it) or CONF_REJ; APPLY takes the value
static int peer_option(fsm_t *f, uint8_t *o, int mode)
{
	if (f == &lcp) {
		switch (o[0]) {
		case LCP_MRU:
			if (o[1] != 4) {
				return CONF_REJ;
			}
			if (mode == APPLY) {
				peer_mru = get16(o + 2);
			}
			return CONF_ACK;
		case LCP_ACCM:
			if (o[1] != 6) {
				return CONF_REJ;
			}
			if (mode == APPLY) {
				tx_accm = get32(o + 2);
			}
			return CONF_ACK;
		case LCP_MAGIC: {
			if (o[1] != 6) {
				return CONF_REJ;
			}
			uint32_t v = get32(o + 2);

			if ((v == 0) || (v == magic)) {	// zero is illegal; ours may mean a looped-back line (RFC 1661 6.4)
				if (mode == NAK_WRITE) {
					put32(o + 2, ~magic);
				}
				return CONF_NAK;
			}
			return CONF_ACK;
		}
		}
		return CONF_REJ;
	}
	if ((o[0] == IPCP_ADDRESS) && (o[1] == 6)) {
		uint32_t peer = ppp_peer_ip();

		if (peer == 0) {
			return CONF_REJ;	// no address to give yet
		}
		if (get32(o + 2) != peer) {
			if (mode == NAK_WRITE) {
				put32(o + 2, peer);
			}
			return CONF_NAK;
		}
		return CONF_ACK;
	}
	return CONF_REJ;
}

// The peer's Configure-Request p (len bytes, header included) becomes our answer in place: the code and the options
// that go back (Ack: all of them, their values taken; Nak: the ones we want otherwise; Reject: the ones we don't
// know). Returns its length, 0 for a malformed request (silently discarded)
static int answer_request(fsm_t *f, uint8_t *p, int len)
{
	uint8_t *end = p + len;
	int worst = CONF_ACK;

	for (uint8_t *o = p + 4; o < end; o += o[1]) {
		if (((end - o) < 2) || (o[1] < 2) || (o[1] > (end - o))) {
			return 0;
		}
		int v = peer_option(f, o, CHECK);

		if (v > worst) {
			worst = v;
		}
	}
	int keep = worst;	// the verdict of the options that go back

	if (worst == CONF_NAK) {
		if (++f->failures > PPP_MAX_FAILURE) {
			worst = CONF_REJ;	// RFC 1661 4.6 Max-Failure: the Nak'd options are rejected from now on
		}
	} else if (worst == CONF_ACK) {
		f->failures = 0;
		if (f == &lcp) {
			peer_mru = DEFAULT_MRU;	// what the request leaves out is the default
			tx_accm = ACCM_ALL;
		}
	}
	uint8_t *w = p + 4;

	for (uint8_t *o = p + 4; o < end;) {
		uint8_t n = o[1];
		int v = peer_option(f, o, (worst == CONF_ACK) ? APPLY : (worst == CONF_NAK) ? NAK_WRITE : CHECK);

		if (v == keep) {
			memmove(w, o, n);
			w += n;
		}
		o += n;
	}
	p[0] = (uint8_t)worst;
	put16(p + 2, (uint16_t)(w - p));
	return (int)(w - p);
}

// The peer's Configure-Nak or -Reject of our request
static void our_options(fsm_t *f, uint8_t code, const uint8_t *p, int len)
{
	const uint8_t *end = p + len;

	for (const uint8_t *o = p + 4; ((end - o) >= 2) && (o[1] >= 2) && (o[1] <= (end - o)); o += o[1]) {
		if (o[0] >= 8) {
			continue;	// not one we ask for
		}
		if (code == CONF_REJ) {
			f->ask &= (uint8_t)~(1u << o[0]);
		} else if ((f == &lcp) && (o[0] == LCP_ACCM) && (o[1] == 6)) {
			want_accm = get32(o + 2);
		} else if ((f == &lcp) && (o[0] == LCP_MAGIC)) {
			magic = (magic * 1103515245u) + 12345u;	// a new number (RFC 1661 6.4)
			if (magic == 0) {
				magic = 1;
			}
		}
		// The MRU and our IP address stay what they are: the gateway's frame size and address
	}
}

static void rcr(fsm_t *f, uint8_t *p, int len)
{
	uint8_t id = p[1];

	switch (f->state) {
	case PPP_CLOSING:
	case PPP_STOPPING:
		return;
	case PPP_CLOSED:
		sta(f, id);
		return;
	default:
		break;
	}
	int n = answer_request(f, p, len);

	if (n == 0) {
		return;
	}
	bool ack = (p[0] == CONF_ACK);

	if ((f->state == PPP_OPENED) || (f->state == PPP_STOPPED)) {
		if (f->state == PPP_OPENED) {
			tld(f);
		}
		irc(f, PPP_MAX_CONFIGURE);
		scr(f);
		f->state = PPP_REQ_SENT;
	}
	send_frame(f->protocol, p, n);
	if (f->state == PPP_ACK_RCVD) {
		if (ack) {
			opened(f);
		}
	} else {
		f->state = ack ? PPP_ACK_SENT : PPP_REQ_SENT;
	}
}

static void rca(fsm_t *f, uint8_t id)
{
	switch (f->state) {
	case PPP_CLOSED:
	case PPP_STOPPED:
		sta(f, id);
		break;
	case PPP_REQ_SENT:
		irc(f, PPP_MAX_CONFIGURE);
		f->state = PPP_ACK_RCVD;
		break;
	case PPP_ACK_SENT:
		irc(f, PPP_MAX_CONFIGURE);
		opened(f);
		break;
	case PPP_OPENED:
		tld(f);
		/* fall through */
	case PPP_ACK_RCVD:
		scr(f);
		f->state = PPP_REQ_SENT;
		break;
	}
}

static void rcn(fsm_t *f, uint8_t id)
{
	switch (f->state) {
	case PPP_CLOSED:
	case PPP_STOPPED:
		sta(f, id);
		break;
	case PPP_REQ_SENT:
	case PPP_ACK_SENT:
		irc(f, PPP_MAX_CONFIGURE);
		scr(f);
		break;
	case PPP_OPENED:
		tld(f);
		/* fall through */
	case PPP_ACK_RCVD:
		scr(f);
		f->state = PPP_REQ_SENT;
		break;
	}
}

static void rtr(fsm_t *f, uint8_t id)
{
	switch (f->state) {
	case PPP_OPENED:
		tld(f);
		irc(f, 0);	// zrc: one restart time in Stopping, then finished
		timer_start(f);
		f->state = PPP_STOPPING;
		break;
	case PPP_ACK_RCVD:
	case PPP_ACK_SENT:
		f->state = PPP_REQ_SENT;
		break;
	}
	sta(f, id);
}

static void rta(fsm_t *f)
{
	switch (f->state) {
	case PPP_CLOSING:
		f->state = PPP_CLOSED;
		tlf(f);
		break;
	case PPP_STOPPING:
		f->state = PPP_STOPPED;
		tlf(f);
		break;
	case PPP_ACK_RCVD:
		f->state = PPP_REQ_SENT;
		break;
	case PPP_OPENED:
		tld(f);
		scr(f);
		f->state = PPP_REQ_SENT;
		break;
	}
}

// A reply that carries a received packet p (len bytes) after a header of hdr bytes, built in place in the headroom
// in front of it, cut to the peer's MRU
static void send_reject(uint8_t *p, int len, int hdr, uint8_t code, uint16_t protocol)
{
	uint8_t *r = p - hdr;
	int n = len + hdr;

	if (n > peer_mru) {
		n = peer_mru;
	}
	if (hdr == 6) {
		put16(r + 4, protocol);
	}
	send_packet(hdr == 6 ? PROTO_LCP : protocol, code, ++other_id, r, n);
}

static void fsm_input(fsm_t *f, uint8_t *p, int n)
{
	if (n < 4) {
		return;
	}
	int len = get16(p + 2);

	if ((len < 4) || (len > n) || (f->state == PPP_INITIAL)) {
		return;	// short, or IPCP while LCP isn't open: silently discarded
	}
	uint8_t code = p[0];

	switch (code) {
	case CONF_REQ:
		rcr(f, p, len);
		return;
	case CONF_ACK:
		if (p[1] == f->id) {
			rca(f, p[1]);
		}
		return;
	case CONF_NAK:
	case CONF_REJ:
		if (p[1] == f->id) {
			our_options(f, code, p, len);
			rcn(f, p[1]);
		}
		return;
	case TERM_REQ:
		rtr(f, p[1]);
		return;
	case TERM_ACK:
		rta(f);
		return;
	case CODE_REJ:
		return;		// RXJ+: the codes we send are all required ones
	default:
		break;
	}
	if (f == &lcp) {
		if (code == PROTO_REJ) {
			if ((len >= 6) && (get16(p + 4) == PROTO_IPCP)) {
				lcp_close();	// the peer won't do IPv4: nothing to carry
			}
			return;
		}
		if (code == ECHO_REQ) {
			if ((f->state == PPP_OPENED) && (len >= 8)) {
				put32(p + 4, magic);
				send_packet(PROTO_LCP, ECHO_REP, p[1], p, len);
			}
			return;
		}
		if ((code == ECHO_REP) || (code == DISCARD_REQ)) {
			return;
		}
	}
	send_reject(p, len, 4, CODE_REJ, f->protocol);
}

static void frame(int n)
{
	uint8_t *f = &rx[RX_FIRST];

	if ((f[0] != 0xFF) || (f[1] != 0x03)) {
		stats.rx_errors++;	// address and control are never compressed (we reject ACFC)
		return;
	}
	stats.rx_frames++;
	last_rx_ms = clock_ms;

	uint16_t protocol = get16(f + 2);
	uint8_t *info = f + 4;
	int len = n - 4;

	if (protocol == PROTO_LCP) {
		fsm_input(&lcp, info, len);
	} else if (lcp.state != PPP_OPENED) {
		// silently discarded until LCP is open
	} else if (protocol == PROTO_IPCP) {
		fsm_input(&ipcp, info, len);
	} else if (protocol == PROTO_IP) {
		if (ipcp.state == PPP_OPENED) {
			ppp_ip_in(info, len);
		}
	} else {
		send_reject(info, len, 6, PROTO_REJ, protocol);	// IPv6CP, CCP, anything else
	}
}

static void rx_reset(void)
{
	rx_len = 0;
	rx_fcs = FCS_INIT;
	rx_esc = false;
	rx_bad = false;
}

static void frame_end(void)
{
	if ((rx_len > 0) || rx_esc) {
		if (rx_bad || rx_esc || (rx_len < 6) || (rx_fcs != FCS_GOOD)) {
			stats.rx_errors++;	// bad FCS, too long, or aborted (RFC 1662 4.3: 0x7D then the flag)
		} else {
			frame(rx_len - 2);
		}
	}
	rx_reset();
}

// ---- the interface ----

void ppp_start(bool active, uint32_t salt, uint32_t now_ms, uint8_t *rx_buffer)
{
	rx = rx_buffer;
	clock_ms = now_ms;
	last_rx_ms = now_ms;
	last_echo_ms = now_ms;
	last_byte_ms = now_ms;
	rx_reset();
	rx_accm = ACCM_ALL;
	tx_accm = ACCM_ALL;
	want_accm = 0;
	peer_mru = DEFAULT_MRU;
	magic = (salt * 2654435761u) ^ 0x4F445221u;
	if ((magic == 0) || (magic == ACCM_ALL)) {
		magic = 0x4F445221u;
	}
	memset(&lcp, 0, sizeof(lcp));
	memset(&ipcp, 0, sizeof(ipcp));
	memset(&stats, 0, sizeof(stats));
	lcp.protocol = PROTO_LCP;
	lcp.ask = (1u << LCP_MRU) | (1u << LCP_ACCM) | (1u << LCP_MAGIC);
	ipcp.protocol = PROTO_IPCP;
	running = true;
	if (active) {
		lcp.state = PPP_REQ_SENT;
		irc(&lcp, PPP_MAX_CONFIGURE);
		scr(&lcp);
	} else {
		lcp.state = PPP_STOPPED;	// the passive open: the peer's Configure-Request starts it
	}
}

size_t ppp_input(const uint8_t *buf, size_t len, uint32_t now_ms)
{
	clock_ms = now_ms;
	if (((rx_len > 0) || rx_esc) && ((now_ms - last_byte_ms) >= GAP_MS)) {
		stats.rx_errors++;	// a frame that never ended: whatever comes now starts afresh
		rx_reset();
	}
	last_byte_ms = now_ms;
	for (size_t i = 0; i < len; i++) {
		uint8_t b = buf[i];

		if (!running) {
			return i;
		}
		if (b == FLAG) {
			frame_end();
			continue;
		}
		if ((rx_len == 0) && !rx_esc && ppp_foreign_start(b)) {
			finish();	// another protocol's request where a frame should start: the host moved on
			return i;
		}
		if (b == ESC) {
			rx_esc = true;
			continue;
		}
		if (rx_esc) {
			b ^= 0x20;
			rx_esc = false;
		} else if ((b < 0x20) && ((rx_accm >> b) & 1u)) {
			continue;	// inserted by the line, not the peer
		}
		if (rx_len < (PPP_RX_BUFFER - RX_FIRST)) {
			rx[RX_FIRST + rx_len++] = b;
			rx_fcs = fcs_byte(rx_fcs, b);
		} else {
			rx_bad = true;
		}
	}
	return len;
}

void ppp_poll(uint32_t now_ms)
{
	clock_ms = now_ms;
	if (!running) {
		return;
	}
	if ((now_ms - last_rx_ms) >= PPP_DEAD_MS) {
		finish();
		return;
	}
	for (int i = 0; (i < 2) && running; i++) {
		fsm_t *f = (i == 0) ? &lcp : &ipcp;

		if (f->timer && ((now_ms - f->timer_ms) >= PPP_RESTART_MS)) {
			ev_timeout(f);
		}
	}
	if ((lcp.state == PPP_OPENED) && ((now_ms - last_rx_ms) >= PPP_ECHO_MS) && ((now_ms - last_echo_ms) >= PPP_ECHO_MS)) {
		uint8_t p[8];

		put32(p + 4, magic);
		send_packet(PROTO_LCP, ECHO_REQ, ++other_id, p, 8);
		last_echo_ms = now_ms;
	}
}

void ppp_close(uint32_t now_ms)
{
	clock_ms = now_ms;
	if (running) {
		lcp_close();
	}
}

bool ppp_running(void)
{
	return running;
}

bool ppp_ip_up(void)
{
	return running && (ipcp.state == PPP_OPENED);
}

bool ppp_send_ip(const uint8_t *packet, int length)
{
	if (!ppp_ip_up() || (length > peer_mru)) {
		return false;
	}
	send_frame(PROTO_IP, packet, length);
	return true;
}

void ppp_stats(ppp_stats_t *s)
{
	*s = stats;
	s->lcp = lcp.state;
	s->ipcp = ipcp.state;
}
