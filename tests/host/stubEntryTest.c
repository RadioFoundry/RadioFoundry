// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The runtime RAM-monitor loader's portable state machine (core/stubentry/stub_load.c, docs/spec/debuglink.md 3.18):
// arming, chunked load, the magic / length / CRC-32 guards and the verified ENTER, and the STM32F4 stub image header
// rule ENTER applies while the IWDG runs (stub/stm32f4/f4stub_header.h).
#include <stdio.h>
#include <string.h>
#include "stubentry/stub_load.h"
#include "f4stub_header.h"

static int failures, checks;

#define CHECK(cond, ...)                                                                                            \
	do {                                                                                                        \
		checks++;                                                                                           \
		if (!(cond)) {                                                                                      \
			failures++;                                                                                 \
			printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                 \
			printf(__VA_ARGS__);                                                                        \
			printf("\n");                                                                               \
		}                                                                                                   \
	} while (0)

// stub_load.c links against this (core/storage/storage.c in the firmware); the standard CRC-32
uint32_t storage_crc32(uint32_t crc, const void *buf, uint32_t len)
{
	const uint8_t *p = buf;

	crc = ~crc;
	while (len-- > 0) {
		crc ^= *p++;
		for (int k = 0; k < 8; k++) {
			crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
		}
	}
	return ~crc;
}

#define LOAD	0x00017000u
#define CAP	64u

static void put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
	put16(p, (uint16_t)v);
	put16(p + 2, (uint16_t)(v >> 16));
}

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// The platform's image check (stub_load_t.image_ok): the length it was asked about
static uint32_t checked_len;

static bool refuse_image(const uint8_t *img, uint32_t len)
{
	(void)img;
	checked_len = len;
	return false;
}

static bool accept_image(const uint8_t *img, uint32_t len)
{
	(void)img;
	checked_len = len;
	return true;
}

// Run one request; returns the reply length, fills *enter / *entry (enter cleared first, as the firmware does)
static size_t req(stub_load_t *s, const uint8_t *r, uint16_t len, uint8_t *out, bool *enter, uint32_t *entry)
{
	*enter = false;
	return stub_load_request(s, r, len, out, CAP, enter, entry);
}

// The STM32's image check (platform/stm32f4/stub_entry.c) with the IWDG running: the shared header rule
static bool iwdg_running;
static bool f4_image_ok(const uint8_t *img, uint32_t len)
{
	return f4stub_image_ok(img, len, iwdg_running);
}

// BEGIN and DATA for a whole image; true when every chunk was taken
static bool load(stub_load_t *s, const uint8_t *img, uint32_t len, uint32_t entry_off)
{
	uint8_t r[CAP], out[CAP];
	bool enter;
	uint32_t entry;
	r[0] = STUB_OP_BEGIN;
	put32(r + 1, STUB_MAGIC);
	put32(r + 5, len);
	put32(r + 9, storage_crc32(0, img, len));
	put32(r + 13, LOAD + entry_off);
	if (req(s, r, 17, out, &enter, &entry) == 0 || out[0] != DL_ST_OK) {
		return false;
	}
	for (uint32_t off = 0; off < len; off += STUB_CHUNK_MAX) {
		uint32_t k = len - off < STUB_CHUNK_MAX ? len - off : STUB_CHUNK_MAX;
		r[0] = STUB_OP_DATA;
		put32(r + 1, off);
		memcpy(r + 5, img + off, k);
		if (req(s, r, (uint16_t)(5u + k), out, &enter, &entry) == 0 || out[0] != DL_ST_OK) {
			return false;
		}
	}
	return true;
}

static void put_header(uint8_t *img, uint32_t magic, uint32_t version, uint32_t flags)
{
	uint8_t *h = img + F4STUB_HDR_OFFSET;
	put32(h, magic);
	put16(h + 4, F4STUB_HDR_LAYOUT);
	put16(h + 6, F4STUB_HDR_SIZE);
	put32(h + 8, version);
	put32(h + 12, flags);
}

