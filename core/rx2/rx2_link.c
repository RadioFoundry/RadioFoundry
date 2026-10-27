// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The second receiver's link: frames, payloads, and both ends' state machines (rx2_link.h)
#include <string.h>
#include "rx2_link.h"

// --- Frames ---

uint16_t rx2_crc16(const uint8_t *p, uint16_t n, uint16_t crc)
{
	while (n-- > 0) {
		crc ^= (uint16_t)(*p++ << 8);
		for (uint8_t i = 0; i < 8; i++) {
			crc = (crc & 0x8000u) != 0 ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
		}
	}
	return crc;
}

uint16_t rx2_frame(uint8_t *out, uint8_t type, uint8_t seq, const uint8_t *payload, uint8_t len)
{
	if (len > RX2_PAYLOAD_MAX) {
		return 0;
	}
	out[0] = RX2_SYNC0;
	out[1] = RX2_SYNC1;
	out[2] = len;
	out[3] = type;
	out[4] = seq;
	if (len > 0) {
		memcpy(&out[5], payload, len);
	}
	uint16_t crc = rx2_crc16(&out[2], (uint16_t)(len + 3u), 0xFFFFu);
	out[5 + len] = (uint8_t)(crc >> 8);
	out[6 + len] = (uint8_t)crc;
	return (uint16_t)(len + 7u);
}

enum { P_SYNC0, P_SYNC1, P_LEN, P_TYPE, P_SEQ, P_DATA };

void rx2_parser_init(rx2_parser_t *p)
{
	memset(p, 0, sizeof(*p));
}

bool rx2_parse(rx2_parser_t *p, uint8_t b)
{
	switch (p->st) {
	case P_SYNC0:
		if (b == RX2_SYNC0) {
			p->st = P_SYNC1;
		}
		return false;
	case P_SYNC1:
		p->st = b == RX2_SYNC1 ? P_LEN : (b == RX2_SYNC0 ? P_SYNC1 : P_SYNC0);
		return false;
	case P_LEN:
		if (b > RX2_PAYLOAD_MAX) {
			p->bad++;
			p->st = b == RX2_SYNC0 ? P_SYNC1 : P_SYNC0;
			return false;
		}
		p->len = b;
		p->st = P_TYPE;
		return false;
	case P_TYPE:
		p->type = b;
		p->st = P_SEQ;
		return false;
	case P_SEQ:
		p->seq = b;
		p->at = 0;
		p->st = P_DATA;
		return false;
	default:
		p->buf[p->at++] = b;
		if (p->at < (uint16_t)(p->len + 2u)) {
			return false;
		}
		p->st = P_SYNC0;
		{
			uint8_t hdr[3] = { p->len, p->type, p->seq };
			uint16_t crc = rx2_crc16(hdr, 3, 0xFFFFu);
			crc = rx2_crc16(p->buf, p->len, crc);
			if (crc != (uint16_t)((p->buf[p->len] << 8) | p->buf[p->len + 1u])) {
				p->bad++;
				return false;
			}
		}
		return true;
	}
}

// --- Payloads ---

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

// rx_hz 4, mode, bw, squelch, tone type, tone value 2, cc, ts, own_id 4, talk_group 4, count, reserved 4 = 25
uint8_t rx2_pack_tune(const rx2_tune_t *t, uint8_t *out)
{
	uint8_t n = t->group_count > RX2_GROUPS ? (uint8_t)RX2_GROUPS : t->group_count;
	memset(out, 0, RX2_TUNE_FIXED);
	put32(&out[0], t->rx_hz);
	out[4] = t->mode;
	out[5] = t->bw;
	out[6] = t->squelch;
	out[7] = (uint8_t)t->rx_tone.type;
	out[8] = (uint8_t)t->rx_tone.value;
	out[9] = (uint8_t)(t->rx_tone.value >> 8);
	out[10] = t->color_code;
	out[11] = t->timeslot;
	put32(&out[12], t->own_id);
	put32(&out[16], t->talk_group);
	out[20] = n;
	for (uint8_t i = 0; i < n; i++) {
		put32(&out[RX2_TUNE_FIXED + 4u * i], t->groups[i]);
	}
	return (uint8_t)(RX2_TUNE_FIXED + 4u * n);
}

