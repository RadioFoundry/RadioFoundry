// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The second receiver's link (core/rx2): frames, payloads, and both ends back to back over a lossy wire
#include <stdio.h>
#include <string.h>
#include "rx2/rx2_link.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

// The wire: what each side sent, delivered on pump()
static uint8_t to_recv[4096], to_main[4096];
static size_t n_to_recv, n_to_main;
static bool drop_to_recv;	// lose what the main side sends
static rx2_main_t m;
static rx2_recv_t r;

static void send_main(const uint8_t *b, uint16_t n)
{
	if (!drop_to_recv && n_to_recv + n <= sizeof to_recv) {
		memcpy(&to_recv[n_to_recv], b, n);
		n_to_recv += n;
	}
}

static void send_recv(const uint8_t *b, uint16_t n)
{
	if (n_to_main + n <= sizeof to_main) {
		memcpy(&to_main[n_to_main], b, n);
		n_to_main += n;
	}
}

// The receiver's radio
static rx2_tune_t b_tune;
static int b_tunes, b_stops, b_stubs, b_resets;
static bool b_audio, b_active, b_refuse, b_off_settings;
static uint8_t b_volume;

static bool b_do_tune(const rx2_tune_t *t)
{
	if (b_refuse) {
		return false;
	}
	b_tune = *t;
	b_tunes++;
	return true;
}
static void b_do_stop(void) { b_stops++; }
static void b_do_audio(bool on, uint8_t volume) { b_audio = on; b_volume = volume; }
static void b_do_status(rx2_status_t *st)
{
	st->tuned = !b_off_settings;
	st->active = b_active;
	st->dmr = b_active;
	st->group = true;
	st->rssi_dbm = -90;
	st->src = b_active ? 3100001u : 0;
	st->dst = b_active ? 91u : 0;
}
static void b_do_stub(void) { b_stubs++; }
static void b_do_reset(void) { b_resets++; }
static uint8_t b_leds = 0xFF;
static void b_do_led(uint8_t leds) { b_leds = leds; }
static bool b_path_on = true;
static uint32_t b_path_hz, b_paths;
static void b_do_path(bool on, uint32_t hz)
{
	b_path_on = on;
	b_path_hz = hz;
	b_paths++;
}

// The receiver's debug link server: 0x01 echoes its payload; 0x40 answers n bytes (u16 n), status included
static uint8_t b_dl_cmd;
static size_t b_do_dl(uint8_t cmd, const uint8_t *req, uint16_t len, uint8_t *out, size_t cap)
{
	b_dl_cmd = cmd;
	if (cmd == 0x01) {
		out[0] = 0;
		memcpy(out + 1, req, len);
		return 1u + len;
	}
	if (cmd == 0x40 && len == 2) {
		size_t n = (size_t)(req[0] | req[1] << 8);
		if (n > cap) {
			n = cap;
		}
		out[0] = 0;
		for (size_t i = 1; i < n; i++) {
			out[i] = (uint8_t)(i * 7u);
		}
		return n;
	}
	out[0] = 1;
	return 1;
}

static const rx2_ops_t b_ops = { b_do_tune, b_do_stop, b_do_audio, b_do_status, b_do_stub, b_do_reset, b_do_led,
				 b_do_dl, b_do_path };
static const rx2_ops_t b_ops_no_dl = { b_do_tune, b_do_stop, b_do_audio, b_do_status, b_do_stub, b_do_reset, b_do_led,
				       NULL, NULL };

static void pump(uint32_t now_ms)
{
	size_t n = n_to_recv;
	n_to_recv = 0;
	uint8_t buf[sizeof to_recv];
	memcpy(buf, to_recv, n);
	for (size_t i = 0; i < n; i++) {
		rx2_recv_rx(&r, buf[i], now_ms);
	}
	n = n_to_main;
	n_to_main = 0;
	uint8_t buf2[sizeof to_main];
	memcpy(buf2, to_main, n);
	for (size_t i = 0; i < n; i++) {
		rx2_main_rx(&m, buf2[i], now_ms);
	}
}

// Both ends' polls every 20 ms from t0 for ms
static uint32_t run(uint32_t t0, uint32_t ms, bool recv_alive)
{
	uint32_t t = t0;
	for (; t < t0 + ms; t += 20) {
		rx2_main_poll(&m, t);
		if (recv_alive) {
			rx2_recv_poll(&r, t);
			pump(t);
		} else {
			n_to_recv = 0;
		}
	}
	return t;
}

