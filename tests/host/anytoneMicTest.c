// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The AnyTone serial hand microphone (drivers/input/anytone_mic): presence, PTT and keys from 'A' frames, the other
// frame types skipped whole, broken frames and UART errors dropped (and PTT with them); the radio's answers and status
// frames, the poll that keeps a silent microphone present, and the microphone dropped with its PTT once it stops
// answering; the bytes from a DMA ring with the UART's error marks
#include <stdio.h>
#include "input/anytone_mic/anytone_mic.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

// Bytes in; how many frames were accepted (each one the radio answers)
static int feed(anytone_mic_t *m, const uint8_t *b, unsigned n, uint32_t now)
{
	int acks = 0;
	for (unsigned i = 0; i < n; i++) {
		acks += anytone_mic_byte(m, b[i], now);
	}
	return acks;
}

// A microphone frame as a radio saw it: PTT, the key pressed (1) or released (0), its code
static int frame(anytone_mic_t *m, uint8_t ptt, uint8_t pressed, uint8_t code, uint32_t now)
{
	const uint8_t f[8] = { 'A', ptt, pressed, 0x00, code, 0x00, 0x00, ANYTONE_MIC_END };
	return feed(m, f, sizeof(f), now);
}

static void test_presence_ptt_keys(void)
{
	anytone_mic_t m;
	anytone_mic_init(&m);
	CHECK(!anytone_mic_poll(&m, 0));
	CHECK(frame(&m, 0, 0, ANYTONE_MIC_ID, 100) == 1);
	CHECK(anytone_mic_poll(&m, 100) && !m.ptt && m.key == ANYTONE_MIC_KEY_NONE);
	CHECK(frame(&m, 1, 0, 0x00, 200) == 1);
	CHECK(m.ptt && m.key == ANYTONE_MIC_KEY_NONE);
	// The presence frame doesn't change PTT or a key
	CHECK(frame(&m, 0, 0, ANYTONE_MIC_ID, 300) == 1);
	CHECK(m.ptt);
	CHECK(frame(&m, 0, 1, ANYTONE_MIC_KEY_UP, 400) == 1);
	CHECK(!m.ptt && m.key == ANYTONE_MIC_KEY_UP);
	// The release: the same code with byte 2 clear
	CHECK(frame(&m, 0, 0, ANYTONE_MIC_KEY_UP, 500) == 1);
	CHECK(m.key == ANYTONE_MIC_KEY_NONE);
	CHECK(m.frames == 5 && m.errors == 0);
	// The bytes a radio saw for '1' and '0'
	const uint8_t one[8] = { 'A', 0x00, 0x01, 0x00, 0x02, 0x00, 0x00, 0x06 };
	CHECK(feed(&m, one, sizeof(one), 600) == 1 && m.key == ANYTONE_MIC_KEY_1);
	const uint8_t zero[8] = { 'A', 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x06 };
	CHECK(feed(&m, zero, sizeof(zero), 700) == 1 && m.key == ANYTONE_MIC_KEY_0);
}

static void test_other_frames_and_errors(void)
{
	anytone_mic_t m;
	anytone_mic_init(&m);
	// A PC link frame ('+', 31 bytes) whose bytes hold a whole 'A' frame isn't taken for one
	uint8_t pc[31] = { '+' };
	const uint8_t inner[8] = { 'A', 1, 1, 1, 0x10, 0, 0, ANYTONE_MIC_END };
	for (unsigned i = 0; i < sizeof(inner); i++) {
		pc[4 + i] = inner[i];
	}
	CHECK(feed(&m, pc, sizeof(pc), 10) == 0);
	CHECK(!m.present && !m.ptt);
	// An 'F' frame (7 bytes) and an acknowledgement (one byte), then a microphone frame: in step
	const uint8_t f[7] = { 'F', 'A', 1, 1, 1, 0x10, ANYTONE_MIC_END };
	CHECK(feed(&m, f, sizeof(f), 20) == 0);
	const uint8_t ack = 0x06;
	CHECK(feed(&m, &ack, 1, 30) == 0);
	CHECK(frame(&m, 1, 0, 0, 40) == 1 && m.ptt);
	// A frame without 0x06 at its end is dropped and counted, and lets go of PTT: it may have been the release
	const uint8_t bad[8] = { 'A', 0, 1, 1, 0x0E, 0, 0, 0x07 };
	CHECK(feed(&m, bad, sizeof(bad), 50) == 0);
	CHECK(!m.ptt && m.key == ANYTONE_MIC_KEY_NONE && m.errors == 1);
	// Bytes that start no frame are skipped and counted; a UART error ends the frame under way
	const uint8_t junk[3] = { 0x00, 0xFF, 0x55 };
	CHECK(feed(&m, junk, sizeof(junk), 60) == 0);
	CHECK(m.errors == 4);
	const uint8_t half[4] = { 'A', 0, 1, 1 };
	CHECK(feed(&m, half, sizeof(half), 70) == 0);
	anytone_mic_error(&m);
	CHECK(frame(&m, 0, 1, ANYTONE_MIC_KEY_MENU, 80) == 1);
	CHECK(!m.ptt && m.key == ANYTONE_MIC_KEY_MENU);
}

