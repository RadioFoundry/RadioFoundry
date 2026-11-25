// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The debug link over the air (docs/spec/debuglink.md 7), its glue: UDP DL_DMRIP_PORT over the air to our own DMR ID
// (handed over by hostlink_net.c), a one-datagram mailbox the radio task fills and the host link task empties (as the SNMP agent's), the replies
// in chunks from the debug link's own reply buffer, the DETOUR and DMRIP commands and the dmrip view section. The
// pure parts (datagram, key, replay, policy, the detour runner) are dl_dmrip.c.
#include <string.h>
#include "config.h"
#include "debuglink/debuglink.h"
#include "debuglink/debuglink_proto.h"
#include "debuglink/dl_dmrip.h"
#if CONFIG_DEBUGLINK_DMRIP
#include "board.h"
#include "hal_time.h"
#include "radio/radio.h"
#include "input/input.h"
#include "settings/settings.h"
#include "dmr/ipGateway.h"
#include "dmr/dmrAirQueue.h"
#include "dmr/dmrDataService.h"

#define UDP_PROTO	17u
#define UDP_HDR		8u
#define GIVE_UP_MS	3000u	// a chunk the air queue had no room for this long is dropped (the host asks again)
#define HELLO_GAP_MS	1000u

enum { IO_FREE, IO_REQ, IO_SEND };

static uint8_t key[DL_DMRIP_KEY];
static uint8_t key_src;			// 0 none, 1 the chip UID's, 2 set over the cable
static dl_dmrip_replay_t replay;
static dl_detour_t detour;
static bool from_air;			// the request being handled came over the air

// The mailbox, and the datagram being sent from it (UDP header first)
static volatile uint8_t io_state;
static uint8_t io[UDP_HDR + DL_DMRIP_DGRAM_MAX];
static uint16_t io_len;
static uint32_t io_src;			// the sender's DMR ID
static uint32_t io_ip;			// the request's IPv4 source: where the answer goes (ipGateway.h)
static uint16_t io_port;

// The last reply, in the debug link's reply buffer while no cable request came since (reply_gen)
static struct {
	bool ok;
	uint32_t counter, gen, dst, ip, since;
	uint16_t port, len;
	uint8_t mask;			// chunks still to send
	uint8_t flags;
} out;
static uint32_t last_hello;

static struct {
	uint32_t rx, busy, bad_auth, not_ours, old, repeats, gone, refused, answered, chunks, dropped, hello;
} cnt;

static void uid_key(void)
{
	hal_chip_t c;
	uint8_t uid[12];
	key_src = 0;
	if (board.system == NULL || board.system->chip == NULL || !board.system->chip(&c)) {
		return;
	}
	for (unsigned i = 0; i < 3; i++) {
		dl_put32(uid + 4 * i, c.uid[i]);
	}
	static const uint8_t zero[12];
	if (memcmp(uid, zero, sizeof(uid)) == 0) {
		return;		// no UID: no transport until a key is set over the cable
	}
	dl_dmrip_kdf(uid, sizeof(uid), key);
	key_src = 1;
}

// --- the detour's hooks ---

static uint8_t item_dispatch(uint8_t cmd, const uint8_t *p, uint8_t len)
{
	uint8_t r[32];
	debuglink_dispatch(cmd, p, len, r, sizeof(r));
	return r[0];
}

static void core_mark(uint32_t backstop_ms)
{
	(void)settings_flush();		// what waits for its save goes now: the hold needs nothing unsaved
	(void)radio_req_detour(RADIO_DETOUR_MARK, backstop_ms);
}

static void core_return(void)
{
	(void)radio_req_detour(RADIO_DETOUR_RETURN, 0);
}

static bool core_armed(void)
{
	return radio_detour_armed();
}

static void ptt_release(void)
{
	(void)input_inject_ptt(false, 0);
}

// The RUN reply is on the air and gone: nothing of ours queued, none taken by the data service and waiting for the
// channel (listen before talk, the slot), the radio not transmitting
static bool reply_clear(void)
{
	return io_state != IO_SEND && airqCount(AIRQ_OWNER_GATEWAY) == 0 && !dmrDataServiceIsBusy() &&
	       !dmrDataServiceIpBusy() && radio_get_state() != RS_TX;
}

static const dl_detour_ops_t ops = {
	.dispatch = item_dispatch,
	.mark = core_mark,
	.ret = core_return,
	.armed = core_armed,
	.release = ptt_release,
	.clear = reply_clear,
};

// --- the air ---