static void test_frames(void)
{
	// CRC-16/CCITT-FALSE check value
	CHECK(rx2_crc16((const uint8_t *)"123456789", 9, 0xFFFFu) == 0x29B1u);

	uint8_t f[RX2_FRAME_MAX];
	uint8_t pl[3] = { 1, 2, 3 };
	uint16_t n = rx2_frame(f, RX2_AUDIO, 7, pl, 3);
	CHECK(n == 10 && f[0] == 0xA5 && f[1] == 0x5A && f[2] == 3 && f[3] == RX2_AUDIO && f[4] == 7);
	uint8_t big[RX2_PAYLOAD_MAX + 1];
	CHECK(rx2_frame(f, RX2_LOG, 0, big, RX2_PAYLOAD_MAX + 1) == 0);

	// Noise, a false sync, a frame
	rx2_parser_t p;
	rx2_parser_init(&p);
	n = rx2_frame(f, RX2_AUDIO, 7, pl, 3);
	uint8_t stream[64] = { 0x00, 0xA5, 0xA5, 0x5A };	// A5 A5 5A: the second A5 starts the frame
	memcpy(&stream[4], &f[2], n - 2u);
	int good = 0;
	for (uint16_t i = 0; i < n + 2u; i++) {
		if (rx2_parse(&p, stream[i])) {
			good++;
			CHECK(p.type == RX2_AUDIO && p.seq == 7 && p.len == 3 && p.buf[2] == 3);
		}
	}
	CHECK(good == 1);

	// A flipped bit: dropped and counted, and the next frame is read
	n = rx2_frame(f, RX2_AUDIO, 8, pl, 3);
	f[6] ^= 0x10;
	good = 0;
	for (uint16_t i = 0; i < n; i++) {
		good += rx2_parse(&p, f[i]);
	}
	CHECK(good == 0 && p.bad == 1);
	n = rx2_frame(f, RX2_STOP, 9, NULL, 0);
	for (uint16_t i = 0; i < n; i++) {
		good += rx2_parse(&p, f[i]);
	}
	CHECK(good == 1 && p.type == RX2_STOP && p.len == 0);

	// A length over the limit resyncs without waiting for a frame's worth of bytes
	rx2_parser_init(&p);
	uint8_t bad_len[3] = { 0xA5, 0x5A, 0xF0 };
	for (int i = 0; i < 3; i++) {
		rx2_parse(&p, bad_len[i]);
	}
	CHECK(p.bad == 1);
	good = 0;
	for (uint16_t i = 0; i < n; i++) {
		good += rx2_parse(&p, f[i]);
	}
	CHECK(good == 1);
}

static void test_payloads(void)
{
	rx2_tune_t t, u;
	memset(&t, 0, sizeof t);
	t.rx_hz = 446006250u;
	t.mode = RADIO_MODE_DMR;
	t.bw = RADIO_BW_12K5;
	t.squelch = 5;
	t.rx_tone.type = TONE_DCS_INV;
	t.rx_tone.value = 0x13;
	t.color_code = 1;
	t.timeslot = 2;
	t.own_id = 3100001u;
	t.talk_group = 91;
	t.group_count = RX2_GROUPS;
	for (uint8_t i = 0; i < RX2_GROUPS; i++) {
		t.groups[i] = 9000u + i;
	}
	uint8_t p[RX2_PAYLOAD_MAX];
	uint8_t n = rx2_pack_tune(&t, p);
	CHECK(n == RX2_TUNE_FIXED + 4u * RX2_GROUPS && n <= RX2_PAYLOAD_MAX);
	CHECK(rx2_unpack_tune(p, n, &u) && memcmp(&t, &u, sizeof t) == 0);
	CHECK(!rx2_unpack_tune(p, (uint8_t)(n - 1u), &u));
	p[20] = RX2_GROUPS + 1u;
	CHECK(!rx2_unpack_tune(p, n, &u));

	rx2_status_t s, v;
	memset(&s, 0, sizeof s);
	s.tuned = s.active = s.dmr = s.heard = true;
	s.rssi_dbm = -121;
	s.noise = 40;
	s.src = 3100001u;
	s.dst = 3100002u;
	n = rx2_pack_status(&s, p);
	CHECK(n == RX2_STATUS_LEN && rx2_unpack_status(p, n, &v));
	CHECK(v.tuned && v.active && v.dmr && !v.group && v.heard && v.rssi_dbm == -121 && v.noise == 40 &&
	      v.src == 3100001u && v.dst == 3100002u && !v.up);
}