// Whatever may have lost a frame lets go of PTT and the key, since the microphone sends nothing more until the next
// change: a UART error, a byte that starts no frame, a broken microphone frame
static void test_loss_releases(void)
{
	anytone_mic_t m;
	anytone_mic_init(&m);
	CHECK(frame(&m, 0, 0, ANYTONE_MIC_ID, 0) == 1);
	CHECK(frame(&m, 1, 0, 0, 10) == 1 && m.ptt);
	anytone_mic_error(&m);
	CHECK(!m.ptt && m.errors == 1 && m.present);
	CHECK(frame(&m, 1, 1, ANYTONE_MIC_KEY_PA, 20) == 1 && m.ptt && m.key == ANYTONE_MIC_KEY_PA);
	const uint8_t stray = 0x00;
	CHECK(feed(&m, &stray, 1, 30) == 0);
	CHECK(!m.ptt && m.key == ANYTONE_MIC_KEY_NONE && m.errors == 2);
	// The microphone's answers (0x06 runs) and other devices' frames don't
	CHECK(frame(&m, 1, 0, 0, 40) == 1 && m.ptt);
	const uint8_t run[4] = { 0x06, 0x06, 0x06, 0x06 };
	CHECK(feed(&m, run, sizeof(run), 50) == 0);
	const uint8_t f[7] = { 'F', 0, 0, 0, 0, 0, ANYTONE_MIC_END };
	CHECK(feed(&m, f, sizeof(f), 60) == 0);
	CHECK(m.ptt && m.errors == 2);
	// A UART error with no microphone (a plain microphone's PTT is a break) only counts
	anytone_mic_t q;
	anytone_mic_init(&q);
	anytone_mic_error(&q);
	CHECK(q.errors == 1 && !q.present && !q.ptt);
}

// A frame one byte short: the next frame's 'A' ends it, and starts that frame, so a release right after a damaged press
// still gets through
static void test_resync(void)
{
	anytone_mic_t m;
	anytone_mic_init(&m);
	CHECK(frame(&m, 0, 0, ANYTONE_MIC_ID, 0) == 1);
	const uint8_t short_press[7] = { 'A', 1, 0, 0, 0, 0, ANYTONE_MIC_END };	// byte 3 lost
	CHECK(feed(&m, short_press, sizeof(short_press), 10) == 0);
	// The release's 'A' ends the broken frame and starts its own: accepted
	CHECK(frame(&m, 0, 0, 0, 20) == 1);
	CHECK(m.errors == 1 && !m.ptt && m.frames == 2);
	// Two bytes short: the 'A' is found further in
	const uint8_t shorter[6] = { 'A', 0, 1, 0, 0x10, 0 };
	CHECK(feed(&m, shorter, sizeof(shorter), 30) == 0);
	CHECK(frame(&m, 1, 0, 0, 40) == 1);
	CHECK(m.errors == 2 && m.frames == 3 && m.ptt);
	// No 'A' in a broken frame: the parser waits for a type byte as usual
	const uint8_t broken[8] = { 'A', 0, 0, 0, 0, 0, 0, 0x00 };
	CHECK(feed(&m, broken, sizeof(broken), 50) == 0);
	CHECK(m.len == 0 && m.n == 0 && !m.ptt);
	CHECK(frame(&m, 1, 0, 0, 60) == 1 && m.ptt);
}

