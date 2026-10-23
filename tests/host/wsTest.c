// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// WebSocket pieces (core/net/ws.c): SHA-1 against the FIPS 180-2 appendix examples, base64 against RFC 4648 10, the
// handshake against RFC 6455 1.3, frame headers against RFC 6455 5.7 and the server's rules (5.1, 5.2, 5.5)
#include <stdio.h>
#include <string.h>
#include "net/ws.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static void hex(const uint8_t *p, size_t n, char *out)
{
	for (size_t i = 0; i < n; i++) {
		sprintf(out + 2 * i, "%02x", p[i]);
	}
}

static bool sha1_is(const void *data, size_t len, const char *want)
{
	uint8_t d[20];
	char s[41];

	ws_sha1(data, len, d);
	hex(d, sizeof(d), s);
	if (strcmp(s, want) != 0) {
		printf("  sha1 of %zu bytes: %s, want %s\n", len, s, want);
		return false;
	}
	return true;
}

static bool b64_is(const char *in, const char *want)
{
	char out[32];

	ws_base64((const uint8_t *)in, strlen(in), out);
	return strcmp(out, want) == 0;
}

static int parse(const uint8_t *p, size_t n, ws_hdr_t *h)
{
	return ws_parse_header(p, n, 1024, h);
}

int main(void)
{
	// SHA-1: the three FIPS examples, the empty message, and every length around the padding's block boundary
	static uint8_t big[1000000];

	CHECK(sha1_is("abc", 3, "a9993e364706816aba3e25717850c26c9cd0d89d"));
	CHECK(sha1_is("", 0, "da39a3ee5e6b4b0d3255bfef95601890afd80709"));
	CHECK(sha1_is("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56,
		      "84983e441c3bd26ebaae4aa1f95129e5e54670f1"));
	memset(big, 'a', sizeof(big));
	CHECK(sha1_is(big, sizeof(big), "34aa973cd4c4daa4f61eeb2bdbad27316534016f"));
	CHECK(sha1_is(big, 55, "c1c8bbdc22796e28c0e15163d20899b65621d65a"));
	CHECK(sha1_is(big, 64, "0098ba824b5c16427bd7a1122a5a442a25ec644d"));

	// Base64
	CHECK(b64_is("", ""));
	CHECK(b64_is("f", "Zg=="));
	CHECK(b64_is("fo", "Zm8="));
	CHECK(b64_is("foo", "Zm9v"));
	CHECK(b64_is("foob", "Zm9vYg=="));
	CHECK(b64_is("fooba", "Zm9vYmE="));
	CHECK(b64_is("foobar", "Zm9vYmFy"));

	// The handshake: RFC 6455's example key, and keys that are not 16 bytes of base64
	char acc[WS_ACCEPT_LEN + 1];
	static const char key[] = "dGhlIHNhbXBsZSBub25jZQ==";

	CHECK(ws_accept(key, strlen(key), acc));
	CHECK(strcmp(acc, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") == 0);
	CHECK(!ws_accept(key, strlen(key) - 1, acc));
	CHECK(!ws_accept("dGhlIHNhbXBsZSBub25jZQ=x", 24, acc));
	CHECK(!ws_accept("dGhlIHNhbXBsZSBub2*jZQ==", 24, acc));
	CHECK(!ws_accept("dGhlIHNhbXBsZSBub25jZQ==AAAA", 28, acc));

	// RFC 6455 5.7: a masked "Hello" from a client
	uint8_t hello[] = { 0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d, 0x7f, 0x9f, 0x4d, 0x51, 0x58 };
	ws_hdr_t h;

	for (size_t n = 0; n < 6; n++) {
		CHECK(parse(hello, n, &h) == WS_NEED_MORE);
	}
	CHECK(parse(hello, sizeof(hello), &h) == 6);
	CHECK(h.opcode == WS_OP_TEXT && h.len == 5 && h.hlen == 6);
	ws_unmask(hello + 6, 2, h.mask, 0);	// in two pieces, as it may come off TCP
	ws_unmask(hello + 8, 3, h.mask, 2);
	CHECK(memcmp(hello + 6, "Hello", 5) == 0);

	// The server's frames: RFC 6455 5.7's unmasked "Hello", a 256 byte and a 64 KiB binary header
	uint8_t out[16];

	CHECK(ws_frame_header(out, WS_OP_TEXT, 5) == 2 && out[0] == 0x81 && out[1] == 0x05);
	CHECK(ws_frame_header(out, WS_OP_BINARY, 125) == 2 && out[1] == 125);
	CHECK(ws_frame_header(out, WS_OP_BINARY, 256) == 4);
	CHECK(memcmp(out, "\x82\x7e\x01\x00", 4) == 0);
	CHECK(ws_frame_header(out, WS_OP_BINARY, 65536) == 10);
	CHECK(memcmp(out, "\x82\x7f\x00\x00\x00\x00\x00\x01\x00\x00", 10) == 0);

	// The three length forms as a client sends them, masked, and the limit
	static const uint8_t m16[] = { 0x82, 0xfe, 0x01, 0x00, 1, 2, 3, 4 };
	static const uint8_t m64[] = { 0x82, 0xff, 0, 0, 0, 0, 0, 0, 0x03, 0xe8, 1, 2, 3, 4 };
	static const uint8_t m64big[] = { 0x82, 0xff, 0, 0, 0, 1, 0, 0, 0, 0, 1, 2, 3, 4 };
	static const uint8_t m64msb[] = { 0x82, 0xff, 0x80, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4 };

	CHECK(parse(m16, sizeof(m16) - 1, &h) == WS_NEED_MORE);
	CHECK(parse(m16, sizeof(m16), &h) == 8 && h.len == 256 && h.mask[3] == 4);
	CHECK(parse(m64, sizeof(m64) - 1, &h) == WS_NEED_MORE);
	CHECK(parse(m64, sizeof(m64), &h) == 14 && h.len == 1000);
	CHECK(ws_parse_header(m64, sizeof(m64), 999, &h) == WS_ERR_TOO_BIG);
	CHECK(parse(m64big, sizeof(m64big), &h) == WS_ERR_TOO_BIG);
	CHECK(parse(m64msb, sizeof(m64msb), &h) == WS_ERR_PROTOCOL);

	// Refused: unmasked, RSV bits, a fragment and its continuation, reserved opcodes, a long control frame
	static const uint8_t unmasked[] = { 0x81, 0x05 };
	static const uint8_t rsv1[] = { 0xc1, 0x80 };
	static const uint8_t frag[] = { 0x01, 0x83 };
	static const uint8_t cont[] = { 0x80, 0x83 };
	static const uint8_t op3[] = { 0x83, 0x80 };
	static const uint8_t opb[] = { 0x8b, 0x80 };
	static const uint8_t bigping[] = { 0x89, 0xfe, 0x00, 0x7e };
	static const uint8_t ping[] = { 0x89, 0xfd, 1, 2, 3, 4 };
	static const uint8_t cls[] = { 0x88, 0x82, 1, 2, 3, 4 };

	CHECK(parse(unmasked, sizeof(unmasked), &h) == WS_ERR_PROTOCOL);
	CHECK(parse(rsv1, sizeof(rsv1), &h) == WS_ERR_PROTOCOL);
	CHECK(parse(frag, sizeof(frag), &h) == WS_ERR_PROTOCOL);
	CHECK(parse(cont, sizeof(cont), &h) == WS_ERR_PROTOCOL);
	CHECK(parse(op3, sizeof(op3), &h) == WS_ERR_PROTOCOL);
	CHECK(parse(opb, sizeof(opb), &h) == WS_ERR_PROTOCOL);
	CHECK(parse(bigping, sizeof(bigping), &h) == WS_ERR_PROTOCOL);
	CHECK(parse(ping, sizeof(ping), &h) == 6 && h.opcode == WS_OP_PING && h.len == 125);
	CHECK(parse(cls, sizeof(cls), &h) == 6 && h.opcode == WS_OP_CLOSE && h.len == 2);

	printf("%s: %d failure(s)\n", __FILE__, failures);
	return failures ? 1 : 0;
}
