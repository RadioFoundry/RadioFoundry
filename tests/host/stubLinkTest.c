// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The stub entry on the wire (docs/spec/stub-entry.md): request frames in, reply frames out, through the framing
// (core/debuglink/dl_frame.c), the loader (core/stubentry/stub_load.c) and the listener of a build without the debug
// link (core/stubentry/stub_link.c).
//
// The golden replies were captured from the debug link's STUB path before the stub entry became its own feature
// (main a63eb47d). Both listeners must still produce them byte for byte: the debug link's (its dispatcher hands 0x1A to
// stub_entry_request) and the stub entry's own (stub_link_handle). The glue below is stub_entry.c's request function
// without the board: ENTER while transmitting is busy before the image is looked at.
#include <stdio.h>
#include <string.h>
#include "stubentry/stub_load.h"
#include "stubentry/stub_link.h"

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

// --- the stub entry's request function, as stub_entry.c has it, over a RAM span here ---

static uint8_t region[256];
static stub_load_t stub;
static bool on_air;
static bool entered;

static size_t entry_request(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap)
{
	if (len >= 1 && req[0] == STUB_OP_ENTER && on_air) {
		out[0] = DL_ST_BUSY;
		return 1;
	}
	bool enter = false;
	uint32_t entry = 0;
	stub.transmitting = on_air;
	size_t n = stub_load_request(&stub, req, len, out, cap, &enter, &entry);
	if (enter) {
		entered = true;
	}
	return n;
}

// The debug link's dispatcher for this command (debuglink.c handle): 0x1A goes to the stub entry
static size_t debuglink_handle(uint8_t cmd, const uint8_t *req, uint16_t len, uint8_t *out, size_t cap)
{
	if (cmd == DL_CMD_STUB) {
		return entry_request(req, len, out, cap);
	}
	out[0] = DL_ST_UNKNOWN_CMD;
	return 1;
}

static const char *codec_id(void)
{
	return "board-2501";
}

static const stub_link_t ident = { .board = "Board", .git = "0123abcd", .codec = codec_id, .stub = entry_request };

static size_t own_handle(uint8_t cmd, const uint8_t *req, uint16_t len, uint8_t *out, size_t cap)
{
	return stub_link_handle(&ident, cmd, req, len, out, cap);
}

// --- the port ---

static uint8_t sent[2048];
static size_t sent_len;

static void port_write(const uint8_t *buf, size_t len)
{
	if (sent_len + len <= sizeof(sent)) {
		memcpy(sent + sent_len, buf, len);
		sent_len += len;
	}
}

static size_t unhex(const char *h, uint8_t *out)
{
	size_t n = 0;

	for (; h[0] != '\0' && h[1] != '\0'; h += 2) {
		unsigned v;
		sscanf(h, "%2x", &v);
		out[n++] = (uint8_t)v;
	}
	return n;
}

static void hex(const uint8_t *p, size_t n, char *out)
{
	for (size_t i = 0; i < n; i++) {
		sprintf(out + 2 * i, "%02x", p[i]);
	}
	out[2 * n] = '\0';
}

// Request frame -> the expected reply frame, captured from main a63eb47d: a 40-byte image (byte i = 7i + 1) into a
// 256-byte span at 0x30000
static const struct {
	const char *what, *req, *reply;
	bool on_air;
} golden[] = {
	{ "INFO", "a55a1a010100007c12", "a55a9a011400004337534200000300000100003b0000000000009159", false },
	{ "ENTER unarmed", "a55a1a0205000343375342d83a", "a55a9a02010002328b", false },
	{ "BEGIN bad magic", "a55a1a0311000142375342280000003ed348c500000300a8ac", "a55a9a0301000723ad", false },
	{ "BEGIN", "a55a1a0411000143375342280000003ed348c500000300958c", "a55a9a04050000000000006c6d", false },
	{ "DATA 0", "a55a1a052300020000000001080f161d242b323940474e555c636a71787f868d949ba2a9b0b7bec5cc6cac",
	  "a55a9a050500001e0000004293", false },
	{ "DATA 30", "a55a1a060f00021e000000d3dae1e8eff6fd040b12d15a", "a55a9a06050000280000004750", false },
	{ "INFO armed", "a55a1a07010000e535", "a55a9a071400004337534200000300000100003b000128000000560f", false },
	{ "ENTER on air", "a55a1a0805000343375342a4ab", "a55a9a08010003b8f3", true },
	{ "ENTER bad magic", "a55a1a09050003bc375342d4a7", "a55a9a0901000788c5", false },
	{ "ENTER", "a55a1a0a050003433753420224", "a55a9a0a010000b32e", false },
	{ "bad op", "a55a1a0b010009feeb", "a55a9a0b0100024578", false },
};

