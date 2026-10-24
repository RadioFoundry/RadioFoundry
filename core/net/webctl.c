// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Web control over the status server's /ws (webctl.h, docs/spec/tcp-services.md 5.1). The HTTP server does the
// upgrade; from then on the connection's bytes come here: client frames (ws.h) are parsed as they arrive, a text
// frame is one command, and the client gets the status JSON when it changes, pings, and the receive audio it asked
// for. Nothing here blocks: what doesn't fit TCP's buffer waits for the next poll (the status) or is dropped (audio).
#include "config.h"
#if CONFIG_NET_WEBCTL	// the whole file
#include <string.h>
#include "net/webctl.h"
#include "net/ws.h"
#include "net/net_svc.h"
#include "net/tcp.h"
#include "board.h"
#include "hal_time.h"
#include "input/input.h"
#include "dmr/ipGateway.h"
#include "debuglink/dl_fmt.h"
#if CONFIG_NET_PPP
#include "net/ppp.h"
#endif

#define JSON_MAX	640	// the status (http.c's, about 450 bytes on a DMR channel) and the hello
#define PAYLOAD_MAX	WS_CTRL_MAX	// client frames: commands and control frames only

static struct {
	int c;			// the client's connection, -1 = none
	bool control;		// from the host: keys and PTT taken
	bool audio;		// the client asked for the receive audio
	bool closing;		// a close frame went out: the rest is ignored
	uint8_t hn;		// header bytes collected
	uint8_t hb[WS_HDR_MAX];
	bool in_payload;
	ws_hdr_t h;
	uint8_t pn;
	uint8_t pay[PAYLOAD_MAX];
	uint32_t rx_ms, ping_ms, status_ms;
	uint32_t status_hash;
	uint32_t held;		// key bits this client holds down (not PTT)
	bool ptt;		// this client holds PTT
	bool ptt_expired;	// held to WEBCTL_PTT_MAX_MS: not again until the page lets go
	uint32_t ptt_ms;
} w = { .c = -1 };

static char json[JSON_MAX];
static struct {
	uint32_t conns, commands, refused, busy, frames_out, audio_out, audio_dropped, ptt_downs, errors;
} st;

// ---- sending ----

static bool send_frame(uint8_t op, const void *data, uint32_t len)
{
	uint8_t hdr[10];
	int hl = ws_frame_header(hdr, op, len);

	if ((w.c < 0) || w.closing || (tcp_space(w.c) < hl + (int)len)) {
		return false;
	}
	tcp_write(w.c, hdr, hl);
	if (len > 0) {
		tcp_write(w.c, data, (int)len);
	}
	st.frames_out++;
	return true;
}

static void close_with(uint16_t code)
{
	uint8_t p[2] = { (uint8_t)(code >> 8), (uint8_t)code };

	send_frame(WS_OP_CLOSE, p, 2);	// best effort: TCP's FIN follows either way
	w.closing = true;
}

// ---- keys ----

static const keymap_entry_t *key_entry(uint32_t bit)
{
	for (uint8_t i = 0; i < board.keymap_len; i++) {
		if (board.keymap[i].bit == bit) {
			return &board.keymap[i];
		}
	}
	return NULL;
}

static void release_all(void)
{
	if (w.held != 0) {
		input_inject_release(w.held);
		w.held = 0;
	}
	if (w.ptt) {
		input_inject_ptt(false, 0);
		w.ptt = false;
	}
	w.ptt_expired = false;
}

static void key(uint32_t bit, bool down, uint32_t now)
{
	const keymap_entry_t *k = key_entry(bit);

	if (!w.control || (k == NULL)) {
		st.refused++;
		return;
	}
	if (k->short_action == ACT_PTT) {
		if (!down) {
			if (w.ptt) {
				input_inject_ptt(false, 0);
			}
			w.ptt = false;
			w.ptt_expired = false;
		} else if (!w.ptt_expired) {
			if (!w.ptt) {
				w.ptt_ms = now;
				st.ptt_downs++;
			}
			w.ptt = input_inject_ptt(true, WEBCTL_PTT_HOLD_MS) == INPUT_INJ_OK;	// a repeat re-arms it
		}
		return;
	}
	uint32_t m = 1u << bit;

	if (!down) {
		if ((w.held & m) != 0) {
			input_inject_release(m);
			w.held &= ~m;
		}
	} else if ((w.held & m) == 0) {
		if (input_inject_keys(m, INPUT_INJECT_MAX_MS) == INPUT_INJ_OK) {
			w.held |= m;
		} else {
			st.busy++;	// another injected key is held: one at a time
		}
	}
}

