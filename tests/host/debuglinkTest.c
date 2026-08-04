// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The debug link's portable parts: the printf subset, the CRC, the frame parser and dispatcher, the dlog ring
// (core/debuglink, docs/spec/debuglink.md)
#include <stdio.h>
#include <string.h>
#include "debuglink/debuglink_proto.h"
#include "debuglink/dl_fmt.h"
#include "debuglink/dlog.h"

static int failures, checks;

#define CHECK(cond, ...)                                                                                           \
	do {                                                                                                       \
		checks++;                                                                                          \
		if (!(cond)) {                                                                                     \
			failures++;                                                                                \
			printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                \
			printf(__VA_ARGS__);                                                                       \
			printf("\n");                                                                              \
		}                                                                                                  \
	} while (0)

// --- fakes ---

static uint32_t fake_ms;
uint32_t hal_millis(void)
{
	return fake_ms;
}

static uint8_t written[4096];
static size_t written_len;
static int writes;

static void fake_write(const uint8_t *buf, size_t len)
{
	if (written_len + len <= sizeof(written)) {
		memcpy(written + written_len, buf, len);
		written_len += len;
	}
	writes++;
}

static int handled;
static uint8_t last_cmd;
static uint16_t last_len;

static size_t fake_handler(uint8_t cmd, const uint8_t *req, uint16_t len, uint8_t *out, size_t cap)
{
	handled++;
	last_cmd = cmd;
	last_len = len;
	if (cmd == 0x01) {	// echo
		out[0] = DL_ST_OK;
		memcpy(out + 1, req, len);
		return 1u + len;
	}
	if (cmd == 0x05) {	// a large reply: the whole capacity
		out[0] = DL_ST_OK;
		for (size_t i = 1; i < cap; i++) {
			out[i] = (uint8_t)i;
		}
		return cap;
	}
	out[0] = DL_ST_UNKNOWN_CMD;
	return 1;
}

static size_t make_frame(uint8_t *f, uint8_t cmd, uint8_t seq, const uint8_t *payload, uint16_t len)
{
	memcpy(f + DL_HDR, payload, len);
	return dl_frame(f, cmd, seq, len);
}

static uint8_t reply_buf[DL_HDR + DL_REPLY_MAX + 2];

// Check one reply frame at written+off: returns its length, 0 if malformed
static size_t check_reply(size_t off, uint8_t cmd, uint8_t seq, uint8_t status)
{
	const uint8_t *r = written + off;
	if (written_len - off < DL_HDR + 2u) {
		return 0;
	}
	uint16_t len = (uint16_t)(r[4] | (r[5] << 8));
	if (written_len - off < DL_HDR + len + 2u) {
		return 0;
	}
	uint16_t crc = dl_crc16(DL_CRC_INIT, r + 2, len + 4u);
	CHECK(r[0] == DL_SOF1 && r[1] == DL_SOF2, "reply SOF");
	CHECK(r[2] == cmd, "reply cmd 0x%02X, expected 0x%02X", r[2], cmd);
	CHECK(r[3] == seq, "reply seq %u, expected %u", r[3], seq);
	CHECK(len >= 1 && r[DL_HDR] == status, "reply status %u, expected %u", len ? r[DL_HDR] : 0xFF, status);
	CHECK((r[DL_HDR + len] | (r[DL_HDR + len + 1] << 8)) == crc, "reply CRC");
	return DL_HDR + len + 2u;
}

static void reset_out(void)
{
	written_len = 0;
	writes = 0;
	handled = 0;
}

// --- tests ---

