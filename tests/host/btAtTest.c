// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A Bluetooth module's AT lines (drivers/input/bt_at): presence, the APRS board told apart, the three PTT sources and
// the key events in the data tunnel, a dropped link or a restarted module releasing PTT, and bad bytes and long lines
// dropped
#include <stdio.h>
#include "input/bt_at/bt_at.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static void feed(bt_at_t *b, const char *s)
{
	while (*s) {
		bt_at_byte(b, (uint8_t)*s++);
	}
}

// An +ADATA line and its payload, as the module sends them
static void tunnel(bt_at_t *b, const char *payload)
{
	char head[32];
	snprintf(head, sizeof(head), "+ADATA:01,%u\r\n", (unsigned)strlen(payload));
	feed(b, head);
	feed(b, payload);
}

static void test_presence(void)
{
	bt_at_t b;
	bt_at_init(&b);
	CHECK(!b.present);
	feed(&b, "\r\n+VER:ET12_AQQX_V10043\r\n");
	CHECK(b.present && !b.aprs_board && b.lines == 1 && b.errors == 0);
	bt_at_init(&b);
	feed(&b, "ERR\r\n");
	CHECK(b.present);
	bt_at_init(&b);
	feed(&b, "+VER:APRS_V200\r\n");
	CHECK(b.present && b.aprs_board);
}

static void test_ptt_sources(void)
{
	bt_at_t b;
	bt_at_init(&b);
	feed(&b, "+PTTKEY=1\r\n");
	CHECK(b.fob_ptt && bt_at_ptt(&b));
	feed(&b, "+PTTKEY=0\r\n");
	CHECK(!bt_at_ptt(&b));
	// the hands-free button toggles; 0 clears
	feed(&b, "+HFREQ:1\r\n");
	CHECK(b.hf_ptt);
	feed(&b, "+HFREQ:1\r\n");
	CHECK(!b.hf_ptt);
	feed(&b, "+HFREQ:1\r\n+HFREQ:0\r\n");
	CHECK(!b.hf_ptt);
	// the hand microphone's lines come in the tunnel; the same text outside it does nothing
	feed(&b, "+PTT=P\r\n");
	CHECK(!b.mic_ptt);
	tunnel(&b, "+PTT=P\r\n");
	CHECK(b.mic_ptt);
	tunnel(&b, "+PTT=R\r\n");
	CHECK(!b.mic_ptt);
	// a line after the payload is the module's again
	tunnel(&b, "+PTT=P\r\n");
	feed(&b, "+PTTKEY=1\r\n");
	CHECK(b.mic_ptt && b.fob_ptt && b.payload == 0);
	CHECK(b.errors == 0);
}

static void test_keys(void)
{
	bt_at_t b;
	bt_at_init(&b);
	tunnel(&b, "+KEY=03\r\n");
	CHECK(b.key == 3 && b.key_events == 1);
	tunnel(&b, "+KEY=01\r\n+KEY=02\r\n");
	CHECK(b.key == 2 && b.key_events == 3);
	// codes the stock firmware ignores, and malformed ones
	tunnel(&b, "+KEY=05\r\n+KEY=1\r\n+KEY=0x\r\n");
	CHECK(b.key == 2 && b.key_events == 3);
}

static void test_release(void)
{
	bt_at_t b;
	const char *ends[] = { "+DISC=0011223344AA,PTT-1\r\n", "+READY\r\n", "+CONN=000000000000,\r\n" };
	for (unsigned i = 0; i < 3; i++) {
		bt_at_init(&b);
		feed(&b, "+PTTKEY=1\r\n+HFREQ:1\r\n");
		tunnel(&b, "+PTT=P\r\n");
		CHECK(bt_at_ptt(&b));
		feed(&b, ends[i]);
		CHECK(!bt_at_ptt(&b));
	}
	// a connection to a real address doesn't
	bt_at_init(&b);
	feed(&b, "+PTTKEY=1\r\n+CONN=0011223344AA,PTT-1\r\n");
	CHECK(bt_at_ptt(&b));
}

static void test_errors(void)
{
	bt_at_t b;
	bt_at_init(&b);
	// bytes that start no line
	feed(&b, "xyz");
	CHECK(b.errors == 3 && b.lines == 0);
	// a line too long is dropped whole, and the next one is read
	char lng[BT_AT_LINE_MAX + 20];
	memset(lng, 'A', sizeof(lng));
	lng[0] = '+';
	lng[sizeof(lng) - 1] = '\0';
	feed(&b, lng);
	feed(&b, "\r\n+PTTKEY=1\r\n");
	CHECK(b.errors == 4 && b.fob_ptt && b.lines == 1);
	// a UART error ends the line under way
	feed(&b, "+PTTK");
	bt_at_error(&b);
	feed(&b, "EY=0\r\n");
	CHECK(b.fob_ptt);
	feed(&b, "+PTTKEY=0\r\n");
	CHECK(!b.fob_ptt);
}

int main(void)
{
	test_presence();
	test_ptt_sources();
	test_keys();
	test_release();
	test_errors();
	if (failures) {
		printf("# btAtTest FAILED (%d)\n", failures);
		return 1;
	}
	printf("# btAtTest PASSED\n");
	return 0;
}