// A signed decimal number, the whole rest of the command
static bool number(const uint8_t *s, uint8_t n, int32_t *v)
{
	int32_t x = 0;
	uint8_t i = ((n > 0) && (s[0] == '-')) ? 1u : 0u;

	if ((i >= n) || (n - i > 4)) {
		return false;
	}
	for (; i < n; i++) {
		if ((s[i] < '0') || (s[i] > '9')) {
			return false;
		}
		x = x * 10 + (s[i] - '0');
	}
	*v = (s[0] == '-') ? -x : x;
	return true;
}

// Receive audio is offered only on a link that carries it: 8 kHz PCM16 is 128 kbit/s, more than PPP gets from a
// 115200 baud UART (docs/spec/ppp.md 6). Keys, status and the screen work on either
static bool audio_ok(void)
{
#if CONFIG_NET_PPP
	if (ppp_running()) {
		return false;
	}
#endif
	return CONFIG_NET_USRP != 0;
}

// One text frame: d<bit> key down, u<bit> key up, e<n> encoder detents, a1 / a0 receive audio on / off
static void command(const uint8_t *p, uint8_t n, uint32_t now)
{
	int32_t v;

	st.commands++;
	if ((n < 2) || !number(p + 1, (uint8_t)(n - 1), &v)) {
		st.errors++;
		return;
	}
	switch (p[0]) {
	case 'd':
	case 'u':
		if ((v >= 0) && (v < 32)) {
			key((uint32_t)v, p[0] == 'd', now);
		} else {
			st.errors++;
		}
		break;
	case 'e':
		if (!w.control || (v == 0) || (v < -127) || (v > 127)) {
			st.refused++;
		} else if (input_inject_action(ACT_ENC_STEP, GESTURE_DOWN, (int8_t)v) != INPUT_INJ_OK) {
			st.busy++;
		}
		break;
	case 'a':
		if ((v != 0) && !audio_ok()) {
			st.refused++;
		} else {
			w.audio = (v != 0);
		}
		break;
	default:
		st.errors++;
		break;
	}
}

// ---- receiving ----

static void frame(uint32_t now)
{
	ws_unmask(w.pay, w.pn, w.h.mask, 0);
	switch (w.h.opcode) {
	case WS_OP_TEXT:
		command(w.pay, w.pn, now);
		break;
	case WS_OP_PING:
		send_frame(WS_OP_PONG, w.pay, w.pn);
		break;
	case WS_OP_CLOSE:
		// Echo the client's code (7.1.5) and end
		close_with((w.pn >= 2) ? (uint16_t)((w.pay[0] << 8) | w.pay[1]) : WS_CLOSE_NORMAL);
		release_all();
		tcp_close(w.c);
		w.c = -1;
		break;
	case WS_OP_PONG:
		break;
	default:
		close_with(WS_CLOSE_UNSUPPORTED);	// binary frames: nothing takes them yet (browser TX audio, later)
		break;
	}
}

void webctl_recv(int c, const uint8_t *d, int n)
{
	uint32_t now = hal_millis();

	if (c != w.c) {
		return;
	}
	w.rx_ms = now;
	for (int i = 0; (i < n) && (w.c == c) && !w.closing; i++) {
		if (!w.in_payload) {
			w.hb[w.hn++] = d[i];
			int r = ws_parse_header(w.hb, w.hn, PAYLOAD_MAX, &w.h);

			if (r == WS_NEED_MORE) {
				continue;
			}
			w.hn = 0;
			if (r < 0) {
				st.errors++;
				close_with((r == WS_ERR_TOO_BIG) ? WS_CLOSE_TOO_BIG : WS_CLOSE_PROTOCOL);
				break;
			}
			w.in_payload = true;
			w.pn = 0;
		} else {
			w.pay[w.pn++] = d[i];
		}
		if (w.in_payload && (w.pn == w.h.len)) {
			w.in_payload = false;
			frame(now);
		}
	}
}

// ---- the connection ----

int webctl_conn(void)
{
	return w.c;
}