// The radio task: copy and return. hostlink_net.c hands over only datagrams to our own ID and address
void debuglink_dmrip_air(uint32_t src, uint32_t src_ip, uint16_t src_port, const uint8_t *p, int len)
{
	cnt.rx++;
	if (io_state != IO_FREE || len <= 0 || (size_t)len > DL_DMRIP_DGRAM_MAX) {
		cnt.busy++;
		return;
	}
	memcpy(io + UDP_HDR, p, (size_t)len);
	io_len = (uint16_t)len;
	io_src = src;
	io_ip = dl_dmrip_reply_ip(src, src_ip);
	io_port = src_port;
	__sync_synchronize();	// complete before the flag says so
	io_state = IO_REQ;
}

// The datagram at io + UDP_HDR (n bytes) to the requester
static bool send_io(size_t n)
{
	uint8_t *u = io;
	size_t l = UDP_HDR + n;
	u[0] = (uint8_t)(DL_DMRIP_PORT >> 8);
	u[1] = (uint8_t)DL_DMRIP_PORT;
	u[2] = (uint8_t)(out.port >> 8);
	u[3] = (uint8_t)out.port;
	u[4] = (uint8_t)(l >> 8);
	u[5] = (uint8_t)l;
	u[6] = u[7] = 0;	// the checksum: dmrDataBuildIP works it out
	return ipGatewayIPToAir(false, out.dst, IPGW_AIR_SRC_OURS, out.ip, DMR_IP_TTL_DEFAULT, UDP_PROTO, io, (int)l);
}

// A reply with no frame in it (HELLO, GONE): body already at io + UDP_HDR + DL_DMRIP_HDR
static void send_short(uint8_t flags, uint32_t counter, uint16_t body)
{
	dl_dmrip_msg_t m = { (uint8_t)(DL_DMRIP_F_REPLY | flags), ipGatewayRadioId(), replay.nonce, counter, 0x01, NULL,
			     body };
	size_t n = dl_dmrip_seal(key, &m, io + UDP_HDR);
	out.dst = io_src;
	out.ip = io_ip;
	out.port = io_port;
	if (!send_io(n)) {
		cnt.dropped++;
	}
}

// The next chunk of the cached reply: false when none is left
static bool send_chunk(uint32_t now_ms)
{
	uint8_t count = (uint8_t)((out.len + DL_DMRIP_CHUNK - 1u) / DL_DMRIP_CHUNK);
	uint8_t i = 0;
	while (i < count && (out.mask & (1u << i)) == 0) {
		i++;
	}
	if (i >= count) {
		return false;
	}
	if (debuglink_reply_gen() != out.gen) {
		out.ok = false;		// a cable request took the buffer meanwhile: the host asks again and gets GONE
		cnt.dropped++;
		return false;
	}
	uint16_t at = (uint16_t)(i * DL_DMRIP_CHUNK);
	unsigned left = (unsigned)out.len - at;
	uint16_t n = (uint16_t)(left < DL_DMRIP_CHUNK ? left : DL_DMRIP_CHUNK);
	memcpy(io + UDP_HDR + DL_DMRIP_HDR, debuglink_reply_buffer() + at, n);
	dl_dmrip_msg_t m = { (uint8_t)(DL_DMRIP_F_REPLY | out.flags), ipGatewayRadioId(), replay.nonce, out.counter,
			     (uint8_t)((i << 4) | count), NULL, n };
	size_t len = dl_dmrip_seal(key, &m, io + UDP_HDR);
	if (send_io(len)) {
		out.mask &= (uint8_t)~(1u << i);
		out.since = now_ms;
		cnt.chunks++;
	} else if ((now_ms - out.since) >= GIVE_UP_MS) {
		out.mask = 0;		// the air queue stayed full: the host asks again for what it lacks
		cnt.dropped++;
		return false;
	}
	return true;
}

static void start_send(uint8_t mask, uint32_t now_ms)
{
	out.mask = mask;
	out.since = now_ms;
	io_state = IO_SEND;
}

static void handle_new(const dl_dmrip_msg_t *m, uint32_t now_ms)
{
	uint8_t cmd, seq;
	const uint8_t *p;
	uint16_t len;
	uint8_t *rb = debuglink_reply_buffer();
	size_t n;
	if (!dl_dmrip_frame(m->body, m->body_len, &cmd, &seq, &p, &len)) {
		cmd = DL_CMD_ERROR & (uint8_t)~DL_REPLY_FLAG;
		seq = 0;
		rb[DL_HDR] = DL_ST_BAD_CRC;
		n = 1;
	} else if (!dl_dmrip_air_allowed(cmd, p, len)) {
		cnt.refused++;
		rb[DL_HDR] = DL_ST_REFUSED;
		n = 1;
	} else {
		from_air = true;
		n = debuglink_dispatch(cmd, p, len, rb + DL_HDR, DL_REPLY_MAX);
		from_air = false;
	}
	out.len = (uint16_t)dl_frame(rb, (uint8_t)(cmd | DL_REPLY_FLAG), seq, (uint16_t)n);
	out.ok = true;
	out.gen = debuglink_reply_gen();
	out.counter = m->counter;
	out.dst = io_src;
	out.ip = io_ip;
	out.port = io_port;
	out.flags = 0;
	cnt.answered++;
	start_send(0xFF, now_ms);
}

