// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The support info (core/support, docs/spec/support-info.md) against a fake board: SHA-256 on the FIPS 180-4 examples
// fed whole and in pieces, the image hash stepped over a fake program flash (the same digest as at once, kept for the
// next caller), the hardware lines in their order with the parts a board leaves out skipped, and the debug link's
// SUPPORT replies (core/debuglink/debuglink_support.c, docs/spec/debuglink.md 3.29).
#include <stdio.h>
#include <string.h>
#include "board.h"
#include "debuglink/debuglink.h"
#include "debuglink/debuglink_proto.h"
#include "support/image_hash.h"
#include "support/sha256.h"
#include "support/support_info.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

// ---- The fake board ----

#define IMAGE_BASE	0x0800C000u
#define IMAGE_LEN	100003u		// not a multiple of a step or a read
static uint8_t image[IMAGE_LEN];
static uint32_t reads;			// bytes read() returned
static uint32_t now, ms_per_read;	// the fake clock (hal_millis), moved on by each read()
static bool preempt;			// the next read() is cut into by the debug link's step, as its task would
static bool preempt_ran;		// that step returned at once, reading nothing

uint32_t hal_millis(void)
{
	return now;
}

static bool chip(hal_chip_t *c)
{
	c->idcode = 0x10076413u;
	c->flash_kb = 1024;
	c->family = HAL_CHIP_STM32F4;
	return true;
}

static void sys_read(uint32_t addr, void *dst, size_t n)
{
	memcpy(dst, image + (addr - IMAGE_BASE), n);
	reads += (uint32_t)n;
	now += ms_per_read;
	if (preempt) {
		preempt = false;
		uint8_t d[SHA256_LEN];
		uint32_t before = reads, done = image_hash_done();
		preempt_ran = !image_hash_step(IMAGE_HASH_LINK, IMAGE_HASH_STEP_LINK, d) && reads == before &&
			image_hash_done() == done;
	}
}

static void sys_image(uint32_t *addr, uint32_t *len)
{
	*addr = IMAGE_BASE;
	*len = IMAGE_LEN;
}

static const hal_system_t sys = { .chip = chip, .read = sys_read, .image = sys_image };

static uint32_t jedec(void) { return 0xEF4018u; }
static hal_flash_t flash = { .jedec = jedec };

// Two RF chains and a batch code, as a board with two transceivers gives them
static bool hw_line(uint8_t i, uint8_t *role, char *buf, uint8_t len)
{
	static const char *const v[] = { "HR_C6000", "main BK4819 id 4819", "LO FD6818 id 6818", "batch DB6" };
	static const uint8_t r[] = { BOARD_HW_DMR, BOARD_HW_RF, BOARD_HW_RF, BOARD_HW_BOARD };
	if (i >= 4) {
		return false;
	}
	*role = r[i];
	snprintf(buf, len, "%s", v[i]);
	return true;
}

static int8_t rev = HWOPT_NONE;
static int8_t rev_get(void) { return rev; }
static bool rev_set(uint8_t c) { rev = (int8_t)c; return true; }
static const char *const revs[] = { "V1-V3", "V4" };
static const hal_hwopt_t opts[] = { { "Board revision", 2, revs, rev_get, rev_set, true, NULL } };

// No display panel hook: its line is left out
const board_t board = { .name = "test", .system = &sys, .flash = &flash, .hw_line = hw_line, .hwopts = opts, .hwopt_count = 1 };

// ---- SHA-256 ----

static void hex(const uint8_t *d, char *out)
{
	for (unsigned i = 0; i < SHA256_LEN; i++) {
		sprintf(out + 2 * i, "%02x", d[i]);
	}
}