static void hello(void)
{
	const keymap_entry_t *ptt = NULL;
	dl_text_t t;

	dl_text_init(&t, json, sizeof(json));
	dl_text(&t, "{\"hello\":1,\"control\":%s,\"audio\":%s,\"keys\":[", w.control ? "true" : "false",
		audio_ok() ? "true" : "false");
	for (uint8_t i = 0; i < board.keymap_len; i++) {
		dl_text(&t, "%s%u", (i > 0) ? "," : "", board.keymap[i].bit);
		if (board.keymap[i].short_action == ACT_PTT) {
			ptt = &board.keymap[i];
		}
	}
	dl_text(&t, "],\"ptt\":%d,\"encoder\":%s}", (ptt != NULL) ? (int)ptt->bit : -1,
		(board.input != NULL && board.input->encoder != NULL) ? "true" : "false");
	send_frame(WS_OP_TEXT, json, (uint32_t)t.len);
}

void webctl_open(int c, uint32_t now_ms)
{
	memset(&w, 0, sizeof(w));
	w.c = c;
	w.control = tcp_remote_ip(c) == ipGatewayHostIP();	// as rigctld's PTT: never from beyond the host
	w.rx_ms = now_ms;
	w.ping_ms = now_ms;
	w.status_ms = now_ms - WEBCTL_STATUS_MS;
	st.conns++;
	hello();
}

static void gone(int c)
{
	if (c == w.c) {
		release_all();
		w.c = -1;
	}
}

void webctl_sent(int c)
{
	(void)c;	// the status waits for the next poll
}

void webctl_peer_closed(int c)
{
	if (c == w.c) {
		gone(c);
		tcp_close(c);
	}
}

void webctl_closed(int c)
{
	gone(c);
}

static uint32_t fnv(const char *s, size_t n)
{
	uint32_t h = 2166136261u;

	for (size_t i = 0; i < n; i++) {
		h = (h ^ (uint8_t)s[i]) * 16777619u;
	}
	return h;
}

void webctl_poll(uint32_t now_ms)
{
	if (w.c < 0) {
		return;
	}
	if (w.closing) {
		release_all();
		tcp_close(w.c);	// the close frame is in TCP's buffer: FIN after it
		w.c = -1;
		return;
	}
	if ((now_ms - w.rx_ms) >= WEBCTL_IDLE_MS) {
		release_all();
		tcp_abort(w.c);	// a page that went away without closing (a laptop asleep, the cable)
		w.c = -1;
		return;
	}
	if (w.ptt && ((now_ms - w.ptt_ms) >= WEBCTL_PTT_MAX_MS)) {
		input_inject_ptt(false, 0);
		w.ptt = false;
		w.ptt_expired = true;
	}
	if ((now_ms - w.ping_ms) >= WEBCTL_PING_MS) {
		w.ping_ms = now_ms;
		send_frame(WS_OP_PING, NULL, 0);
	}
	if ((now_ms - w.status_ms) >= WEBCTL_STATUS_MS) {
		dl_text_t t;

		dl_text_init(&t, json, sizeof(json));
		http_status_json(&t);
		uint32_t h = fnv(json, t.len);

		if (!t.full && (h != w.status_hash) && send_frame(WS_OP_TEXT, json, (uint32_t)t.len)) {
			w.status_hash = h;
		}
		w.status_ms = now_ms;
	}
}

void webctl_audio(const int16_t *pcm, uint32_t n, bool open)
{
	uint8_t le[2 * WEBCTL_AUDIO_SAMPLES];

	if ((w.c < 0) || !w.audio || !open || (n != WEBCTL_AUDIO_SAMPLES)) {
		return;
	}
	for (uint32_t i = 0; i < n; i++) {
		le[2 * i] = (uint8_t)pcm[i];
		le[2 * i + 1] = (uint8_t)((uint16_t)pcm[i] >> 8);
	}
	if (send_frame(WS_OP_BINARY, le, sizeof(le))) {
		st.audio_out++;
	} else {
		st.audio_dropped++;
	}
}

void webctl_view(dl_text_t *t)
{
	dl_text(t, "webctl.client=%u\nwebctl.control=%u\nwebctl.conns=%u\nwebctl.commands=%u\nwebctl.refused=%u\n"
		"webctl.busy=%u\nwebctl.errors=%u\nwebctl.frames_out=%u\nwebctl.audio_out=%u\nwebctl.audio_dropped=%u\n"
		"webctl.ptt=%u\nwebctl.ptt_downs=%u\n", w.c >= 0, w.control, (unsigned)st.conns, (unsigned)st.commands,
		(unsigned)st.refused, (unsigned)st.busy, (unsigned)st.errors, (unsigned)st.frames_out,
		(unsigned)st.audio_out, (unsigned)st.audio_dropped, w.ptt, (unsigned)st.ptt_downs);
}

#endif	// CONFIG_NET_WEBCTL