static void run_golden(const char *listener, dl_handler_fn handler, uint16_t reply_cap)
{
	static dl_parser_t p;
	static uint8_t reply[DL_HDR + DL_REPLY_MAX + 2u];
	uint8_t req[96];
	char got[2 * sizeof(sent) + 1];

	dl_parser_init(&p);
	p.reply_cap = reply_cap;
	stub_load_init(&stub, region, sizeof(region), 0x30000u);
	entered = false;
	for (size_t i = 0; i < sizeof(golden) / sizeof(golden[0]); i++) {
		size_t n = unhex(golden[i].req, req);
		on_air = golden[i].on_air;
		sent_len = 0;
		size_t took = dl_parser_feed(&p, req, n, 0, handler, port_write, reply);
		hex(sent, sent_len, got);
		CHECK(took == n, "%s %s: took %zu of %zu", listener, golden[i].what, took, n);
		CHECK(strcmp(got, golden[i].reply) == 0, "%s %s: reply %s, want %s", listener, golden[i].what, got,
		      golden[i].reply);
		CHECK(entered == (i >= 9), "%s %s: entered %d", listener, golden[i].what, entered);
	}
}

// One request through the stub entry's own listener; the reply frame's payload (status first) in *payload
static size_t own_request(dl_parser_t *p, uint8_t *reply, uint8_t cmd, const uint8_t *pl, uint16_t len,
			  const uint8_t **payload)
{
	uint8_t f[DL_HDR + DL_REQ_MAX + 2u];

	if (len != 0) {		// memcpy from NULL is undefined even for 0 bytes (UBSan)
		memcpy(f + DL_HDR, pl, len);
	}
	size_t n = dl_frame(f, cmd, 0x42, len);
	sent_len = 0;
	(void)dl_parser_feed(p, f, n, 0, own_handle, port_write, reply);
	*payload = sent + DL_HDR;
	return sent_len >= DL_HDR + 2u ? (size_t)(sent[4] | (sent[5] << 8)) : 0;
}