// Bytes through a DMA ring: in order across the wrap and across drains, a frame split between two drains, and the bytes
// the UART's error interrupt marked dropped as errors, their marks cleared
static void test_ring(void)
{
	enum { LEN = 64 };
	uint8_t ring[LEN];
	uint32_t err[LEN / 32] = { 0 };
	uint16_t rd = 0, wr = 0;
	anytone_mic_t m;
	anytone_mic_init(&m);
	// What the DMA does: the next byte at wr, wrapping
	#define PUT(b) do { ring[wr] = (uint8_t)(b); wr = (uint16_t)((wr + 1) % LEN); } while (0)
	const uint8_t presence[8] = { 'A', 0, 0, 0, ANYTONE_MIC_ID, 0, 0, ANYTONE_MIC_END };
	const uint8_t press[8] = { 'A', 1, 0, 0, 0, 0, 0, ANYTONE_MIC_END };
	const uint8_t release[8] = { 'A', 0, 0, 0, 0, 0, 0, ANYTONE_MIC_END };
	CHECK(anytone_mic_ring(&m, ring, err, LEN, &rd, wr, 0) == 0 && rd == 0);
	for (unsigned i = 0; i < 8; i++) {
		PUT(presence[i]);
	}
	CHECK(anytone_mic_ring(&m, ring, err, LEN, &rd, wr, 5) == 8 && rd == 8 && m.present && m.frames == 1);
	// Answers run the ring round: 0x06 bytes up to 4 before the end
	while (wr != LEN - 4) {
		PUT(0x06);
	}
	CHECK(anytone_mic_ring(&m, ring, err, LEN, &rd, wr, 6) == LEN - 12 && rd == LEN - 4);
	// The press across the wrap, drained in two parts
	for (unsigned i = 0; i < 5; i++) {
		PUT(press[i]);
	}
	CHECK(anytone_mic_ring(&m, ring, err, LEN, &rd, wr, 7) == 5 && rd == 1 && !m.ptt);
	for (unsigned i = 5; i < 8; i++) {
		PUT(press[i]);
	}
	CHECK(anytone_mic_ring(&m, ring, err, LEN, &rd, wr, 8) == 3 && rd == 4 && m.ptt && m.frames == 2 && m.errors == 0);
	// A byte of the release marked (a noise error): dropped with its frame, PTT let go all the same
	for (unsigned i = 0; i < 8; i++) {
		PUT(release[i]);
	}
	anytone_mic_ring_mark(err, (uint16_t)(rd + 3));
	CHECK(anytone_mic_ring(&m, ring, err, LEN, &rd, wr, 9) == 7 && m.frames == 2 && !m.ptt);
	CHECK(m.errors == 4 && err[0] == 0 && err[1] == 0);	// the mark, then three zeros that start no frame
	uint32_t errors = m.errors;
	// A mark on the last slot (the wrap) and on slot 32 (the second word): the next lap's bytes there are clean
	anytone_mic_ring_mark(err, LEN - 1);
	anytone_mic_ring_mark(err, 32);
	CHECK(err[0] == 0 && err[1] == (1u << 31 | 1u));
	while (wr != 33) {
		PUT(0x06);
	}
	CHECK(anytone_mic_ring(&m, ring, err, LEN, &rd, wr, 10) == (uint16_t)((33 - 12 + LEN) % LEN) - 1);
	CHECK(m.errors == errors + 1 && err[1] == (1u << 31));
	while (wr != 0) {
		PUT(0x06);
	}
	CHECK(anytone_mic_ring(&m, ring, err, LEN, &rd, wr, 11) == LEN - 33 - 1 && rd == 0);
	CHECK(m.errors == errors + 2 && err[0] == 0 && err[1] == 0);
	// A frame after the marks: clean
	for (unsigned i = 0; i < 8; i++) {
		PUT(press[i]);
	}
	CHECK(anytone_mic_ring(&m, ring, err, LEN, &rd, wr, 12) == 8 && m.ptt);
	#undef PUT
}