// The STM32F4 stub's image header (stub/stm32f4/f4stub_header.h, docs/spec/watchdog.md 3.1): with the IWDG running
// ENTER needs the magic, a version of at least F4STUB_VERSION_FEEDS_IWDG and the feed flag; without it, anything
static void test_f4_header(void)
{
	static uint8_t region[0x200];
	uint8_t img[200], r[CAP], out[CAP];
	bool enter;
	uint32_t entry;
	stub_load_t s;
	struct {
		const char *what;
		bool header;
		uint32_t magic, version, flags;
		bool ok_running;
	} cases[] = {
		{ "no header (an older stub)", false, 0, 0, 0, false },
		{ "wrong magic", true, F4STUB_HDR_MAGIC ^ 0x01000000u, F4STUB_VERSION, F4STUB_FLAG_FEEDS_IWDG, false },
		{ "an old version", true, F4STUB_HDR_MAGIC, F4STUB_VERSION_FEEDS_IWDG - 1u, F4STUB_FLAG_FEEDS_IWDG, false },
		{ "the feed version without the flag", true, F4STUB_HDR_MAGIC, F4STUB_VERSION_FEEDS_IWDG, 0, false },
		{ "the feed version", true, F4STUB_HDR_MAGIC, F4STUB_VERSION_FEEDS_IWDG, F4STUB_FLAG_FEEDS_IWDG, true },
		{ "a later version", true, F4STUB_HDR_MAGIC, F4STUB_VERSION + 1u, F4STUB_FLAG_FEEDS_IWDG | 0x80u, true },
	};
	for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
		for (unsigned i = 0; i < sizeof(img); i++) {
			img[i] = (uint8_t)(i * 13u + c);	// garbage where an old stub has code
		}
		if (cases[c].header) {
			put_header(img, cases[c].magic, cases[c].version, cases[c].flags);
		}
		for (int running = 1; running >= 0; running--) {
			iwdg_running = running != 0;
			bool want = !iwdg_running || cases[c].ok_running;
			CHECK(f4stub_image_ok(img, sizeof(img), iwdg_running) == want, "%s, IWDG %s: %s", cases[c].what,
			      iwdg_running ? "running" : "off", want ? "accepted" : "refused");
			// The same through ENTER
			stub_load_init(&s, region, sizeof(region), LOAD);
			s.image_ok = f4_image_ok;
			CHECK(load(&s, img, sizeof(img), 0), "%s: loaded", cases[c].what);
			r[0] = STUB_OP_ENTER;
			put32(r + 1, STUB_MAGIC);
			size_t n = req(&s, r, 5, out, &enter, &entry);
			if (want) {
				CHECK(n == 1 && out[0] == DL_ST_OK && enter, "%s, IWDG %s: ENTER ok (%u)", cases[c].what,
				      iwdg_running ? "running" : "off", out[0]);
			} else {
				CHECK(n == 2 && out[0] == DL_ST_REFUSED && out[1] == STUB_REFUSE_WATCHDOG && !enter,
				      "%s: ENTER refused for the watchdog (%u)", cases[c].what, out[0]);
			}
		}
	}
	CHECK(!f4stub_image_ok(img, F4STUB_HDR_OFFSET + F4STUB_HDR_SIZE - 1u, true), "an image too short for a header");
}