bool rx2_unpack_tune(const uint8_t *p, uint8_t len, rx2_tune_t *t)
{
	if (len < RX2_TUNE_FIXED || p[20] > RX2_GROUPS || len != RX2_TUNE_FIXED + 4u * p[20]) {
		return false;
	}
	memset(t, 0, sizeof(*t));
	t->rx_hz = get32(&p[0]);
	t->mode = p[4];
	t->bw = p[5];
	t->squelch = p[6];
	t->rx_tone.type = p[7];
	t->rx_tone.value = (uint16_t)(p[8] | p[9] << 8);
	t->color_code = p[10];
	t->timeslot = p[11];
	t->own_id = get32(&p[12]);
	t->talk_group = get32(&p[16]);
	t->group_count = p[20];
	for (uint8_t i = 0; i < t->group_count; i++) {
		t->groups[i] = get32(&p[RX2_TUNE_FIXED + 4u * i]);
	}
	return true;
}

// flags (tuned 1, active 2, dmr 4, group 8, heard 16), rssi 2, noise, src 4, dst 4, reserved = 13
uint8_t rx2_pack_status(const rx2_status_t *st, uint8_t *out)
{
	memset(out, 0, RX2_STATUS_LEN);
	out[0] = (uint8_t)((st->tuned ? 1u : 0u) | (st->active ? 2u : 0u) | (st->dmr ? 4u : 0u) | (st->group ? 8u : 0u) |
			   (st->heard ? 16u : 0u));
	out[1] = (uint8_t)st->rssi_dbm;
	out[2] = (uint8_t)((uint16_t)st->rssi_dbm >> 8);
	out[3] = st->noise;
	put32(&out[4], st->src);
	put32(&out[8], st->dst);
	return RX2_STATUS_LEN;
}

bool rx2_unpack_status(const uint8_t *p, uint8_t len, rx2_status_t *st)
{
	if (len < RX2_STATUS_LEN) {
		return false;
	}
	memset(st, 0, sizeof(*st));
	st->tuned = (p[0] & 1u) != 0;
	st->active = (p[0] & 2u) != 0;
	st->dmr = (p[0] & 4u) != 0;
	st->group = (p[0] & 8u) != 0;
	st->heard = (p[0] & 16u) != 0;
	st->rssi_dbm = (int16_t)(uint16_t)(p[1] | p[2] << 8);
	st->noise = p[3];
	st->src = get32(&p[4]);
	st->dst = get32(&p[8]);
	return true;
}

// --- Main side ---

static void main_send(rx2_main_t *m, uint8_t type, const uint8_t *payload, uint8_t len)
{
	uint8_t f[RX2_FRAME_MAX];
	uint16_t n = rx2_frame(f, type, m->seq, payload, len);
	m->seq++;
	if (n > 0) {
		m->send(f, n);
	}
}

static void main_send_tune(rx2_main_t *m, uint32_t now_ms)
{
	uint8_t p[RX2_PAYLOAD_MAX];
	uint8_t n = rx2_pack_tune(&m->tune, p);
	m->tune_seq = m->seq;
	m->tune_acked = false;
	m->tune_sent_ms = now_ms;
	main_send(m, RX2_TUNE, p, n);
}

static void main_send_audio(rx2_main_t *m)
{
	uint8_t p[2] = { m->audio ? 1u : 0u, m->volume };
	main_send(m, RX2_AUDIO, p, 2);
}

static void main_send_led(rx2_main_t *m)
{
	main_send(m, RX2_LED, &m->leds, 1);
}

static void main_send_path(rx2_main_t *m)
{
	uint8_t p[5] = { m->path_on ? 1u : 0u };
	put32(&p[1], m->path_hz);
	main_send(m, RX2_PATH, p, 5);
}

void rx2_main_init(rx2_main_t *m, rx2_send_fn send)
{
	memset(m, 0, sizeof(*m));
	m->send = send;
	rx2_parser_init(&m->parser);
}

static void main_dl_fail(rx2_main_t *m)
{
	m->dl_state = RX2_DL_FAILED;
	m->dl_failed++;
}

// A part of the reply in flight: in order, from index 0, until the one marked last
static void main_dl_part(rx2_main_t *m, const rx2_parser_t *p)
{
	if (m->dl_state != RX2_DL_WAIT || p->len < 2u || p->buf[0] != m->dl_tag) {
		return;		// late, or another request's
	}
	uint8_t idx = (uint8_t)(p->buf[1] & ~RX2_DL_LAST);
	uint16_t n = (uint16_t)(p->len - 2u);
	if (idx != m->dl_next || m->dl_len + n > RX2_DL_REPLY_MAX) {
		main_dl_fail(m);
		return;
	}
	memcpy(&m->dl_reply[m->dl_len], &p->buf[2], n);
	m->dl_len = (uint16_t)(m->dl_len + n);
	m->dl_next++;
	if (p->buf[1] & RX2_DL_LAST) {
		m->dl_state = m->dl_len > 0 ? RX2_DL_DONE : RX2_DL_FAILED;
	}
}