static void test_own_listener(void)
{
	static dl_parser_t p;
	static uint8_t reply[DL_HDR + STUB_LINK_REPLY_MAX + 2u];
	const uint8_t *pl;
	uint8_t buf[DL_REQ_MAX];

	dl_parser_init(&p);
	p.reply_cap = STUB_LINK_REPLY_MAX;
	stub_load_init(&stub, region, sizeof(region), 0x30000u);
	on_air = false;

	// VERSION: the keys the flashing tools read (stubtool version_board, odrflash probe, the web flasher's probe)
	size_t n = own_request(&p, reply, DL_CMD_VERSION, NULL, 0, &pl);
	CHECK(sent[2] == (DL_CMD_VERSION | DL_REPLY_FLAG) && sent[3] == 0x42, "VERSION reply cmd/seq");
	CHECK(n > 1 && pl[0] == DL_ST_OK, "VERSION ok");
	char text[STUB_LINK_REPLY_MAX + 1];
	memcpy(text, pl + 1, n - 1);
	text[n - 1] = '\0';
	CHECK(strcmp(text, "proto=1\ngit=0123abcd\nboard=Board\ncodec=board-2501\nstub=1\n") == 0, "VERSION text %s", text);
	CHECK(n <= STUB_LINK_REPLY_MAX, "VERSION fits the reply buffer");

	// PING echoes, the longest request included
	for (unsigned i = 0; i < sizeof(buf); i++) {
		buf[i] = (uint8_t)(0xA5u ^ i);
	}
	n = own_request(&p, reply, DL_CMD_PING, buf, sizeof(buf), &pl);
	CHECK(n == 1u + sizeof(buf) && pl[0] == DL_ST_OK && memcmp(pl + 1, buf, sizeof(buf)) == 0, "PING echo %zu", n);

	// Every other debug link command: unknown, so a host learns the debug link isn't built
	static const uint8_t others[] = { 0x03, 0x05, 0x06, 0x07, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0F, 0x1C, 0x7E };
	for (unsigned i = 0; i < sizeof(others); i++) {
		n = own_request(&p, reply, others[i], NULL, 0, &pl);
		CHECK(n == 1 && pl[0] == DL_ST_UNKNOWN_CMD && sent[2] == (others[i] | DL_REPLY_FLAG),
		      "command 0x%02x unknown", others[i]);
	}

	// A bad CRC and an over-long request get the error frame, as from the debug link
	uint8_t f[DL_HDR + 2u];
	size_t k = dl_frame(f, DL_CMD_STUB, 7, 0);
	f[k - 1] ^= 0xFFu;
	sent_len = 0;
	(void)dl_parser_feed(&p, f, k, 0, own_handle, port_write, reply);
	CHECK(sent_len == DL_HDR + 4u && sent[2] == DL_CMD_ERROR && sent[6] == DL_ST_BAD_CRC && sent[7] == DL_CMD_STUB,
	      "bad CRC: error frame");
	const uint8_t too_long[] = { DL_SOF1, DL_SOF2, DL_CMD_STUB, 8, DL_REQ_MAX + 1u, 0 };
	sent_len = 0;
	(void)dl_parser_feed(&p, too_long, sizeof(too_long), 0, own_handle, port_write, reply);
	CHECK(sent_len == DL_HDR + 4u && sent[2] == DL_CMD_ERROR && sent[6] == DL_ST_TOO_LONG, "too long: error frame");

	// A byte that isn't the second start byte: not the listener's (the host link gives it to the other protocols)
	const uint8_t notours[] = { DL_SOF1, 'R' };
	sent_len = 0;
	k = dl_parser_feed(&p, notours, sizeof(notours), 0, own_handle, port_write, reply);
	CHECK(k == 1 && sent_len == 0 && !dl_parser_in_frame(&p, 0), "A5 then R: only A5 taken, nothing sent");
	const uint8_t cps[] = { 'R', 1 };
	CHECK(dl_parser_feed(&p, cps, sizeof(cps), 0, own_handle, port_write, reply) == 0, "a CPS request isn't taken");

	// A partial frame is dropped after the time-out and the next one is answered
	const uint8_t half[] = { DL_SOF1, DL_SOF2, DL_CMD_PING };
	(void)dl_parser_feed(&p, half, sizeof(half), 1000, own_handle, port_write, reply);
	CHECK(dl_parser_in_frame(&p, 1000 + DL_FRAME_TIMEOUT_MS), "partial frame held");
	CHECK(!dl_parser_in_frame(&p, 1001 + DL_FRAME_TIMEOUT_MS), "partial frame dropped after the time-out");
	n = own_request(&p, reply, DL_CMD_PING, NULL, 0, &pl);
	CHECK(n == 1 && pl[0] == DL_ST_OK, "answered after the time-out");

	// A build whose platform has no stub entry: STUB unavailable
	const stub_link_t none = { .board = "X", .git = "g", .codec = NULL, .stub = NULL };
	uint8_t out[STUB_LINK_REPLY_MAX];
	uint8_t info = STUB_OP_INFO;
	CHECK(stub_link_handle(&none, DL_CMD_STUB, &info, 1, out, sizeof(out)) == 1 && out[0] == DL_ST_UNAVAILABLE,
	      "no stub: unavailable");
	n = stub_link_handle(&none, DL_CMD_VERSION, NULL, 0, out, sizeof(out));
	CHECK(n > 1 && memcmp(out + 1, "proto=1\ngit=g\nboard=X\ncodec=none\n", 33) == 0, "no codec: codec=none");

	// A reply buffer too short for INFO is never written past
	CHECK(stub_load_request(&stub, &info, 1, out, STUB_REPLY_MAX - 1u, &(bool){ false }, &(uint32_t){ 0 }) == 1 &&
		      out[0] == DL_ST_UNAVAILABLE,
	      "short buffer: unavailable");
}

int main(void)
{
	run_golden("debug link", debuglink_handle, DL_REPLY_MAX);
	run_golden("stub entry", own_handle, STUB_LINK_REPLY_MAX);
	test_own_listener();
	printf("stubLinkTest: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
