// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The AnyTone serial hand microphone's frames (drivers/input/anytone_mic): presence, PTT and keys from 'A' frames, the
// other frame types skipped whole, broken frames and UART errors dropped, and the microphone dropped with its PTT
// after the timeout
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

static int frame(anytone_mic_t *m, uint8_t ptt, uint8_t valid, uint8_t code, uint32_t now)
{
	const uint8_t f[8] = { 'A', ptt, 0x01, valid, code, 0x00, 0x00, ANYTONE_MIC_END };
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
	// The valid flag clear: no key held
	CHECK(frame(&m, 0, 0, ANYTONE_MIC_KEY_UP, 500) == 1);
	CHECK(m.key == ANYTONE_MIC_KEY_NONE);
	CHECK(m.frames == 5 && m.errors == 0);
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
	CHECK(anytone_mic_poll(&m, 1000 + ANYTONE_MIC_TIMEOUT_MS - 1) && m.ptt);
	CHECK(!anytone_mic_poll(&m, 1000 + ANYTONE_MIC_TIMEOUT_MS));
	CHECK(!m.ptt && m.key == ANYTONE_MIC_KEY_NONE);
	// Across the millisecond counter's wrap
	CHECK(frame(&m, 1, 0, 0, 0xFFFFFF00u) == 1);
	CHECK(anytone_mic_poll(&m, 0x100u) && m.ptt);
	CHECK(!anytone_mic_poll(&m, 0xFFFFFF00u + ANYTONE_MIC_TIMEOUT_MS));
}

int main(void)
{
	test_presence_ptt_keys();
	test_other_frames_and_errors();
	test_timeout();
	if (failures) {
		printf("# anytoneMicTest FAILED (%d)\n", failures);
		return 1;
	}
	printf("# anytoneMicTest PASSED\n");
	return 0;
}