static void check_sha(const char *msg, size_t len, const char *want)
{
	uint8_t d[SHA256_LEN];
	char h[2 * SHA256_LEN + 1];
	sha256_t s;
	sha256_init(&s);
	sha256_update(&s, msg, len);
	sha256_final(&s, d);
	hex(d, h);
	CHECK(strcmp(h, want) == 0);
	// The same in pieces of every length from 1 to 70: a block split anywhere
	for (size_t piece = 1; piece <= 70; piece++) {
		sha256_init(&s);
		for (size_t o = 0; o < len; o += piece) {
			sha256_update(&s, msg + o, len - o < piece ? len - o : piece);
		}
		sha256_final(&s, d);
		hex(d, h);
		if (strcmp(h, want) != 0) {
			printf("FAIL sha256 of %zu bytes in pieces of %zu\n", len, piece);
			failures++;
			break;
		}
	}
}

static void sha_tests(void)
{
	// FIPS 180-4 examples (the NIST example values for SHA-256)
	check_sha("", 0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
	check_sha("abc", 3, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
	const char *m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
	check_sha(m, strlen(m), "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
	// A million 'a'
	static char a[1000000];
	memset(a, 'a', sizeof a);
	uint8_t d[SHA256_LEN];
	char h[2 * SHA256_LEN + 1];
	sha256_t s;
	sha256_init(&s);
	sha256_update(&s, a, sizeof a);
	sha256_final(&s, d);
	hex(d, h);
	CHECK(strcmp(h, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0") == 0);
}

// ---- The image hash ----

static uint8_t want[SHA256_LEN];	// the image's SHA-256 at once

// After hash_steps finished it: the digest is the image's, and it is kept, so another caller has it at once
// with no read
static void image_tests(void)
{
	uint8_t got[SHA256_LEN];
	uint32_t addr, len;
	CHECK(image_hash_span(&addr, &len) && addr == IMAGE_BASE && len == IMAGE_LEN);
	CHECK(image_hash_known(got) && memcmp(got, want, SHA256_LEN) == 0);
	memset(got, 0, sizeof got);
	reads = 0;
	CHECK(image_hash_step(IMAGE_HASH_UI, 1u, got) && reads == 0 && memcmp(got, want, SHA256_LEN) == 0);
}

// ---- The hardware lines ----

static void line_tests(void)
{
	static const struct {
		const char *key, *value;
	} lines[] = {
		{ "mcu", "STM32F4 413 rev 1007, 1024 KB" },
		{ "flash", "EF4018" },
		{ "dmr", "HR_C6000" },
		{ "rf", "main BK4819 id 4819" },
		{ "rf", "LO FD6818 id 6818" },
		{ "board", "batch DB6" },
		{ "hwopt", "not set" },
	};
	support_line_t l;
	uint8_t n = 0;
	while (support_hw_line(n, &l)) {
		if (n < sizeof lines / sizeof lines[0]) {
			if (strcmp(l.key, lines[n].key) != 0 || strcmp(l.value, lines[n].value) != 0) {
				printf("FAIL line %u: %s=%s, want %s=%s\n", n, l.key, l.value, lines[n].key, lines[n].value);
				failures++;
			}
		}
		n++;
	}
	CHECK(n == sizeof lines / sizeof lines[0]);
	CHECK(support_hw_line(6, &l) && l.label == TR_NONE && strcmp(l.label_text, "Board revision") == 0);
	rev = 1;
	CHECK(support_hw_line(6, &l) && strcmp(l.value, "V4") == 0);
	CHECK(support_hw_line(0, &l) && strcmp(tr(l.label), "MCU: ") == 0);
	// A build without the Makefile's stamp: unknown, no repository
	CHECK(support_source() == SUPPORT_SRC_UNKNOWN && strcmp(support_source_key(support_source()), "unknown") == 0);
	CHECK(strcmp(support_repo(), "") == 0);
}

// ---- The debug link ----

static uint8_t out[1100];

static size_t cmd(uint8_t op)
{
	memset(out, 0, sizeof out);
	return debuglink_cmd_support(&op, 1, out, sizeof out);
}

static bool has(const char *s)
{
	return strstr((const char *)out + 1, s) != NULL;
}

static void link_tests(void)
{
	size_t n = cmd(DL_SUPPORT_OP_INFO);
	CHECK(n > 1 && out[0] == DL_ST_OK);
	CHECK(has("source=unknown\n"));
	CHECK(has("repo=\n"));
	CHECK(has("hw.mcu=STM32F4 413 rev 1007, 1024 KB\n"));
	CHECK(has("hw.flash=EF4018\n"));
	CHECK(has("hw.rf=main BK4819 id 4819\nhw.rf.2=LO FD6818 id 6818\n"));
	CHECK(has("hw.board=batch DB6\n"));
	CHECK(!has("hwopt"));
	CHECK(has("image.addr=0x0800C000\nimage.size=100003\n"));
	// The hash was kept by image_tests: done at once
	n = cmd(DL_SUPPORT_OP_HASH);
	CHECK(out[0] == DL_ST_OK && has("image.state=done\n") && has("image.sha256="));
	const char *p = strstr((const char *)out + 1, "image.sha256=");
	CHECK(p != NULL && strlen(p) == strlen("image.sha256=") + 2 * SHA256_LEN + 1);
	uint8_t bad[2] = { DL_SUPPORT_OP_INFO, 0 };
	CHECK(debuglink_cmd_support(bad, 2, out, sizeof out) == 1 && out[0] == DL_ST_BAD_ARG);
	uint8_t op = 9;
	CHECK(debuglink_cmd_support(&op, 1, out, sizeof out) == 1 && out[0] == DL_ST_BAD_ARG);
}

// A fresh hash, by both callers in turn: one hash, so each carries on where the other stopped. A step ends at its
// byte limit or after IMAGE_HASH_STEP_MS, and a step from the other task while one runs does nothing. image_hash keeps
// its result for the run, so this runs before anything finishes one
static void hash_steps(void)
{
	uint8_t got[SHA256_LEN];
	CHECK(!image_hash_known(got));
	reads = 0;
	size_t n = cmd(DL_SUPPORT_OP_HASH);
	CHECK(n > 1 && out[0] == DL_ST_OK && has("image.state=pending\nimage.done=32768\nimage.size=100003\n"));
	CHECK(reads == IMAGE_HASH_STEP_LINK);
	// A slow read: the step ends on time, after 5 reads of 256 bytes
	ms_per_read = 1;
	reads = 0;
	cmd(DL_SUPPORT_OP_HASH);
	CHECK(reads == IMAGE_HASH_STEP_MS * 256u && has("image.done=34048\n"));
	ms_per_read = 0;
	// About's step, cut into by the debug link's: that one waits, About's goes on from the link's progress
	preempt = true;
	preempt_ran = false;
	reads = 0;
	CHECK(!image_hash_step(IMAGE_HASH_UI, IMAGE_HASH_STEP_UI, got));
	CHECK(preempt_ran && reads == IMAGE_HASH_STEP_UI && image_hash_done() == 34048u + IMAGE_HASH_STEP_UI);
	// The link finishes it: the rest of the image, then done with its digest
	reads = 0;
	cmd(DL_SUPPORT_OP_HASH);
	CHECK(reads == IMAGE_LEN - 34048u - IMAGE_HASH_STEP_UI && has("image.state=done\n"));
	char h[2 * SHA256_LEN + 1], line[100];
	hex(want, h);
	snprintf(line, sizeof line, "image.sha256=%s\n", h);
	CHECK(has(line));
}

int main(void)
{
	for (uint32_t i = 0; i < IMAGE_LEN; i++) {
		image[i] = (uint8_t)(i * 7u + (i >> 9));
	}
	sha256_t s;
	sha256_init(&s);
	sha256_update(&s, image, IMAGE_LEN);
	sha256_final(&s, want);
	sha_tests();
	hash_steps();
	image_tests();
	line_tests();
	link_tests();
	if (failures) {
		printf("supportInfoTest: %d failure(s)\n", failures);
		return 1;
	}
	printf("supportInfoTest: all passed\n");
	return 0;
}