static void test_fmt(void)
{
	char b[64];

	dl_fmt(b, sizeof(b), "a%db%uc%xd%Xe%sf%cg%%", -12, 34u, 0xabu, 0xCDu, "str", 'Z');
	CHECK(strcmp(b, "a-12b34cabdCDestrfZg%") == 0, "fmt basic: %s", b);
	dl_fmt(b, sizeof(b), "[%5d][%05d][%08x][%3s][%q]", 42, -42, 0x1234u, "ab");
	CHECK(strcmp(b, "[   42][-0042][00001234][ ab][%q]") == 0, "fmt width, unknown conversion shown as is: %s", b);
	dl_fmt(b, sizeof(b), "%ld %lu %d", -2147483647L - 1, 4294967295UL, 0);
	CHECK(strcmp(b, "-2147483648 4294967295 0") == 0, "fmt extremes: %s", b);
	size_t n = dl_fmt(b, 6, "%s", "truncated");
	CHECK(n == 5 && strcmp(b, "trunc") == 0, "fmt truncation: %zu %s", n, b);
	dl_fmt(b, sizeof(b), "%s", (const char *)NULL);
	CHECK(strcmp(b, "(null)") == 0, "fmt null string");

	dl_text_t t;
	char tb[16];
	dl_text_init(&t, tb, sizeof(tb));
	dl_text(&t, "k=%u\n", 1u);
	dl_text(&t, "long=%s\n", "0123456789");
	CHECK(t.full && t.len == 4 && memcmp(tb, "k=1\n", 4) == 0, "text: whole pieces only (len %zu)", t.len);
	dl_text_init(&t, tb, sizeof(tb));
	dl_text_kv_str(&t, "n", "a\nb\x01");
	CHECK(t.len == 7 && memcmp(tb, "n=a?b?\n", 7) == 0, "text kv: control characters replaced");
}

static void test_crc(void)
{
	CHECK(dl_crc16(DL_CRC_INIT, (const uint8_t *)"123456789", 9) == 0x29B1, "CRC-16/CCITT-FALSE check value");
}

static void test_parser(void)
{
	dl_parser_t p;
	uint8_t f[128];
	size_t n, took;

	dl_parser_init(&p);
	fake_ms = 1000;

	// A whole PING frame
	reset_out();
	n = make_frame(f, 0x01, 7, (const uint8_t *)"hi", 2);
	took = dl_parser_feed(&p, f, n, fake_ms, fake_handler, fake_write, reply_buf);
	CHECK(took == n, "ping: took %zu of %zu", took, n);
	CHECK(handled == 1 && last_cmd == 0x01 && last_len == 2, "ping handled");
	CHECK(check_reply(0, 0x81, 7, DL_ST_OK) == DL_HDR + 3u + 2u, "ping reply");
	CHECK(memcmp(written + DL_HDR + 1, "hi", 2) == 0, "ping echo");
	CHECK(!dl_parser_in_frame(&p, fake_ms), "idle after a frame");

	// The same frame a byte at a time, then followed by another protocol's bytes in the same chunk
	reset_out();
	for (size_t i = 0; i < n; i++) {
		CHECK(dl_parser_feed(&p, f + i, 1, fake_ms, fake_handler, fake_write, reply_buf) == 1, "byte %zu", i);
		if (i + 1 < n) {
			CHECK(dl_parser_in_frame(&p, fake_ms), "in frame at byte %zu", i);
		}
	}
	CHECK(handled == 1, "byte-wise frame handled");
	uint8_t two[160];
	memcpy(two, f, n);
	two[n] = 0xE0;
	two[n + 1] = 0x03;
	reset_out();
	took = dl_parser_feed(&p, two, n + 2, fake_ms, fake_handler, fake_write, reply_buf);
	CHECK(took == n, "stops at the end of its frame: took %zu", took);

	// Bad CRC
	reset_out();
	f[n - 1] ^= 0x55;
	took = dl_parser_feed(&p, f, n, fake_ms, fake_handler, fake_write, reply_buf);
	CHECK(took == n && handled == 0, "bad CRC not handled");
	CHECK(check_reply(0, DL_CMD_ERROR, 7, DL_ST_BAD_CRC) > 0, "bad CRC error reply");
	CHECK(written[DL_HDR + 1] == 0x01, "error names the command");
	CHECK(p.bad_crc == 1, "bad CRC counted");

	// Not SOF2 after SOF1: only the SOF1 is taken
	reset_out();
	uint8_t junk[] = { DL_SOF1, 0xE0, 0x03, 0x00 };
	took = dl_parser_feed(&p, junk, sizeof(junk), fake_ms, fake_handler, fake_write, reply_buf);
	CHECK(took == 1 && writes == 0, "SOF1 then junk: took %zu", took);
	CHECK(!dl_parser_in_frame(&p, fake_ms), "back to idle");
	CHECK(dl_parser_feed(&p, junk + 1, 3, fake_ms, fake_handler, fake_write, reply_buf) == 0, "not ours: 0");

	// Too long
	reset_out();
	uint8_t big[] = { DL_SOF1, DL_SOF2, 0x01, 9, (uint8_t)(DL_REQ_MAX + 1), 0 };
	took = dl_parser_feed(&p, big, sizeof(big), fake_ms, fake_handler, fake_write, reply_buf);
	CHECK(took == sizeof(big) && handled == 0, "too long: took %zu", took);
	CHECK(check_reply(0, DL_CMD_ERROR, 9, DL_ST_TOO_LONG) > 0, "too long error reply");

	// A partial frame times out
	reset_out();
	n = make_frame(f, 0x01, 3, (const uint8_t *)"abc", 3);
	dl_parser_feed(&p, f, 5, fake_ms, fake_handler, fake_write, reply_buf);
	CHECK(dl_parser_in_frame(&p, fake_ms + DL_FRAME_TIMEOUT_MS), "still in frame at the limit");
	CHECK(!dl_parser_in_frame(&p, fake_ms + DL_FRAME_TIMEOUT_MS + 1), "dropped after the limit");
	CHECK(p.timeouts == 1, "timeout counted");
	fake_ms += DL_FRAME_TIMEOUT_MS + 1;
	took = dl_parser_feed(&p, f, n, fake_ms, fake_handler, fake_write, reply_buf);
	CHECK(took == n && handled == 1, "a fresh frame after the timeout");

	// Unknown command, and the largest reply
	reset_out();
	n = make_frame(f, 0x42, 1, NULL, 0);
	dl_parser_feed(&p, f, n, fake_ms, fake_handler, fake_write, reply_buf);
	CHECK(check_reply(0, 0xC2, 1, DL_ST_UNKNOWN_CMD) > 0, "unknown command reply");
	reset_out();
	n = make_frame(f, 0x05, 2, NULL, 0);
	dl_parser_feed(&p, f, n, fake_ms, fake_handler, fake_write, reply_buf);
	CHECK(check_reply(0, 0x85, 2, DL_ST_OK) == DL_HDR + DL_REPLY_MAX + 2u, "full-size reply");
}