static void test_link(void)
{
	rx2_main_init(&m, send_main);
	rx2_recv_init(&r, send_recv, &b_ops, RX2_HELLO_DMR, "rfoundry-rx2 1.2");
	rx2_status_t st;

	// Down until the receiver answers; tune() refused meanwhile (the core alternates)
	rx2_tune_t t;
	memset(&t, 0, sizeof t);
	t.rx_hz = 145500000u;
	t.mode = RADIO_MODE_FM;
	CHECK(!rx2_main_tune(&m, &t, 0));
	uint32_t now = run(0, 100, true);
	rx2_main_status(&m, &st);
	CHECK(m.up && st.up && !st.tuned && m.flags == RX2_HELLO_DMR && strcmp(m.fw, "rfoundry-rx2 1.2") == 0);

	// Tune: acknowledged, tuned
	CHECK(rx2_main_tune(&m, &t, now));
	now = run(now, 100, true);
	rx2_main_status(&m, &st);
	CHECK(b_tunes == 1 && b_tune.rx_hz == 145500000u && st.tuned && !st.active);
	// The same settings again: nothing sent
	CHECK(rx2_main_tune(&m, &t, now) && m.tunes == 1);
	now = run(now, 100, true);
	CHECK(b_tunes == 1);

	// A call there, the speaker given: heard
	b_active = true;
	rx2_main_audio(&m, true, 60);
	now = run(now, 100, true);
	rx2_main_status(&m, &st);
	CHECK(b_audio && b_volume == 60 && st.active && st.heard && st.dmr && st.src == 3100001u);
	rx2_main_audio(&m, false, 60);
	now = run(now, 100, true);
	rx2_main_status(&m, &st);
	CHECK(!b_audio && st.active && !st.heard);

	// A TUNE lost on the wire goes again
	t.rx_hz = 145600000u;
	drop_to_recv = true;
	CHECK(rx2_main_tune(&m, &t, now));
	now = run(now, 40, true);
	drop_to_recv = false;
	rx2_main_status(&m, &st);
	CHECK(!st.tuned && !st.active);	// not on these settings yet: nothing it hears counts
	now = run(now, RX2_RETUNE_MS + 100u, true);
	rx2_main_status(&m, &st);
	CHECK(m.resent >= 1 && b_tune.rx_hz == 145600000u && st.tuned);

	// Refused (out of its band): not tuned
	b_refuse = true;
	t.rx_hz = 30000000u;
	CHECK(rx2_main_tune(&m, &t, now));
	now = run(now, 100, true);
	rx2_main_status(&m, &st);
	CHECK(!st.tuned);
	b_refuse = false;
	t.rx_hz = 145600000u;
	rx2_main_tune(&m, &t, now);
	now = run(now, 100, true);

	// Accepted, but the board says it isn't on the settings: not tuned until it says it is (the status op's word)
	b_off_settings = true;
	now = run(now, RX2_STATUS_EVERY_MS + 100u, true);
	rx2_main_status(&m, &st);
	CHECK(!st.tuned);
	b_off_settings = false;
	now = run(now, RX2_STATUS_EVERY_MS + 100u, true);
	rx2_main_status(&m, &st);
	CHECK(st.tuned);

	// The receiver goes silent: down after RX2_ALIVE_MS, nothing active
	now = run(now, RX2_ALIVE_MS + 100u, false);
	rx2_main_status(&m, &st);
	CHECK(!st.up && !st.tuned && !st.active);
	CHECK(!rx2_main_tune(&m, &t, now));

	// It comes back (restarted: stopped, muted): HELLO, and the main side sends its settings again
	int tunes = b_tunes;
	rx2_main_audio(&m, true, 70);
	rx2_main_led(&m, 2);	// while it is down: kept, sent when it is back
	rx2_main_path(&m, true, 461462500u);
	rx2_recv_init(&r, send_recv, &b_ops, RX2_HELLO_DMR, "rfoundry-rx2 1.3");
	b_audio = false;
	now = run(now, RX2_HELLO_EVERY_MS + 200u, true);
	rx2_main_status(&m, &st);
	CHECK(st.up && st.tuned && b_tunes == tunes + 1 && b_audio && b_volume == 70 && strcmp(m.fw, "rfoundry-rx2 1.3") == 0);
	CHECK(b_leds == 2);
	rx2_main_led(&m, 3);
	now = run(now, 40, true);
	CHECK(b_leds == 3);
	CHECK(b_path_on && b_path_hz == 461462500u);
	uint32_t paths = b_paths;
	rx2_main_path(&m, true, 461462500u);	// no change: nothing sent
	now = run(now, 40, true);
	CHECK(b_paths == paths);
	rx2_main_path(&m, true, 162475000u);
	now = run(now, 40, true);
	CHECK(b_path_on && b_path_hz == 162475000u && b_paths == paths + 1);

	// Stop
	int stops = b_stops;
	rx2_main_stop(&m);
	now = run(now, 100, true);
	rx2_main_status(&m, &st);
	CHECK(b_stops == stops + 1 && !b_audio && !st.tuned && !st.active);

	// Update route: STUB and RESET with the magic; a wrong magic is ignored
	rx2_main_command(&m, RX2_STUB);
	rx2_main_command(&m, RX2_RESET);
	uint8_t f[RX2_FRAME_MAX], z[4] = { 0 };
	uint16_t n = rx2_frame(f, RX2_STUB, 0, z, 4);
	send_main(f, n);
	now = run(now, 40, true);
	CHECK(b_stubs == 1 && b_resets == 1);

	// The main side leaving for its flash stub: the stub bit reaches the receiver's LEDs (it blinks them)
	rx2_main_led(&m, RX2_LED_STUB);
	now = run(now, 40, true);
	CHECK(b_leds == RX2_LED_STUB);

	// The main side restarting (HELLO_REQ while tuned): the receiver stops and mutes, its LEDs off (the stub's blink
	// ends)
	rx2_main_tune(&m, &t, now);
	rx2_main_audio(&m, true, 50);
	now = run(now, 100, true);
	CHECK(b_audio);
	stops = b_stops;
	rx2_main_init(&m, send_main);
	now = run(now, 100, true);
	CHECK(b_stops == stops + 1 && !b_audio && m.up && b_leds == 0 && !b_path_on);
	(void)now;
}