void rx2_main_rx(rx2_main_t *m, uint8_t b, uint32_t now_ms)
{
	rx2_parser_t *p = &m->parser;
	if (!rx2_parse(p, b)) {
		return;
	}
	m->frames++;
	m->heard_ms = now_ms;
	switch (p->type) {
	case RX2_HELLO:
		if (p->len >= 2) {
			bool was_up = m->up;
			m->version = p->buf[0];
			m->flags = p->buf[1];
			uint8_t n = (uint8_t)(p->len - 2u);
			if (n >= sizeof(m->fw)) {
				n = (uint8_t)(sizeof(m->fw) - 1u);
			}
			memcpy(m->fw, &p->buf[2], n);
			m->fw[n] = '\0';
			m->up = m->version == RX2_PROTO_VERSION;
			if (m->up && !was_up) {
				// Back (or new): it starts stopped and muted; give it the settings again
				if (m->want_rx) {
					main_send_tune(m, now_ms);
				}
				main_send_audio(m);
				if (m->leds != 0) {
					main_send_led(m);
				}
				if (m->path_on) {
					main_send_path(m);
				}
			}
		}
		break;
	case RX2_STATUS:
		if (m->up) {
			rx2_unpack_status(p->buf, p->len, &m->st);
		}
		break;
	case RX2_ACK:
		if (p->len >= 2 && p->buf[0] == m->tune_seq) {
			m->tune_acked = p->buf[1] == 0;
		}
		break;
	case RX2_DL_REPLY:
		main_dl_part(m, p);
		break;
	default:
		break;	// RX2_LOG: the board reads it from the parser before calling here, if it wants it
	}
}

void rx2_main_poll(rx2_main_t *m, uint32_t now_ms)
{
	if (m->up && now_ms - m->heard_ms > RX2_ALIVE_MS) {
		m->up = false;
		memset(&m->st, 0, sizeof(m->st));
	}
	if (m->dl_state == RX2_DL_WAIT && (!m->up || now_ms - m->dl_sent_ms >= RX2_DL_TIMEOUT_MS)) {
		main_dl_fail(m);
	}
	if (!m->up) {
		if (now_ms - m->asked_ms >= RX2_HELLO_EVERY_MS || m->asked_ms == 0) {
			m->asked_ms = now_ms == 0 ? 1u : now_ms;
			main_send(m, RX2_HELLO_REQ, NULL, 0);
		}
		return;
	}
	if (m->want_rx && !m->tune_acked && now_ms - m->tune_sent_ms >= RX2_RETUNE_MS) {
		m->resent++;
		main_send_tune(m, now_ms);
	}
}

bool rx2_main_tune(rx2_main_t *m, const rx2_tune_t *t, uint32_t now_ms)
{
	if (!m->up) {
		return false;
	}
	bool same = m->want_rx && memcmp(&m->tune, t, sizeof(*t)) == 0;
	m->tune = *t;
	m->want_rx = true;
	if (!same) {
		m->tunes++;
		main_send_tune(m, now_ms);
	}
	return true;
}

void rx2_main_stop(rx2_main_t *m)
{
	m->want_rx = false;
	m->tune_acked = false;
	m->audio = false;
	m->st.active = m->st.heard = m->st.tuned = false;
	if (m->up) {
		main_send(m, RX2_STOP, NULL, 0);
	}
}

void rx2_main_status(const rx2_main_t *m, rx2_status_t *st)
{
	*st = m->st;
	st->up = m->up;
	st->tuned = m->up && m->want_rx && m->tune_acked && m->st.tuned;
	if (!st->tuned) {
		st->active = st->heard = false;
	}
}

void rx2_main_audio(rx2_main_t *m, bool on, uint8_t volume)
{
	if (on == m->audio && volume == m->volume) {
		return;
	}
	m->audio = on;
	m->volume = volume;
	if (m->up) {
		main_send_audio(m);
	}
}

void rx2_main_led(rx2_main_t *m, uint8_t leds)
{
	if (leds == m->leds) {
		return;
	}
	m->leds = leds;
	if (m->up) {
		main_send_led(m);
	}
}

void rx2_main_path(rx2_main_t *m, bool on, uint32_t hz)
{
	if (!on) {
		hz = 0;
	}
	if (on == m->path_on && hz == m->path_hz) {
		return;
	}
	m->path_on = on;
	m->path_hz = hz;
	if (m->up) {
		main_send_path(m);
	}
}