static void test_dlog(void)
{
	char buf[4096];
	uint8_t more;
	uint32_t dropped;
	size_t n;

	fake_ms = 5;
	dlog("hello %d", 42);
	dlog("two\nlines");
	n = dlog_read(buf, sizeof(buf), &more, &dropped);
	buf[n] = '\0';
	CHECK(strcmp(buf, "5 hello 42\n5 two lines\n") == 0, "dlog lines: '%s'", buf);
	CHECK(!more && dropped == 0, "dlog empty after read");

	// Overflow drops whole old lines and counts them
	for (int i = 0; i < 200; i++) {
		dlog("line %03d padding padding padding", i);
	}
	n = dlog_read(buf, sizeof(buf), &more, &dropped);
	buf[n] = '\0';
	CHECK(dropped > 0 && n <= DLOG_RING_BYTES, "dlog overflow: dropped %u, read %zu", dropped, n);
	CHECK(strstr(buf, "line 199 ") != NULL && strstr(buf, "line 000 ") == NULL, "newest kept, oldest dropped");
	CHECK(buf[0] == '5' && buf[n - 1] == '\n', "whole lines");

	// A small read buffer gets whole lines and says more remain
	dlog("aaaa");
	dlog("bbbb");
	n = dlog_read(buf, 8, &more, &dropped);
	CHECK(n == 7 && more, "partial read: %zu, more %u", n, more);
	n = dlog_read(buf, sizeof(buf), &more, &dropped);
	CHECK(n == 7 && !more, "rest: %zu", n);

	// A long line is cut
	char longmsg[200];
	memset(longmsg, 'x', sizeof(longmsg) - 1);
	longmsg[sizeof(longmsg) - 1] = '\0';
	dlog("%s", longmsg);
	n = dlog_read(buf, sizeof(buf), &more, &dropped);
	CHECK(n == 2 + DLOG_LINE_MAX + 1, "long line cut to DLOG_LINE_MAX: %zu", n);
}

int main(void)
{
	test_fmt();
	test_crc();
	test_parser();
	test_dlog();
	printf("# debuglinkTest: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