// A reply of n bytes from the receiver's server, through the link: the main side's state and reply afterwards
static uint32_t dl_ask(uint32_t now, uint16_t n)
{
	uint8_t req[3] = { 0x40, (uint8_t)n, (uint8_t)(n >> 8) };
	CHECK(rx2_main_dl_send(&m, req, sizeof req, now));
	CHECK(m.dl_state == RX2_DL_WAIT);
	return run(now, 40, true);
}

static bool reply_is(uint16_t n)
{
	if (m.dl_state != RX2_DL_DONE || m.dl_len != n || m.dl_reply[0] != 0) {
		return false;
	}
	for (uint16_t i = 1; i < n; i++) {
		if (m.dl_reply[i] != (uint8_t)(i * 7u)) {
			return false;
		}
	}
	return true;
}

// The debug link through the main SoC (rx2-link.md 2.3)
static void test_dl(void)
{
	n_to_recv = n_to_main = 0;
	drop_to_recv = false;
	rx2_main_init(&m, send_main);
	rx2_recv_init(&r, send_recv, &b_ops, 0, "rx2");
	uint8_t ping[4] = { 0x01, 'a', 'b', 'c' };
	CHECK(!rx2_main_dl_send(&m, ping, sizeof ping, 0));	// down: nothing sent
	uint32_t now = run(0, 200, true);
	CHECK(m.up);

	CHECK(rx2_main_dl_send(&m, ping, sizeof ping, now));
	now = run(now, 40, true);
	CHECK(m.dl_state == RX2_DL_DONE && m.dl_len == 4 && m.dl_reply[0] == 0 && memcmp(&m.dl_reply[1], "abc", 3) == 0);
	CHECK(b_dl_cmd == 0x01 && r.dl_requests == 1);

	// One part, two parts at the boundary, the largest reply (11 parts)
	now = dl_ask(now, 1);
	CHECK(reply_is(1));
	now = dl_ask(now, RX2_DL_PIECE);
	CHECK(reply_is(RX2_DL_PIECE));
	now = dl_ask(now, RX2_DL_PIECE + 1u);
	CHECK(reply_is(RX2_DL_PIECE + 1u));
	now = dl_ask(now, 2u * RX2_DL_PIECE);
	CHECK(reply_is(2u * RX2_DL_PIECE));
	now = dl_ask(now, RX2_DL_REPLY_MAX);
	CHECK(reply_is(RX2_DL_REPLY_MAX));
	CHECK(m.dl_failed == 0 && m.up);

	// Too long a request, or an empty one: refused, nothing in flight
	uint8_t big[RX2_DL_REQ_MAX + 1] = { 0x01 };
	CHECK(!rx2_main_dl_send(&m, big, sizeof big, now));
	CHECK(!rx2_main_dl_send(&m, big, 0, now));
	CHECK(rx2_main_dl_send(&m, big, RX2_DL_REQ_MAX, now));
	now = run(now, 40, true);
	CHECK(m.dl_state == RX2_DL_DONE && m.dl_len == RX2_DL_REQ_MAX);

	// No answer: failed after the timeout, not before
	drop_to_recv = true;
	CHECK(rx2_main_dl_send(&m, ping, sizeof ping, now));
	uint32_t t = run(now, RX2_DL_TIMEOUT_MS - 40u, true);
	CHECK(m.dl_state == RX2_DL_WAIT);
	t = run(t, 60, true);
	CHECK(m.dl_state == RX2_DL_FAILED && m.dl_failed == 1);
	drop_to_recv = false;
	now = run(t, 200, true);

	// A reply to an older request (another tag) is ignored; a part out of order fails the request
	CHECK(rx2_main_dl_send(&m, ping, sizeof ping, now));
	uint8_t f[RX2_FRAME_MAX];
	uint8_t stale[3] = { (uint8_t)(m.dl_tag - 1u), RX2_DL_LAST, 0 };
	uint16_t fn = rx2_frame(f, RX2_DL_REPLY, 0, stale, sizeof stale);
	for (uint16_t i = 0; i < fn; i++) {
		rx2_main_rx(&m, f[i], now);
	}
	CHECK(m.dl_state == RX2_DL_WAIT);
	uint8_t skip[3] = { m.dl_tag, (uint8_t)(1u | RX2_DL_LAST), 0 };
	fn = rx2_frame(f, RX2_DL_REPLY, 0, skip, sizeof skip);
	for (uint16_t i = 0; i < fn; i++) {
		rx2_main_rx(&m, f[i], now);
	}
	CHECK(m.dl_state == RX2_DL_FAILED);
	now = run(now, 40, true);		// the receiver's real reply comes late: ignored
	CHECK(m.dl_state == RX2_DL_FAILED);

	// The receiver going down fails the request in flight at once
	CHECK(rx2_main_dl_send(&m, ping, sizeof ping, now));
	n_to_recv = 0;
	run(now, RX2_ALIVE_MS + 100u, false);
	CHECK(!m.up && m.dl_state == RX2_DL_FAILED);

	// A receiver without a server answers every command DL_ST_UNKNOWN_CMD
	n_to_recv = n_to_main = 0;
	rx2_main_init(&m, send_main);
	rx2_recv_init(&r, send_recv, &b_ops_no_dl, 0, "rx2");
	now = run(0, 200, true);
	CHECK(rx2_main_dl_send(&m, ping, sizeof ping, now));
	run(now, 40, true);
	CHECK(m.dl_state == RX2_DL_DONE && m.dl_len == 1 && m.dl_reply[0] == 1);
}