static void test_timeout(void)
{
	anytone_mic_t m;
	anytone_mic_init(&m);
	CHECK(frame(&m, 1, 1, ANYTONE_MIC_KEY_1 + 4, 1000) == 1);
	CHECK(m.key == 0x06);	// '5'
	CHECK(anytone_mic_poll(&m, 1000 + ANYTONE_MIC_TIMEOUT_MS - 1) && m.ptt);
	CHECK(!anytone_mic_poll(&m, 1000 + ANYTONE_MIC_TIMEOUT_MS));
	CHECK(!m.ptt && m.key == ANYTONE_MIC_KEY_NONE);
	// Across the millisecond counter's wrap
	CHECK(frame(&m, 1, 0, 0, 0xFFFFFF00u) == 1);
	CHECK(anytone_mic_poll(&m, 0x100u) && m.ptt);
	CHECK(!anytone_mic_poll(&m, 0xFFFFFF00u + ANYTONE_MIC_TIMEOUT_MS));
}

static bool is_status(const uint8_t *b)
{
	uint8_t want[ANYTONE_MIC_STATUS_LEN];
	anytone_mic_status(want, 0);
	for (unsigned i = 0; i < ANYTONE_MIC_STATUS_LEN; i++) {
		if (b[i] != want[i]) {
			return false;
		}
	}
	return true;
}

// The answers: one ANYTONE_MIC_ACK per accepted frame, a status frame after a presence frame (the microphone sends no
// PTT or key until it has one), at least ANYTONE_MIC_STATUS_GAP_MS apart, and one per ANYTONE_MIC_POLL_MS
static void test_answers(void)
{
	anytone_mic_t m;
	anytone_mic_init(&m);
	uint8_t out[ANYTONE_MIC_STATUS_LEN + 4];
	CHECK(anytone_mic_out(&m, 0, 0, out, sizeof(out)) == 0);	// nothing to a microphone not heard
	const uint8_t f[8] = { 'A', 0, 0, 0, ANYTONE_MIC_ID, 0, 0, ANYTONE_MIC_END };	// as a radio saw it
	uint32_t t = 5000;
	CHECK(feed(&m, f, sizeof(f), t) == 1);
	CHECK(anytone_mic_out(&m, t, 0, out, sizeof(out)) == 1 + ANYTONE_MIC_STATUS_LEN);
	CHECK(out[0] == ANYTONE_MIC_ACK && is_status(out + 1));
	CHECK(out[1] == 'S' && out[10] == 0x10 && out[13] == ANYTONE_MIC_STATUS_LONG && out[16] == 0x06);
	CHECK(anytone_mic_out(&m, t + 10, 0, out, sizeof(out)) == 0);
	// Presence again 50 ms later: answered at once, its status frame only once the gap is over
	CHECK(feed(&m, f, sizeof(f), t + 50) == 1);
	CHECK(anytone_mic_out(&m, t + 50, 0, out, sizeof(out)) == 1 && out[0] == ANYTONE_MIC_ACK);
	CHECK(anytone_mic_out(&m, t + ANYTONE_MIC_STATUS_GAP_MS - 1, 0, out, sizeof(out)) == 0);
	CHECK(anytone_mic_out(&m, t + ANYTONE_MIC_STATUS_GAP_MS, 0, out, sizeof(out)) == ANYTONE_MIC_STATUS_LEN);
	t += ANYTONE_MIC_STATUS_GAP_MS;
	// The poll
	CHECK(anytone_mic_out(&m, t + ANYTONE_MIC_POLL_MS - 1, 0, out, sizeof(out)) == 0);
	CHECK(anytone_mic_out(&m, t + ANYTONE_MIC_POLL_MS, 0, out, sizeof(out)) == ANYTONE_MIC_STATUS_LEN && is_status(out));
	// Two frames before the task runs: two answers, ahead of a due status frame
	CHECK(frame(&m, 1, 0, 0, t + 1500) == 1 && frame(&m, 0, 0, 0, t + 1505) == 1);
	CHECK(anytone_mic_out(&m, t + 2000, 0, out, sizeof(out)) == 2 + ANYTONE_MIC_STATUS_LEN);
	CHECK(out[0] == ANYTONE_MIC_ACK && out[1] == ANYTONE_MIC_ACK && is_status(out + 2));
	// No room for a status frame: the answers go, the status frame waits
	CHECK(frame(&m, 1, 0, 0, t + 3500) == 1);
	CHECK(anytone_mic_out(&m, t + 3500, 0, out, 4) == 1);
	CHECK(anytone_mic_out(&m, t + 3500, 0, out, sizeof(out)) == ANYTONE_MIC_STATUS_LEN);
}

