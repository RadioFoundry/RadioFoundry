// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The debug link's portable parts: the printf subset, the CRC, the frame parser and dispatcher, the dlog ring
// (core/debuglink, docs/spec/debuglink.md)
#include <stdio.h>
#include <string.h>
#include "debuglink/debuglink_proto.h"
#include "debuglink/dl_fmt.h"
#include "debuglink/dlog.h"
#include "debuglink/dl_chip.h"
#include "debuglink/dl_shot.h"
#include "cps/cps_lz4.h"
#include "../../platform/stm32f4/mem_window.h"
#include "../../platform/hrc7000/mem_window.h"

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
	if (len > 0) memcpy(f + DL_HDR, payload, len);	// payload may be NULL for a command without one
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

static uint32_t get32le(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Read a packed screenshot in replies of at most cap bytes, as the host does, and decode it
static void shot_read(const dl_shot_t *s, size_t cap, const char *what)
{
	static uint8_t out[DL_REPLY_MAX], fb[0x10000];
	cps_lz4_dec_t dec;
	uint32_t offset = 0, total = 0;
	size_t hdr = DL_SHOT_HDR + (s->bpp == 4 ? 32u : 0u);
	bool last = false;
	int replies = 0;

	cps_lz4_dec_init(&dec, fb, s->size);
	while (!last && replies < 1000) {
		size_t n = dl_shot_reply(s, offset, out, cap, &last);
		CHECK(n > hdr && n <= cap, "%s: reply of %zu bytes", what, n);
		if (n <= hdr || n > cap)
			return;
		CHECK(out[0] == DL_ST_OK && out[5] == s->bpp && out[6] == DL_SHOT_LZ4, "%s: header", what);
		CHECK((out[1] | out[2] << 8) == s->width && (out[3] | out[4] << 8) == s->height && out[7] == s->insets[0] &&
		      out[10] == s->insets[3], "%s: geometry", what);
		CHECK(get32le(out + 15) == offset && get32le(out + 19) == s->size && get32le(out + 23) == s->frame &&
		      out[27] == s->drawing, "%s: offset, size, frame", what);
		if (s->bpp == 4)
			CHECK((out[DL_SHOT_HDR + 2] | out[DL_SHOT_HDR + 3] << 8) == s->palette[1], "%s: palette", what);
		total = get32le(out + 11);
		CHECK(cps_lz4_dec_feed(&dec, out + hdr, (uint32_t)(n - hdr)), "%s: stream at %u", what, (unsigned)offset);
		offset += (uint32_t)(n - hdr);
		replies++;
	}
	CHECK(last && offset == total && cps_lz4_dec_done(&dec), "%s: whole stream (%u of %u)", what, (unsigned)offset,
	      (unsigned)total);
	CHECK(memcmp(fb, s->fb, s->size) == 0, "%s: decodes to the frame buffer", what);
	CHECK(dl_shot_reply(s, total + 1u, out, cap, &last) == 0, "%s: offset past the end", what);
}

static void test_shot(void)
{
	static uint8_t fb[240 * 320 / 2];
	dl_shot_t s = { .fb = fb, .size = 80 * 128, .width = 160, .height = 128, .bpp = 4, .insets = { 8, 0, 0, 0 },
			.frame = 0x12345678u, .drawing = true };
	for (int i = 0; i < 16; i++)
		s.palette[i] = (uint16_t)(0x1111u * (unsigned)i);
	// A screen-like frame: a background, a title bar, text-like noise in a few rows
	memset(fb, 0x00, sizeof fb);
	memset(fb, 0x33, 80 * 16);
	uint32_t r = 1;
	for (int y = 40; y < 60; y++)
		for (int x = 4; x < 76; x++) {
			r = r * 1103515245u + 12345u;
			fb[y * 80 + x] = (uint8_t)(r >> 24);
		}
	shot_read(&s, DL_REPLY_MAX, "4 bpp");
	shot_read(&s, DL_SHOT_HDR + 32u + 7u, "4 bpp, 7-byte windows");
	// The largest panel: 38400 bytes, one LZ4 block
	s.width = 240;
	s.height = 320;
	s.size = 120 * 320;
	s.drawing = false;
	shot_read(&s, DL_REPLY_MAX, "240 x 320");
	s.bpp = 1;
	s.width = 128;
	s.height = 64;
	s.size = 16 * 64;
	shot_read(&s, DL_REPLY_MAX, "1 bpp");
	uint8_t out[DL_SHOT_HDR];
	bool last;
	CHECK(dl_shot_reply(&s, 0, out, sizeof out, &last) == 0, "no room for a byte");
}

static bool stm1024(unsigned i, hal_mem_window_t *w)
{
	return stm32_mem_window_for(i, w, 1024);
}
static bool stm512(unsigned i, hal_mem_window_t *w)
{
	return stm32_mem_window_for(i, w, 512);
}
static bool stm0(unsigned i, hal_mem_window_t *w)
{
	return stm32_mem_window_for(i, w, 0);
}
static bool hrc1m(unsigned i, hal_mem_window_t *w)
{
	return hrc_mem_window_for(i, w, 0x100000u);
}
static bool hrc2m(unsigned i, hal_mem_window_t *w)
{
	return hrc_mem_window_for(i, w, 0x200000u);
}

static void test_chip(void)
{
	// Option bytes: RDP levels, write protection, BOR, user bits
	CHECK(dl_rdp_level(0x0FFFAAEDu) == 0 && dl_rdp_byte(0x0FFFAAEDu) == 0xAA, "RDP 0");
	CHECK(dl_rdp_level(0x0FFFCCEDu) == 2, "RDP 2");
	CHECK(dl_rdp_level(0x0FFF55EDu) == 1 && dl_rdp_level(0x0FFF00EDu) == 1, "RDP 1: anything else");
	CHECK(dl_wrp_mask(0x0FFFAAEDu) == 0, "no WRP");
	CHECK(dl_wrp_mask(0x0FF8AAEDu) == 0x007, "WRP sectors 0-2: 0x%03X", dl_wrp_mask(0x0FF8AAEDu));
	CHECK(dl_bor_level(0x0FFFAAEDu) == 3 && dl_user_bits(0x0FFFAAEDu) == 7, "BOR off, user bits");

	// READ_MEM windows: the STM32F4's
	CHECK(dl_mem_allowed(0x08000000u, 1000, stm1024), "stock bootloader start");
	CHECK(dl_mem_allowed(0x080FFC18u, 1000, stm1024), "flash end exact");
	CHECK(!dl_mem_allowed(0x080FFC19u, 1000, stm1024), "past the flash end");
	CHECK(!dl_mem_allowed(0x0807FFFFu, 2, stm512), "a 512 KB part ends at 0x0807FFFF");
	CHECK(dl_mem_allowed(0x080FFFFFu, 1, stm0), "flash size unreadable: 1 MB");
	CHECK(!dl_mem_allowed(0x08000000u, 0, stm1024), "zero length");
	CHECK(!dl_mem_allowed(0x07FFFFFFu, 2, stm1024), "below flash");
	CHECK(dl_mem_allowed(0x1FFF0000u, 1000, stm1024) && dl_mem_allowed(0x1FFF7400u, 0x400, stm1024), "system memory");
	CHECK(!dl_mem_allowed(0x1FFF7700u, 0x200, stm1024), "across system memory and OTP: refused");
	CHECK(dl_mem_allowed(0x1FFF7800u, 0x210, stm1024) && !dl_mem_allowed(0x1FFF7800u, 0x211, stm1024), "OTP");
	CHECK(!dl_mem_allowed(0x1FFF7A10u, 12, stm1024), "UID is INFO_CHIP's, not READ_MEM's");
	CHECK(dl_mem_allowed(0x1FFFC000u, 16, stm1024) && !dl_mem_allowed(0x1FFFC000u, 17, stm1024), "option bytes");
	CHECK(!dl_mem_allowed(0x20000000u, 4, stm1024) && !dl_mem_allowed(0x10000000u, 4, stm1024), "RAM, CCM refused");
	CHECK(!dl_mem_allowed(0x40023C00u, 4, stm1024) && !dl_mem_allowed(0xE0042000u, 4, stm1024), "peripherals refused");
	CHECK(!dl_mem_allowed(0xFFFFFFF0u, 0x20, stm1024) && !dl_mem_allowed(0x08000010u, 0xFFFFFFF8u, stm1024),
	      "wrap-around refused");

	CHECK(!dl_mem_allowed(0x08000000u, 4, NULL), "no windows");

	// The HR_C7000's: memory byte by byte, register blocks in aligned words only
	CHECK(dl_mem_allowed(0x00010000u, 1000, hrc1m) && dl_mem_allowed(0x00057C18u, 1000, hrc1m), "main SRAM");
	CHECK(!dl_mem_allowed(0x00057C19u, 1000, hrc1m) && !dl_mem_allowed(0x0000FFFFu, 2, hrc1m), "past main SRAM");
	CHECK(dl_mem_allowed(0x00010001u, 3, hrc1m), "SRAM: any alignment");
	CHECK(dl_mem_allowed(0x18000000u, 1000, hrc1m) && !dl_mem_allowed(0x18007FFFu, 2, hrc1m), "shared SRAM");
	CHECK(dl_mem_allowed(0x030FFC18u, 1000, hrc1m) && !dl_mem_allowed(0x030FFC19u, 1000, hrc1m), "1 MB flash end");
	CHECK(dl_mem_allowed(0x031FFC18u, 1000, hrc2m) && !dl_mem_allowed(0x03100000u, 4, hrc1m), "2 MB flash");
	CHECK(dl_mem_allowed(0x14100040u, 4, hrc1m) && dl_mem_allowed(0x14100000u, 0x80, hrc1m), "GPIO B words");
	CHECK(dl_mem_allowed(0x14020000u, 0x80, hrc1m) && dl_mem_allowed(0x14110000u, 0x80, hrc1m), "GPIO A, C");
	CHECK(!dl_mem_allowed(0x14100040u, 2, hrc1m) && !dl_mem_allowed(0x14100042u, 4, hrc1m) &&
	      !dl_mem_allowed(0x14100041u, 1, hrc1m), "GPIO: unaligned or partial words refused");
	CHECK(!dl_mem_allowed(0x1410007Cu, 8, hrc1m), "past the GPIO block");
	CHECK(dl_mem_allowed(0x17000000u, 0x90, hrc1m) && dl_mem_allowed(0x17000068u, 4, hrc1m), "PIC");
	CHECK(dl_mem_allowed(0x1100003Cu, 4, hrc1m) && !dl_mem_allowed(0x11000060u, 4, hrc1m), "system block, not the modem");
	CHECK(!dl_mem_allowed(0x14000000u, 4, hrc1m) && !dl_mem_allowed(0x1400000Cu, 4, hrc1m), "timers refused (EOI)");
	CHECK(!dl_mem_allowed(0x14010000u, 4, hrc1m), "watchdog refused");
	CHECK(!dl_mem_allowed(0x14030000u, 4, hrc1m) && !dl_mem_allowed(0x14040000u, 4, hrc1m), "UARTs refused");
	CHECK(!dl_mem_allowed(0x140A0060u, 4, hrc1m), "SPI data register refused");
	CHECK(!dl_mem_allowed(0x16000980u, 4, hrc1m) && !dl_mem_allowed(0x11000100u, 4, hrc1m), "modem refused");
	CHECK(!dl_mem_allowed(0xFFFFFFFCu, 8, hrc1m) && !dl_mem_allowed(0x00010000u, 0xFFFFFFF0u, hrc1m),
	      "wrap-around refused");

	// REBOOT gate
	hal_chip_t c = { .uid = { 0x12345678u, 1, 2 }, .optcr = 0x0FFFAAEDu, .flash_kb = 1024 };
	CHECK(dl_reboot_check(HAL_REBOOT_ROM_DFU, 0x12345678u, &c, false) == DL_REBOOT_OK, "ROM DFU at RDP 0");
	CHECK(dl_reboot_check(HAL_REBOOT_NORMAL, 0x12345678u, &c, false) == DL_REBOOT_OK, "normal");
	CHECK(dl_reboot_check(HAL_REBOOT_ROM_DFU, 0x12345679u, &c, false) == DL_REBOOT_BAD_TOKEN, "bad token");
	CHECK(dl_reboot_check(HAL_REBOOT_NORMAL, 0, &c, false) == DL_REBOOT_BAD_TOKEN, "normal needs the token too");
	CHECK(dl_reboot_check(HAL_REBOOT_ROM_DFU, 0x12345678u, &c, true) == DL_REBOOT_TX, "TX");
	CHECK(dl_reboot_check(HAL_REBOOT_NORMAL, 0x12345678u, &c, true) == DL_REBOOT_TX, "TX, normal");
	CHECK(dl_reboot_check(HAL_REBOOT_ROM_DFU, 0x12345678u, NULL, false) == DL_REBOOT_NO_CHIP, "no chip");
	c.optcr = 0x0FFF55EDu;
	CHECK(dl_reboot_check(HAL_REBOOT_ROM_DFU, 0x12345678u, &c, false) == DL_REBOOT_RDP, "RDP 1 refused");
	CHECK(dl_reboot_check(HAL_REBOOT_NORMAL, 0x12345678u, &c, false) == DL_REBOOT_OK, "RDP 1: normal reset fine");
	c.optcr = 0x0FFFCCEDu;
	CHECK(dl_reboot_check(HAL_REBOOT_ROM_DFU, 0x12345678u, &c, false) == DL_REBOOT_RDP, "RDP 2 refused");
	// A chip without option bytes (HR_C7000): the UID token works, there is no ROM path to offer
	hal_chip_t h = { .uid = { 0x9ABCDEF0u, 3, 4 }, .family = HAL_CHIP_HRC7000 };
	CHECK(dl_reboot_check(HAL_REBOOT_NORMAL, 0x9ABCDEF0u, &h, false) == DL_REBOOT_OK, "HR_C7000 normal");
	CHECK(dl_reboot_check(HAL_REBOOT_NORMAL, 0x9ABCDEF1u, &h, false) == DL_REBOOT_BAD_TOKEN, "HR_C7000 token");
	CHECK(dl_reboot_check(HAL_REBOOT_ROM_DFU, 0x9ABCDEF0u, &h, false) == DL_REBOOT_RDP, "HR_C7000 no ROM DFU");
}

int main(void)
{
	test_fmt();
	test_crc();
	test_parser();
	test_dlog();
	test_chip();
	test_shot();
	printf("# debuglinkTest: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