static void handle(uint32_t now_ms)
{
	dl_dmrip_msg_t m;
	uint8_t *d = io + UDP_HDR;
	int v = key_src == 0 ? DL_DMRIP_BAD : dl_dmrip_check(key, d, io_len, ipGatewayRadioId(), &m);
	if (v != DL_DMRIP_ACCEPT) {
		if (v == DL_DMRIP_BAD) {
			cnt.bad_auth++;
		} else {
			cnt.not_ours++;
		}
		io_state = IO_FREE;	// silent: no answer to what isn't ours to answer
		return;
	}
	if (m.flags & DL_DMRIP_F_HELLO) {
		if (last_hello != 0 && (now_ms - (last_hello & ~1u)) < HELLO_GAP_MS) {	// bit 0 only says "set"
			cnt.busy++;
			io_state = IO_FREE;
			return;
		}
		last_hello = now_ms | 1u;
		if (replay.nonce == 0) {
			// Once a start: the host's challenge, the time and our ID under the key; never 0
			uint8_t seed[8 + DL_DMRIP_HELLO_MAX];
			uint16_t b = m.body_len > DL_DMRIP_HELLO_MAX ? DL_DMRIP_HELLO_MAX : m.body_len;
			dl_put32(seed, now_ms);
			dl_put32(seed + 4, m.id);
			memcpy(seed + 8, m.body, b);
			replay.nonce = (uint32_t)dl_siphash(key, seed, 8u + b) | 1u;
			replay.last = 0;
		}
		cnt.hello++;
		uint16_t b = m.body_len > DL_DMRIP_HELLO_MAX ? DL_DMRIP_HELLO_MAX : m.body_len;	// echoed, in place
		send_short(DL_DMRIP_F_HELLO, replay.last, b);
		io_state = IO_FREE;
		return;
	}
	switch (dl_dmrip_replay(&replay, m.nonce, m.counter, dl_siphash(key, m.body, m.body_len))) {
	case DL_DMRIP_NEW:
		handle_new(&m, now_ms);
		return;
	case DL_DMRIP_REPEAT:
		cnt.repeats++;
		if (out.ok && out.counter == m.counter && out.gen == debuglink_reply_gen()) {
			out.dst = io_src;
			out.ip = io_ip;
			out.port = io_port;
			start_send(m.chunk != 0 ? m.chunk : 0xFF, now_ms);
		} else {
			cnt.gone++;
			send_short(DL_DMRIP_F_GONE, m.counter, 0);
			io_state = IO_FREE;
		}
		return;
	default:
		cnt.old++;
		io_state = IO_FREE;
		return;
	}
}

// UDP 3335 over the air to our own ID and address, in the gateway's table of air services (ipGatewayUdpBindAir):
// the answer goes to the request's IPv4 source (debuglink.md 7.1). From the link: not ours, dropped
static void air_udp(bool air, uint32_t src, uint16_t src_port, uint32_t dst, uint16_t dst_port, const uint8_t *p,
		    int len)
{
	(void)dst;
	(void)dst_port;
	if (air) {
		debuglink_dmrip_air(src, ipGatewayUdpAirSource(), src_port, p, len);
	}
}

void debuglink_dmrip_init(void)
{
	ipGatewayUdpBindAir(DL_DMRIP_PORT, air_udp);
	uid_key();
	dl_detour_init(&detour);
}

void debuglink_dmrip_poll(uint32_t now_ms)
{
	dl_detour_poll(&detour, &ops, now_ms);
	if (io_state == IO_SEND) {
		if (!send_chunk(now_ms)) {
			io_state = IO_FREE;
		}
	} else if (io_state == IO_REQ) {
		handle(now_ms);
	}
}

// --- the commands (7.4, 7.5) ---

