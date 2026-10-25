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
static bool b_audio, b_active, b_refuse;
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
	st->tuned = true;
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

static const rx2_ops_t b_ops = { b_do_tune, b_do_stop, b_do_audio, b_do_status, b_do_stub, b_do_reset, b_do_led };

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

	// The receiver goes silent: down after RX2_ALIVE_MS, nothing active
	now = run(now, RX2_ALIVE_MS + 100u, false);
	rx2_main_status(&m, &st);
	CHECK(!st.up && !st.tuned && !st.active);
	CHECK(!rx2_main_tune(&m, &t, now));

	// It comes back (restarted: stopped, muted): HELLO, and the main side sends its settings again
	int tunes = b_tunes;
	rx2_main_audio(&m, true, 70);
	rx2_main_led(&m, 2);	// while it is down: kept, sent when it is back
	rx2_recv_init(&r, send_recv, &b_ops, RX2_HELLO_DMR, "rfoundry-rx2 1.3");
	b_audio = false;
	now = run(now, RX2_HELLO_EVERY_MS + 200u, true);
	rx2_main_status(&m, &st);
	CHECK(st.up && st.tuned && b_tunes == tunes + 1 && b_audio && b_volume == 70 && strcmp(m.fw, "rfoundry-rx2 1.3") == 0);
	CHECK(b_leds == 2);
	rx2_main_led(&m, 3);
	now = run(now, 40, true);
	CHECK(b_leds == 3);

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

	// The main side restarting (HELLO_REQ while tuned): the receiver stops and mutes
	rx2_main_tune(&m, &t, now);
	rx2_main_audio(&m, true, 50);
	now = run(now, 100, true);
	CHECK(b_audio);
	stops = b_stops;
	rx2_main_init(&m, send_main);
	now = run(now, 100, true);
	CHECK(b_stops == stops + 1 && !b_audio && m.up && b_leds == 0);
	(void)now;
}

int main(void)
{
	test_frames();
	test_payloads();
	test_link();
	printf("rx2LinkTest: %s (%d failures)\n", failures ? "FAILED" : "passed", failures);
	return failures ? 1 : 0;
}