void rx2_main_command(rx2_main_t *m, uint8_t type)
{
	uint8_t p[4];
	put32(p, RX2_STUB_MAGIC);
	main_send(m, type, p, 4);
}

bool rx2_main_dl_send(rx2_main_t *m, const uint8_t *req, uint8_t len, uint32_t now_ms)
{
	uint8_t p[RX2_PAYLOAD_MAX];
	if (!m->up || len == 0 || len > RX2_DL_REQ_MAX) {
		return false;
	}
	m->dl_tag++;
	m->dl_state = RX2_DL_WAIT;
	m->dl_next = 0;
	m->dl_len = 0;
	m->dl_sent_ms = now_ms;
	m->dl_requests++;
	p[0] = m->dl_tag;
	p[1] = RX2_DL_LAST;
	memcpy(&p[2], req, len);
	main_send(m, RX2_DL, p, (uint8_t)(len + 2u));
	return true;
}

// --- The update bridge's exit sequence ---

const uint8_t rx2_exit_seq[RX2_EXIT_LEN] = { 0x7E, 0xE7, 0x81, 0x18 };

void rx2_exit_init(rx2_exit_t *x, uint32_t now_us)
{
	x->held = 0;
	x->ok = false;
	x->last_us = now_us;
}

static uint8_t exit_release(rx2_exit_t *x, uint8_t *out)
{
	uint8_t n = x->held;
	memcpy(out, rx2_exit_seq, n);
	x->held = 0;
	return n;
}

uint8_t rx2_exit_byte(rx2_exit_t *x, uint8_t c, uint32_t now_us, uint8_t *out)
{
	uint8_t n = 0;
	if (now_us - x->last_us >= RX2_EXIT_GUARD_US) {
		n = exit_release(x, out);	// a new burst: what the last one held goes first
		x->ok = true;
	}
	x->last_us = now_us;
	if (x->ok && x->held < RX2_EXIT_LEN && c == rx2_exit_seq[x->held]) {
		x->held++;
		return n;
	}
	n = (uint8_t)(n + exit_release(x, out + n));
	x->ok = false;
	out[n++] = c;
	return n;
}

bool rx2_exit_idle(rx2_exit_t *x, uint32_t now_us, uint8_t *out, uint8_t *n)
{
	*n = 0;
	if (x->held == 0 || now_us - x->last_us < RX2_EXIT_GUARD_US) {
		return false;
	}
	if (x->held == RX2_EXIT_LEN) {
		return true;
	}
	*n = exit_release(x, out);
	x->ok = false;
	return false;
}

// --- Receiver side ---

static void recv_send(rx2_recv_t *r, uint8_t type, const uint8_t *payload, uint8_t len)
{
	uint8_t f[RX2_FRAME_MAX];
	uint16_t n = rx2_frame(f, type, r->seq, payload, len);
	r->seq++;
	if (n > 0) {
		r->send(f, n);
	}
}

static void recv_hello(rx2_recv_t *r)
{
	uint8_t p[RX2_PAYLOAD_MAX];
	size_t n = r->fw != NULL ? strlen(r->fw) : 0;
	if (n > RX2_PAYLOAD_MAX - 2u) {
		n = RX2_PAYLOAD_MAX - 2u;
	}
	p[0] = RX2_PROTO_VERSION;
	p[1] = r->flags;
	if (n > 0) {
		memcpy(&p[2], r->fw, n);
	}
	recv_send(r, RX2_HELLO, p, (uint8_t)(n + 2u));
}

static void recv_status(rx2_recv_t *r, uint32_t now_ms)
{
	rx2_status_t st;
	memset(&st, 0, sizeof(st));
	r->ops->status(&st);
	st.up = true;
	st.tuned = st.tuned && r->tuned;
	st.heard = st.active && r->audio;
	uint8_t p[RX2_STATUS_LEN];
	rx2_pack_status(&st, p);
	recv_send(r, RX2_STATUS, p, RX2_STATUS_LEN);
	r->last = st;
	r->sent_ms = now_ms;
}

void rx2_recv_init(rx2_recv_t *r, rx2_send_fn send, const rx2_ops_t *ops, uint8_t flags, const char *fw)
{
	memset(r, 0, sizeof(*r));
	r->send = send;
	r->ops = ops;
	r->flags = flags;
	r->fw = fw;
	rx2_parser_init(&r->parser);
}