// Feed bytes at the given times (us) through the exit detector, checking idle every 100 us up to end_us. Returns true
// when it asked to end; *passed gets every byte passed on, in order
static uint8_t ex_out[256];
static size_t ex_n;
static bool ex_run(const uint8_t *b, const uint32_t *at, size_t n, uint32_t end_us)
{
	rx2_exit_t x;
	uint8_t o[RX2_EXIT_LEN + 1u];
	uint8_t k;
	size_t i = 0;
	ex_n = 0;
	rx2_exit_init(&x, 0);
	for (uint32_t t = 0; t <= end_us; t += 100) {
		for (; i < n && at[i] <= t; i++) {
			k = rx2_exit_byte(&x, b[i], at[i], o);
			CHECK(k <= RX2_EXIT_LEN + 1u);
			memcpy(&ex_out[ex_n], o, k);
			ex_n += k;
		}
		if (rx2_exit_idle(&x, t, o, &k)) {
			return true;
		}
		memcpy(&ex_out[ex_n], o, k);
		ex_n += k;
	}
	return false;
}

// The update bridge's exit sequence (rx2-link.md 4.1)
static void test_exit(void)
{
	const uint8_t *q = rx2_exit_seq;
	CHECK(q[0] == 0x7E && q[1] == 0xE7 && q[2] == 0x81 && q[3] == 0x18);
	const uint32_t G = RX2_EXIT_GUARD_US;
	uint32_t at[16];

	// Data, quiet, the sequence (87 us a byte at 115200), quiet: the end, the data passed on, the sequence not
	uint8_t a[7] = { 1, 2, 3, q[0], q[1], q[2], q[3] };
	for (int i = 0; i < 3; i++) at[i] = 1000u + 87u * (uint32_t)i;
	for (int i = 3; i < 7; i++) at[i] = 1174u + G + 87u * (uint32_t)(i - 3);
	CHECK(ex_run(a, at, 7, 20000));
	CHECK(ex_n == 3 && ex_out[0] == 1 && ex_out[2] == 3);

	// The sequence alone at the start (the host's first bytes): the end too
	for (int i = 0; i < 4; i++) at[i] = G + 87u * (uint32_t)i;
	CHECK(ex_run(q, at, 4, 20000) && ex_n == 0);

	// Not quiet before it (the tail of a longer message): passed on, no end
	for (int i = 0; i < 7; i++) at[i] = 3000u + 87u * (uint32_t)i;
	CHECK(!ex_run(a, at, 7, 20000));
	CHECK(ex_n == 7 && memcmp(ex_out, a, 7) == 0);

	// Not quiet after it (the start of a longer message): passed on in order, no end
	uint8_t b[6] = { q[0], q[1], q[2], q[3], 9, 10 };
	for (int i = 0; i < 6; i++) at[i] = 3000u + 87u * (uint32_t)i;
	CHECK(!ex_run(b, at, 6, 20000));
	CHECK(ex_n == 6 && memcmp(ex_out, b, 6) == 0);

	// A byte just inside the guard after it still counts as the same burst
	at[4] = 3000u + 3u * 87u + G - 1u;
	CHECK(!ex_run(b, at, 5, 20000) && ex_n == 5 && memcmp(ex_out, b, 5) == 0);

	// Part of the sequence, then quiet: the held bytes go on once the burst has ended
	for (int i = 0; i < 3; i++) at[i] = 3000u + 87u * (uint32_t)i;
	CHECK(!ex_run(q, at, 3, 20000));
	CHECK(ex_n == 3 && memcmp(ex_out, q, 3) == 0);

	// A near miss (one byte wrong) and the sequence split by a quiet gap: never the end, every byte passed on
	uint8_t c[4] = { q[0], q[1], (uint8_t)(q[2] ^ 1u), q[3] };
	for (int i = 0; i < 4; i++) at[i] = 3000u + 87u * (uint32_t)i;
	CHECK(!ex_run(c, at, 4, 20000) && ex_n == 4 && memcmp(ex_out, c, 4) == 0);
	at[0] = 3000u; at[1] = 3087u; at[2] = 3087u + G + 10u; at[3] = at[2] + 87u;
	CHECK(!ex_run(q, at, 4, 20000) && ex_n == 4 && memcmp(ex_out, q, 4) == 0);

	// Two sequences back to back are 8 bytes in one burst: not the end
	uint8_t d[8] = { q[0], q[1], q[2], q[3], q[0], q[1], q[2], q[3] };
	for (int i = 0; i < 8; i++) at[i] = 3000u + 87u * (uint32_t)i;
	CHECK(!ex_run(d, at, 8, 20000) && ex_n == 8 && memcmp(ex_out, d, 8) == 0);

	// The end waits for the quiet after it
	for (int i = 0; i < 4; i++) at[i] = 3000u + 87u * (uint32_t)i;
	CHECK(!ex_run(q, at, 4, 3261u + G - 100u));
	CHECK(ex_run(q, at, 4, 3261u + G + 100u));

	// The time base wraps (a 32-bit microsecond count)
	rx2_exit_t x;
	uint8_t o[RX2_EXIT_LEN + 1u], k;
	uint32_t t0 = 0xFFFFFF00u;
	rx2_exit_init(&x, t0 - G);
	for (uint32_t i = 0; i < 4; i++) {
		CHECK(rx2_exit_byte(&x, q[i], t0 + 87u * i, o) == 0);
	}
	CHECK(!rx2_exit_idle(&x, t0 + 300u, o, &k) && k == 0);
	CHECK(rx2_exit_idle(&x, t0 + 261u + G, o, &k));
}

int main(void)
{
	test_frames();
	test_payloads();
	test_link();
	test_dl();
	test_exit();
	printf("rx2LinkTest: %s (%d failures)\n", failures ? "FAILED" : "passed", failures);
	return failures ? 1 : 0;
}
