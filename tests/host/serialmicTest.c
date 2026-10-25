// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The hand mic key decoder (drivers/input/serialmic): a held key, its release, a key change, single frames, UART
// errors, and the PC's traffic on the same line (packets split at any point, payloads full of "K01") never giving a
// key, with the mic working again once the host has been quiet for a second
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "input/serialmic/serialmic.h"

static int failures;
static int checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define BYTE_US		87u	// one character at 115200 8N1
#define FRAME_EVERY_US	2560u	// the mic's frame period

static serialmic_t m;
static uint32_t now;

static void bytes(const uint8_t *b, int n, bool error)
{
	for (int i = 0; i < n; i++) {
		serialmic_byte(&m, b[i], error, now);
		now += BYTE_US;
	}
}

static void mic_frame(int code, bool error)
{
	uint8_t f[3] = { 'K', (uint8_t)('0' + code / 10), (uint8_t)('0' + code % 10) };
	bytes(f, 3, error);
}

// Advance time, polling the key the way the 10 ms scan does; returns the last key seen
static uint8_t wait_us(uint32_t us)
{
	uint8_t k = serialmic_key(&m, now);
	while (us > 0) {
		uint32_t d = us < 1000u ? us : 1000u;
		now += d;
		us -= d;
		k = serialmic_key(&m, now);
	}
	return k;
}

// The mic holding a key for n frames; returns how many polls between frames saw the key
static int hold(int code, int n)
{
	int seen = 0;
	for (int i = 0; i < n; i++) {
		mic_frame(code, false);
		if (wait_us(FRAME_EVERY_US - 3u * BYTE_US) == code) {
			seen++;
		}
	}
	return seen;
}

static void test_press_release(void)
{
	serialmic_init(&m);
	now = 5000000u;
	mic_frame(1, false);
	CHECK(wait_us(FRAME_EVERY_US) == SERIALMIC_NONE);	// one frame isn't a key yet
	mic_frame(1, false);
	CHECK(wait_us(1000) == 1);
	CHECK(hold(1, 50) == 50);
	CHECK(wait_us(SERIALMIC_HELD_US - 5000u) == 1);		// still held just inside the timeout
	CHECK(wait_us(10000) == SERIALMIC_NONE);		// let go
}

static void test_key_change(void)
{
	serialmic_init(&m);
	now = 5000000u;
	hold(5, 10);
	CHECK(serialmic_key(&m, now) == 5);
	hold(18, 1);
	CHECK(serialmic_key(&m, now) == 5);			// one frame of the new key doesn't switch
	hold(18, 2);
	CHECK(serialmic_key(&m, now) == 18);
}

static void test_single_frames(void)
{
	serialmic_init(&m);
	now = 5000000u;
	for (int i = 0; i < 20; i++) {
		mic_frame(3, false);
		CHECK(wait_us(SERIALMIC_REPEAT_US + 1000u) == SERIALMIC_NONE);	// repeats too far apart
	}
}

static void test_errors_and_shapes(void)
{
	serialmic_init(&m);
	now = 5000000u;
	for (int i = 0; i < 10; i++) {
		mic_frame(7, true);					// framing errors
		wait_us(FRAME_EVERY_US);
	}
	CHECK(serialmic_key(&m, now) == SERIALMIC_NONE);
	static const uint8_t four[] = { 'K', '0', '7', '7' }, two[] = { 'K', '0' }, notk[] = { 'J', '0', '7' },
		notdig[] = { 'K', '0', 'A' };
	const uint8_t *shapes[] = { four, two, notk, notdig };
	const int lens[] = { 4, 2, 3, 3 };
	for (int s = 0; s < 4; s++) {
		for (int i = 0; i < 10; i++) {
			bytes(shapes[s], lens[s], false);
			wait_us(FRAME_EVERY_US);
		}
		CHECK(serialmic_key(&m, now) == SERIALMIC_NONE);
	}
	// A key frame glued to other bytes without a gap isn't one
	for (int i = 0; i < 10; i++) {
		static const uint8_t glued[] = { 0x11, 'K', '0', '7' };
		bytes(glued, 4, false);
		wait_us(FRAME_EVERY_US);
	}
	CHECK(serialmic_key(&m, now) == SERIALMIC_NONE);
}

// Host packets (A5 5A, then a payload of repeated "K01") cut into chunks of three bytes with gaps between them, the
// worst a USB-serial adapter could do: never a key. Then the mic works once the host has been quiet for a second
static void test_host_traffic(void)
{
	serialmic_init(&m);
	now = 5000000u;
	int keys = 0;
	for (int p = 0; p < 200; p++) {
		uint8_t pkt[2 + 3 * 20];
		pkt[0] = 0xA5;
		pkt[1] = 0x5A;
		for (int i = 0; i < 20; i++) {
			memcpy(&pkt[2 + 3 * i], "K01", 3);
		}
		bytes(pkt, 2, false);
		for (int i = 0; i < 20; i++) {
			wait_us(SERIALMIC_GAP_US + 200u);
			bytes(&pkt[2 + 3 * i], 3, false);
		}
		if (wait_us(3000) != SERIALMIC_NONE) {
			keys++;
		}
	}
	CHECK(keys == 0);
	// A mic key during host traffic is ignored too (the line can't carry both)
	hold(2, 20);
	CHECK(serialmic_key(&m, now) == SERIALMIC_NONE);
	wait_us(SERIALMIC_HOST_QUIET_US);
	hold(2, 3);
	CHECK(serialmic_key(&m, now) == 2);
	// Host traffic arriving while a key is held drops it at once
	static const uint8_t sync[] = { 0xA5, 0x5A };
	bytes(sync, 2, false);
	CHECK(serialmic_key(&m, now) == SERIALMIC_NONE);
}

// Random host traffic: packets with the sync, random payloads and random chunking and gaps. Never a key
static void test_random_host(void)
{
	serialmic_init(&m);
	now = 5000000u;
	srand(7);
	int keys = 0;
	for (int p = 0; p < 20000; p++) {
		int len = 4 + rand() % 120;
		uint8_t pkt[128];
		pkt[0] = 0xA5;
		pkt[1] = 0x5A;
		for (int i = 2; i < len; i++) {
			int r = rand() % 4;
			pkt[i] = r == 0 ? 'K' : r == 1 ? (uint8_t)('0' + rand() % 10) : (uint8_t)rand();
		}
		int i = 0;
		while (i < len) {
			int chunk = 1 + rand() % 6;
			if (chunk > len - i) {
				chunk = len - i;
			}
			bytes(&pkt[i], chunk, false);
			i += chunk;
			if (wait_us((uint32_t)(rand() % 3000)) != SERIALMIC_NONE) {
				keys++;
			}
		}
		if (wait_us((uint32_t)(rand() % 20000)) != SERIALMIC_NONE) {
			keys++;
		}
	}
	CHECK(keys == 0);
}

int main(void)
{
	test_press_release();
	test_key_change();
	test_single_frames();
	test_errors_and_shapes();
	test_host_traffic();
	test_random_host();
	printf("serialmicTest: %d checks, %d failures\n", checks, failures);
	return failures != 0;
}