// PTT held for longer than the timeout: the microphone sends nothing more of its own, but its answers to the polls
// keep it present and PTT on; once it stops answering (pulled out), PTT goes
static void test_held_ptt(void)
{
	anytone_mic_t m;
	anytone_mic_init(&m);
	uint8_t out[ANYTONE_MIC_STATUS_LEN + 4];
	uint32_t t = 100;
	CHECK(frame(&m, 1, 0, 0, t) == 1);
	const uint8_t ack = 0x06;
	for (uint32_t ms = t; ms < t + 3u * ANYTONE_MIC_TIMEOUT_MS; ms += 10) {
		if (anytone_mic_out(&m, ms, 0, out, sizeof(out)) >= ANYTONE_MIC_STATUS_LEN) {
			CHECK(feed(&m, &ack, 1, ms + 2) == 0);	// its answer; not a frame
		}
		CHECK(anytone_mic_poll(&m, ms + 5) && m.ptt);
	}
	uint32_t last = m.last_ms;
	CHECK(anytone_mic_poll(&m, last + ANYTONE_MIC_TIMEOUT_MS - 1) && m.ptt);
	CHECK(!anytone_mic_poll(&m, last + ANYTONE_MIC_TIMEOUT_MS) && !m.ptt);
	// Gone: nothing is sent, and nothing owed carries over to the next microphone
	CHECK(anytone_mic_out(&m, last + ANYTONE_MIC_TIMEOUT_MS + ANYTONE_MIC_POLL_MS, 0, out, sizeof(out)) == 0);
}

// The lights: each flag in its own byte, 1 or 0, and a change goes out without waiting for the poll
static void test_lights(void)
{
	uint8_t out[ANYTONE_MIC_STATUS_LEN + 4u];
	anytone_mic_status(out, ANYTONE_MIC_LIGHT_A_BUSY | ANYTONE_MIC_LIGHT_B_TX | ANYTONE_MIC_LIGHT_MAIN_B);
	CHECK(out[0] == 'S' && out[1] == 1 && out[2] == 0 && out[3] == 0 && out[4] == 0 && out[5] == 0 && out[6] == 1 && out[7] == 1);
	CHECK(out[8] == 0 && out[9] == 0x10 && out[12] == ANYTONE_MIC_STATUS_LONG && out[15] == 0x06);

	anytone_mic_t m;
	anytone_mic_init(&m);
	static const uint8_t presence[] = { 'A', 0, 0, 0, ANYTONE_MIC_ID, 0, 0, ANYTONE_MIC_END };
	uint32_t t = 10000;
	for (unsigned i = 0; i < sizeof(presence); i++) {
		anytone_mic_byte(&m, presence[i], t);
	}
	CHECK(anytone_mic_out(&m, t, 0, out, sizeof(out)) == 1 + ANYTONE_MIC_STATUS_LEN && out[1 + 1] == 0);
	// Unchanged: quiet until the poll
	CHECK(anytone_mic_out(&m, t + 300, 0, out, sizeof(out)) == 0);
	// A change inside the gap waits for it, then goes
	CHECK(anytone_mic_out(&m, t + 100, ANYTONE_MIC_LIGHT_A_TX, out, sizeof(out)) == 0);
	CHECK(anytone_mic_out(&m, t + ANYTONE_MIC_STATUS_GAP_MS, ANYTONE_MIC_LIGHT_A_TX, out, sizeof(out)) == ANYTONE_MIC_STATUS_LEN);
	CHECK(out[3] == 1 && out[1] == 0);
	t += ANYTONE_MIC_STATUS_GAP_MS;
	CHECK(anytone_mic_out(&m, t + 50, ANYTONE_MIC_LIGHT_A_TX, out, sizeof(out)) == 0);
	CHECK(anytone_mic_out(&m, t + ANYTONE_MIC_STATUS_GAP_MS, 0, out, sizeof(out)) == ANYTONE_MIC_STATUS_LEN && out[3] == 0);
}