size_t debuglink_cmd_detour(const uint8_t *req, uint16_t len, uint8_t *o, size_t cap)
{
	uint32_t now = hal_millis();
	size_t n = dl_detour_request(&detour, &ops, req, len, from_air, o, cap, now);
	if (len >= 1 && req[0] == DL_DETOUR_STATUS && n == 13 && cap >= 13 + 32) {
		// The radio core's summary of the last detour (radio_detour_view_t)
		radio_detour_view_t dv;
		radio_get_detour(&dv);
		const radio_detour_view_t *d = &dv;
		uint8_t *q = o + n;
		q[0] = (uint8_t)(d->armed | (d->returning << 1) | (d->restored << 2));
		q[1] = d->why;
		q[2] = (uint8_t)d->count;
		q[3] = (uint8_t)(d->count >> 8);
		q[4] = d->mode;
		dl_put32(q + 5, d->rx_hz);
		dl_put32(q + 9, d->ms);
		dl_put32(q + 13, d->rx_ms);
		dl_put32(q + 17, d->tx_ms);
		q[21] = (uint8_t)d->rx_count;
		q[22] = (uint8_t)(d->rx_count >> 8);
		q[23] = (uint8_t)d->tx_count;
		q[24] = (uint8_t)(d->tx_count >> 8);
		q[25] = (uint8_t)d->tx_denied;
		q[26] = (uint8_t)(d->tx_denied >> 8);
		q[27] = (uint8_t)d->rssi_max;
		q[28] = (uint8_t)((uint16_t)d->rssi_max >> 8);
		n += 29;
	}
	return n;
}

size_t debuglink_cmd_dmrip(const uint8_t *req, uint16_t len, uint8_t *o, size_t cap)
{
	(void)cap;
	if (len < 1) {
		o[0] = DL_ST_BAD_ARG;
		return 1;
	}
	switch (req[0]) {
	case DL_DMRIP_OP_STATUS:
		o[0] = DL_ST_OK;
		o[1] = (uint8_t)DL_DMRIP_PORT;
		o[2] = (uint8_t)(DL_DMRIP_PORT >> 8);
		o[3] = key_src;
		dl_put32(o + 4, replay.nonce != 0);
		dl_put32(o + 8, cnt.rx);
		dl_put32(o + 12, cnt.answered);
		dl_put32(o + 16, cnt.bad_auth);
		return 20;
	case DL_DMRIP_OP_KEY: {
		if (from_air) {
			o[0] = DL_ST_REFUSED;	// dl_dmrip_air_allowed refuses it first; kept here as well
			return 1;
		}
		if (len != 1 + DL_DMRIP_KEY) {
			o[0] = DL_ST_BAD_ARG;
			return 1;
		}
		static const uint8_t zero[DL_DMRIP_KEY];
		if (memcmp(req + 1, zero, DL_DMRIP_KEY) == 0) {
			uid_key();
		} else {
			memcpy(key, req + 1, DL_DMRIP_KEY);
			key_src = 2;
		}
		replay.nonce = 0;	// a new key: a new session
		out.ok = false;
		o[0] = DL_ST_OK;
		return 1;
	}
	default:
		o[0] = DL_ST_BAD_ARG;
		return 1;
	}
}

void debuglink_view_dmrip(dl_text_t *t)
{
	dl_text(t, "dmrip.built=1\ndmrip.id=%u\ndmrip.port=%u\ndmrip.key=%s\ndmrip.session=%u\ndmrip.counter=%u\n",
		(unsigned)ipGatewayRadioId(), (unsigned)DL_DMRIP_PORT, key_src == 2 ? "set" : key_src == 1 ? "uid" : "none",
		replay.nonce != 0, (unsigned)replay.last);
	dl_text(t, "dmrip.rx=%u\ndmrip.busy=%u\ndmrip.bad_auth=%u\ndmrip.not_ours=%u\ndmrip.old=%u\ndmrip.repeats=%u\n"
		"dmrip.gone=%u\ndmrip.refused=%u\ndmrip.answered=%u\ndmrip.chunks=%u\ndmrip.dropped=%u\ndmrip.hello=%u\n",
		(unsigned)cnt.rx, (unsigned)cnt.busy, (unsigned)cnt.bad_auth, (unsigned)cnt.not_ours, (unsigned)cnt.old,
		(unsigned)cnt.repeats, (unsigned)cnt.gone, (unsigned)cnt.refused, (unsigned)cnt.answered,
		(unsigned)cnt.chunks, (unsigned)cnt.dropped, (unsigned)cnt.hello);
	uint32_t now = hal_millis();
	uint8_t st[16];
	static const uint8_t q = DL_DETOUR_STATUS;
	(void)dl_detour_request(&detour, &ops, &q, 1, false, st, sizeof(st), now);
	dl_text(t, "detour.phase=%u\ndetour.left_ms=%u\ndetour.items=%u\ndetour.failed=%u\ndetour.result=%u\n"
		"detour.runs=%u\n", st[1], (unsigned)dl_get32(st + 2), st[6], st[7], st[8], (unsigned)dl_get32(st + 9));
}

#endif