int main(void)
{
	static uint8_t region[0x200];
	uint8_t out[CAP], r[CAP];
	bool enter;
	uint32_t entry;
	stub_load_t s;

	// A test image: 130 bytes, entry at offset 8
	uint8_t img[130];
	for (unsigned i = 0; i < sizeof(img); i++) {
		img[i] = (uint8_t)(i * 7u + 3u);
	}
	uint32_t img_crc = storage_crc32(0, img, sizeof(img));
	uint32_t img_entry = LOAD + 8u;

	stub_load_init(&s, region, sizeof(region), LOAD);

	// INFO before anything: reports the span and no transfer armed
	r[0] = STUB_OP_INFO;
	size_t n = req(&s, r, 1, out, &enter, &entry);
	CHECK(n == 20 && out[0] == DL_ST_OK, "INFO len/status %zu %u", n, out[0]);
	CHECK(get32(out + 1) == STUB_MAGIC, "INFO magic");
	CHECK(get32(out + 5) == LOAD, "INFO load_addr 0x%x", get32(out + 5));
	CHECK(get32(out + 9) == sizeof(region), "INFO region_size %u", get32(out + 9));
	CHECK(out[15] == 0, "INFO not armed");
	CHECK(!enter, "INFO never enters");

	// DATA before BEGIN is refused
	r[0] = STUB_OP_DATA;
	put32(r + 1, 0);
	r[5] = 0xAB;
	n = req(&s, r, 6, out, &enter, &entry);
	CHECK(out[0] == DL_ST_BAD_ARG, "DATA before BEGIN rejected (%u)", out[0]);

	// BEGIN with a bad magic is refused
	r[0] = STUB_OP_BEGIN;
	put32(r + 1, STUB_MAGIC ^ 1u);
	put32(r + 5, sizeof(img));
	put32(r + 9, img_crc);
	put32(r + 13, img_entry);
	n = req(&s, r, 17, out, &enter, &entry);
	CHECK(out[0] == DL_ST_REFUSED, "BEGIN bad magic refused (%u)", out[0]);

	// BEGIN with an entry outside the image is a bad arg
	put32(r + 1, STUB_MAGIC);
	put32(r + 13, LOAD + sizeof(img));	// one past the end
	n = req(&s, r, 17, out, &enter, &entry);
	CHECK(out[0] == DL_ST_BAD_ARG, "BEGIN entry out of range (%u)", out[0]);

	// BEGIN too large for the region
	put32(r + 5, sizeof(region) + 1u);
	put32(r + 13, img_entry);
	n = req(&s, r, 17, out, &enter, &entry);
	CHECK(out[0] == DL_ST_BAD_ARG, "BEGIN too large (%u)", out[0]);

	// A good BEGIN arms the transfer
	put32(r + 5, sizeof(img));
	n = req(&s, r, 17, out, &enter, &entry);
	CHECK(n == 5 && out[0] == DL_ST_OK && get32(out + 1) == 0, "BEGIN ok");

	// ENTER before the image is complete is refused
	r[0] = STUB_OP_ENTER;
	put32(r + 1, STUB_MAGIC);
	n = req(&s, r, 5, out, &enter, &entry);
	CHECK(out[0] == DL_ST_BAD_ARG && !enter, "ENTER before complete rejected (%u)", out[0]);

	// Stream the image in STUB_CHUNK_MAX pieces
	uint32_t off = 0;
	while (off < sizeof(img)) {
		uint32_t c = sizeof(img) - off;
		if (c > STUB_CHUNK_MAX) {
			c = STUB_CHUNK_MAX;
		}
		r[0] = STUB_OP_DATA;
		put32(r + 1, off);
		memcpy(r + 5, img + off, c);
		n = req(&s, r, (uint16_t)(5u + c), out, &enter, &entry);
		CHECK(out[0] == DL_ST_OK && get32(out + 1) == off + c, "DATA @%u", off);
		off += c;
	}
	CHECK(memcmp(region, img, sizeof(img)) == 0, "image landed in the region");

	// A DATA chunk past the end is refused and does not overrun
	r[0] = STUB_OP_DATA;
	put32(r + 1, sizeof(img));
	r[5] = 0;
	n = req(&s, r, 6, out, &enter, &entry);
	CHECK(out[0] == DL_ST_BAD_ARG, "DATA past end rejected (%u)", out[0]);

	// ENTER with a bad magic is refused even with a complete image
	r[0] = STUB_OP_ENTER;
	put32(r + 1, STUB_MAGIC ^ 0xFFu);
	n = req(&s, r, 5, out, &enter, &entry);
	CHECK(out[0] == DL_ST_REFUSED && !enter, "ENTER bad magic refused (%u)", out[0]);

	// A good ENTER while the radio transmits answers BUSY and doesn't fire; the image stays loaded
	put32(r + 1, STUB_MAGIC);
	s.transmitting = true;
	n = req(&s, r, 5, out, &enter, &entry);
	CHECK(n == 1 && out[0] == DL_ST_BUSY && !enter, "ENTER while transmitting busy (%u)", out[0]);
	s.transmitting = false;

	// A platform check that refuses the image (the STM32 with its IWDG running and a stub that doesn't feed it):
	// refused with the watchdog reason, nothing fires, and the image stays loaded
	s.image_ok = refuse_image;
	n = req(&s, r, 5, out, &enter, &entry);
	CHECK(n == 2 && out[0] == DL_ST_REFUSED && out[1] == STUB_REFUSE_WATCHDOG && !enter,
	      "ENTER refused for the watchdog (%u, %u)", out[0], out[1]);
	CHECK(checked_len == sizeof(img), "the check saw the whole image (%u)", checked_len);
	s.image_ok = accept_image;

	// A good ENTER verifies the CRC-32 and fires, with the right entry address
	n = req(&s, r, 5, out, &enter, &entry);
	CHECK(n == 1 && out[0] == DL_ST_OK, "ENTER ok (%u)", out[0]);
	CHECK(enter && entry == img_entry, "ENTER fires at 0x%x (want 0x%x)", entry, img_entry);

	// Corrupt one byte: re-arm, load with a flipped byte, ENTER must fail the CRC-32 and not fire
	r[0] = STUB_OP_BEGIN;
	put32(r + 1, STUB_MAGIC);
	put32(r + 5, sizeof(img));
	put32(r + 9, img_crc);
	put32(r + 13, img_entry);
	(void)req(&s, r, 17, out, &enter, &entry);
	off = 0;
	while (off < sizeof(img)) {
		uint32_t c = sizeof(img) - off;
		if (c > STUB_CHUNK_MAX) {
			c = STUB_CHUNK_MAX;
		}
		r[0] = STUB_OP_DATA;
		put32(r + 1, off);
		memcpy(r + 5, img + off, c);
		if (off == 0) {
			r[5] ^= 0x80u;	// flip a byte of the first chunk
		}
		(void)req(&s, r, (uint16_t)(5u + c), out, &enter, &entry);
		off += c;
	}
	r[0] = STUB_OP_ENTER;
	put32(r + 1, STUB_MAGIC);
	n = req(&s, r, 5, out, &enter, &entry);
	CHECK(out[0] == DL_ST_BAD_ARG && !enter, "ENTER rejects a bad CRC-32 (%u)", out[0]);

	// A gap (a skipped chunk reads back as the zeroed region) also fails the CRC-32
	(void)req(&s, (uint8_t[]){ STUB_OP_BEGIN }, 1, out, &enter, &entry);	// malformed BEGIN: stays unarmed
	r[0] = STUB_OP_BEGIN;
	put32(r + 1, STUB_MAGIC);
	put32(r + 5, sizeof(img));
	put32(r + 9, img_crc);
	put32(r + 13, img_entry);
	(void)req(&s, r, 17, out, &enter, &entry);
	r[0] = STUB_OP_DATA;			// write only the first chunk, leave a gap
	put32(r + 1, 0);
	memcpy(r + 5, img, STUB_CHUNK_MAX);
	(void)req(&s, r, (uint16_t)(5u + STUB_CHUNK_MAX), out, &enter, &entry);
	r[0] = STUB_OP_ENTER;
	put32(r + 1, STUB_MAGIC);
	n = req(&s, r, 5, out, &enter, &entry);
	CHECK(out[0] == DL_ST_BAD_ARG && !enter, "ENTER rejects an incomplete image (%u)", out[0]);

	test_f4_header();

	printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "ok", checks, failures);
	return failures ? 1 : 0;
}