// RX2_DL: run the command, then send the reply in parts, in order. DL_ST_UNKNOWN_CMD (1) without a server
static void recv_dl(rx2_recv_t *r, const rx2_parser_t *p)
{
	if (p->len < 3u || p->buf[1] != RX2_DL_LAST) {
		return;		// a request is one part with at least its command
	}
	uint8_t tag = p->buf[0];
	size_t n = 1;
	r->dl_requests++;
	if (r->ops->dl != NULL) {
		n = r->ops->dl(p->buf[2], &p->buf[3], (uint16_t)(p->len - 3u), r->dl_out, sizeof(r->dl_out));
	} else {
		r->dl_out[0] = 1;
	}
	if (n == 0 || n > sizeof(r->dl_out)) {
		n = 1;
		r->dl_out[0] = 1;
	}
	uint8_t f[RX2_PAYLOAD_MAX];
	for (size_t off = 0, idx = 0; off < n; idx++) {
		size_t k = n - off < RX2_DL_PIECE ? n - off : RX2_DL_PIECE;
		f[0] = tag;
		f[1] = (uint8_t)(idx | (off + k == n ? RX2_DL_LAST : 0u));
		memcpy(&f[2], &r->dl_out[off], k);
		recv_send(r, RX2_DL_REPLY, f, (uint8_t)(k + 2u));
		off += k;
	}
}

static bool magic_ok(const rx2_parser_t *p)
{
	return p->len == 4 && get32(p->buf) == RX2_STUB_MAGIC;
}

void rx2_recv_rx(rx2_recv_t *r, uint8_t b, uint32_t now_ms)
{
	rx2_parser_t *p = &r->parser;
	if (!rx2_parse(p, b)) {
		return;
	}
	r->frames++;
	switch (p->type) {
	case RX2_HELLO_REQ:
		// The main SoC (re)started: it thinks we're stopped and muted, so we are
		if (r->tuned) {
			r->ops->stop();
			r->tuned = false;
		}
		if (r->audio) {
			r->ops->audio(false, 0);
			r->audio = false;
		}
		if (r->ops->led != NULL) {
			r->ops->led(0);
		}
		if (r->ops->path != NULL) {
			r->ops->path(false, 0);
		}
		recv_hello(r);
		break;
	case RX2_TUNE: {
		rx2_tune_t t;
		uint8_t res = 1;
		if (rx2_unpack_tune(p->buf, p->len, &t) && r->ops->tune(&t)) {
			r->tuned = true;
			res = 0;
		}
		uint8_t a[2] = { p->seq, res };
		recv_send(r, RX2_ACK, a, 2);
		recv_status(r, now_ms);
		break;
	}
	case RX2_STOP:
		r->ops->stop();
		r->ops->audio(false, 0);
		r->tuned = r->audio = false;
		recv_status(r, now_ms);
		break;
	case RX2_AUDIO:
		if (p->len >= 2) {
			r->audio = p->buf[0] != 0;
			r->ops->audio(r->audio, p->buf[1]);
		}
		break;
	case RX2_LED:
		if (p->len >= 1 && r->ops->led != NULL) {
			r->ops->led(p->buf[0]);
		}
		break;
	case RX2_PATH:
		if (p->len >= 5 && r->ops->path != NULL) {
			r->ops->path(p->buf[0] != 0, get32(&p->buf[1]));
		}
		break;
	case RX2_STUB:
		if (magic_ok(p) && r->ops->stub != NULL) {
			r->ops->stub();
		}
		break;
	case RX2_RESET:
		if (magic_ok(p) && r->ops->reset != NULL) {
			r->ops->reset();
		}
		break;
	case RX2_DL:
		recv_dl(r, p);
		break;
	default:
		break;
	}
}

static bool status_changed(const rx2_status_t *a, const rx2_status_t *b)
{
	int d = a->rssi_dbm - b->rssi_dbm;
	return a->tuned != b->tuned || a->active != b->active || a->dmr != b->dmr || a->group != b->group ||
	       a->heard != b->heard || a->src != b->src || a->dst != b->dst || d > 3 || d < -3;
}

void rx2_recv_poll(rx2_recv_t *r, uint32_t now_ms)
{
	rx2_status_t st;
	memset(&st, 0, sizeof(st));
	r->ops->status(&st);
	st.up = true;
	st.tuned = st.tuned && r->tuned;
	st.heard = st.active && r->audio;
	if (now_ms - r->sent_ms >= RX2_STATUS_EVERY_MS || status_changed(&st, &r->last)) {
		recv_status(r, now_ms);
	}
}

void rx2_recv_log(rx2_recv_t *r, const char *text)
{
	size_t n = strlen(text);
	if (n > RX2_PAYLOAD_MAX) {
		n = RX2_PAYLOAD_MAX;
	}
	recv_send(r, RX2_LOG, (const uint8_t *)text, (uint8_t)n);
}
