// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The AnyTone serial hand microphone (drivers/input/anytone_mic): presence, PTT and keys from 'A' frames, the other
// frame types skipped whole, broken frames and UART errors dropped; the radio's answers and status frames, the poll
// that keeps a silent microphone present, and the microphone dropped with its PTT once it stops answering
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
	// A frame without 0x06 at its end is dropped
	const uint8_t bad[8] = { 'A', 0, 1, 1, 0x0E, 0, 0, 0x07 };
	CHECK(feed(&m, bad, sizeof(bad), 50) == 0);
	CHECK(m.ptt && m.key == ANYTONE_MIC_KEY_NONE);
	// Bytes that start no frame are skipped; a UART error ends the frame under way
	const uint8_t junk[3] = { 0x00, 0xFF, 0x55 };
	CHECK(feed(&m, junk, sizeof(junk), 60) == 0);
	CHECK(m.errors == 3);
	const uint8_t half[4] = { 'A', 0, 1, 1 };
	CHECK(feed(&m, half, sizeof(half), 70) == 0);
	anytone_mic_error(&m);
	CHECK(frame(&m, 0, 1, ANYTONE_MIC_KEY_MENU, 80) == 1);
	CHECK(!m.ptt && m.key == ANYTONE_MIC_KEY_MENU);
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
		(ANYTONE_MIC_LIGHT_A_BUSY | ANYTONE_MIC_LIGHT_MAIN_AUDIO));
	CHECK(anytone_mic_lights(&(anytone_mic_radio_t){ .tx = true }) == ANYTONE_MIC_LIGHT_A_TX);
	// B selected: the main light, and a transmission on B
	CHECK(anytone_mic_lights(&(anytone_mic_radio_t){ .dual = true, .sel = 1, .at = 1 }) == ANYTONE_MIC_LIGHT_MAIN_B);
	CHECK(anytone_mic_lights(&(anytone_mic_radio_t){ .dual = true, .sel = 1, .at = 1, .tx = true }) ==
		(ANYTONE_MIC_LIGHT_MAIN_B | ANYTONE_MIC_LIGHT_B_TX));
	// A selected, the receiver looking at B and hearing it: B busy, the other side's audio
	CHECK(anytone_mic_lights(&(anytone_mic_radio_t){ .dual = true, .sel = 0, .at = 1, .rx = true }) ==
		(ANYTONE_MIC_LIGHT_B_BUSY | ANYTONE_MIC_LIGHT_OTHER_AUDIO));
	// The second receiver has the side not selected
	CHECK(anytone_mic_lights(&(anytone_mic_radio_t){ .dual = true, .sel = 0, .rx2_busy = true, .rx2_audio = true }) ==
		(ANYTONE_MIC_LIGHT_B_BUSY | ANYTONE_MIC_LIGHT_OTHER_AUDIO));
	CHECK(anytone_mic_lights(&(anytone_mic_radio_t){ .dual = true, .sel = 1, .at = 1, .rx2_busy = true }) ==
		(ANYTONE_MIC_LIGHT_MAIN_B | ANYTONE_MIC_LIGHT_A_BUSY));
}

int main(void)
{
	test_presence_ptt_keys();
	test_other_frames_and_errors();
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