// The lights follow the selected side, not the receivers
static void test_lights_sides(void)
{
	// No dual watch: everything is side A
	CHECK(anytone_mic_lights(&(anytone_mic_radio_t){ .rx = true, .sel = 1, .at = 1 }) ==
		(ANYTONE_MIC_LIGHT_A_BUSY | ANYTONE_MIC_LIGHT_A_AUDIO));
	// Seen on a microphone in dual watch: the green light is on the side the call is on, whichever is selected
	CHECK(anytone_mic_lights(&(anytone_mic_radio_t){ .dual = true, .sel = 1, .at = 1, .rx = true }) ==
		(ANYTONE_MIC_LIGHT_MAIN_B | ANYTONE_MIC_LIGHT_B_BUSY | ANYTONE_MIC_LIGHT_B_AUDIO));
	CHECK(anytone_mic_lights(&(anytone_mic_radio_t){ .dual = true, .sel = 1, .at = 1, .rx2_busy = true,
		.rx2_audio = true }) == (ANYTONE_MIC_LIGHT_MAIN_B | ANYTONE_MIC_LIGHT_A_BUSY | ANYTONE_MIC_LIGHT_A_AUDIO));
	CHECK(anytone_mic_lights(&(anytone_mic_radio_t){ .dual = true, .sel = 0, .at = 0, .rx = true }) ==
		(ANYTONE_MIC_LIGHT_A_BUSY | ANYTONE_MIC_LIGHT_A_AUDIO));
	CHECK(anytone_mic_lights(&(anytone_mic_radio_t){ .dual = true, .sel = 0, .at = 0, .rx2_busy = true,
		.rx2_audio = true }) == (ANYTONE_MIC_LIGHT_B_BUSY | ANYTONE_MIC_LIGHT_B_AUDIO));
	CHECK(anytone_mic_lights(&(anytone_mic_radio_t){ .tx = true }) == ANYTONE_MIC_LIGHT_A_TX);
	// B selected: the main light, and a transmission on B
	CHECK(anytone_mic_lights(&(anytone_mic_radio_t){ .dual = true, .sel = 1, .at = 1 }) == ANYTONE_MIC_LIGHT_MAIN_B);
	CHECK(anytone_mic_lights(&(anytone_mic_radio_t){ .dual = true, .sel = 1, .at = 1, .tx = true }) ==
		(ANYTONE_MIC_LIGHT_MAIN_B | ANYTONE_MIC_LIGHT_B_TX));
	// A selected, the receiver looking at B and hearing it: B busy, B's audio
	CHECK(anytone_mic_lights(&(anytone_mic_radio_t){ .dual = true, .sel = 0, .at = 1, .rx = true }) ==
		(ANYTONE_MIC_LIGHT_B_BUSY | ANYTONE_MIC_LIGHT_B_AUDIO));
	// The second receiver has the side not selected
	CHECK(anytone_mic_lights(&(anytone_mic_radio_t){ .dual = true, .sel = 0, .rx2_busy = true, .rx2_audio = true }) ==
		(ANYTONE_MIC_LIGHT_B_BUSY | ANYTONE_MIC_LIGHT_B_AUDIO));
	CHECK(anytone_mic_lights(&(anytone_mic_radio_t){ .dual = true, .sel = 1, .at = 1, .rx2_busy = true }) ==
		(ANYTONE_MIC_LIGHT_MAIN_B | ANYTONE_MIC_LIGHT_A_BUSY));
}

int main(void)
{
	test_presence_ptt_keys();
	test_other_frames_and_errors();
	test_loss_releases();
	test_resync();
	test_ring();
	test_timeout();
	test_answers();
	test_held_ptt();
	test_lights();
	test_lights_sides();
	if (failures) {
		printf("# anytoneMicTest FAILED (%d)\n", failures);
		return 1;
	}
	printf("# anytoneMicTest PASSED\n");
	return 0;
}
