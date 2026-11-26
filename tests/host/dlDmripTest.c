// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The debug link over the air, its pure parts (core/debuglink/dl_dmrip.c, docs/spec/debuglink.md 7): SipHash, the
// datagram and its addressing, the replay check, what the air may ask, a detour's items and its runner (the deadline
// always fires, a longer one is refused), and GET_VIEW's page (dl_fmt.c)
#include <stdio.h>
#include <string.h>
#include "debuglink/dl_dmrip.h"
#include "debuglink/debuglink.h"
#include "debuglink/dl_fmt.h"

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

static const uint8_t K[DL_DMRIP_KEY] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };

// --- SipHash-2-4: the reference vectors (key 00..0f, message 00..n-1) ---

static void test_siphash(void)
{
	uint8_t m[64];
	for (unsigned i = 0; i < sizeof(m); i++) {
		m[i] = (uint8_t)i;
	}
	CHECK(dl_siphash(K, m, 0) == 0x726fdb47dd0e0e31ull, "empty");
	CHECK(dl_siphash(K, m, 1) == 0x74f839c593dc67fdull, "1 byte");
	CHECK(dl_siphash(K, m, 7) == 0xab0200f58b01d137ull, "7 bytes");
	CHECK(dl_siphash(K, m, 8) == 0x93f5f5799a932462ull, "8 bytes");
	CHECK(dl_siphash(K, m, 15) == 0xa129ca6149be45e5ull, "15 bytes");
	CHECK(dl_siphash(K, m, 63) == 0x958a324ceb064572ull, "63 bytes");

	uint8_t a[DL_DMRIP_KEY], b[DL_DMRIP_KEY];
	const uint8_t uid1[12] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };
	uint8_t uid2[12];
	memcpy(uid2, uid1, 12);
	uid2[11] ^= 1;
	dl_dmrip_kdf(uid1, 12, a);
	dl_dmrip_kdf(uid1, 12, b);
	CHECK(memcmp(a, b, sizeof(a)) == 0, "kdf: the same UID, the same key");
	dl_dmrip_kdf(uid2, 12, b);
	CHECK(memcmp(a, b, sizeof(a)) != 0, "kdf: another UID, another key");
	CHECK(memcmp(a, a + 8, 8) != 0, "kdf: the halves differ");
}

// --- a request datagram as the host builds it ---

static size_t frame(uint8_t *b, uint8_t cmd, uint8_t seq, const uint8_t *p, uint16_t len)
{
	if (len > 0) {
		memcpy(b + DL_HDR, p, len);	// (p may be NULL with nothing to copy)
	}
	return dl_frame(b, cmd, seq, len);
}

static size_t request(uint8_t *d, const uint8_t *key, uint8_t flags, uint32_t id, uint32_t nonce, uint32_t counter,
		      const uint8_t *body, uint16_t n)
{
	memcpy(d + DL_DMRIP_HDR, body, n);
	dl_dmrip_msg_t m = { flags, id, nonce, counter, 0, NULL, n };
	return dl_dmrip_seal(key, &m, d);
}

static void test_datagram(void)
{
	uint8_t f[DL_HDR + DL_REQ_MAX + 2], d[DL_DMRIP_DGRAM_MAX + 8];
	const uint8_t ping[3] = { 'a', 'b', 'c' };
	size_t fl = frame(f, DL_CMD_PING, 7, ping, sizeof(ping));
	size_t n = request(d, K, 0, 3101276, 0x1234, 5, f, (uint16_t)fl);
	CHECK(n == DL_DMRIP_HDR + fl + DL_DMRIP_TAG, "length %zu", n);
	CHECK(d[0] == DL_DMRIP_VERSION && d[2] == (3101276 & 0xFF) && d[4] == (3101276 >> 16), "header");

	dl_dmrip_msg_t m;
	CHECK(dl_dmrip_open(K, d, n, &m), "opens");
	CHECK(m.id == 3101276 && m.nonce == 0x1234 && m.counter == 5 && m.body_len == fl, "fields");
	uint8_t cmd, seq;
	const uint8_t *p;
	uint16_t len;
	CHECK(dl_dmrip_frame(m.body, m.body_len, &cmd, &seq, &p, &len), "frame");
	CHECK(cmd == DL_CMD_PING && seq == 7 && len == 3 && memcmp(p, "abc", 3) == 0, "frame fields");

	// Every byte is under the tag
	for (size_t i = 0; i < n; i++) {
		d[i] ^= 0x01;
		CHECK(!dl_dmrip_open(K, d, n, &m), "byte %zu flipped", i);
		d[i] ^= 0x01;
	}
	uint8_t k2[DL_DMRIP_KEY];
	memcpy(k2, K, sizeof(k2));
	k2[0] ^= 0x80;
	CHECK(!dl_dmrip_open(k2, d, n, &m), "another key");
	CHECK(!dl_dmrip_open(K, d, n - 1, &m), "short");
	CHECK(!dl_dmrip_open(K, d, DL_DMRIP_HDR + DL_DMRIP_TAG - 1, &m), "under the header");
	CHECK(!dl_dmrip_open(K, d, DL_DMRIP_DGRAM_MAX + 1, &m), "over the longest");

	// A body that is not a whole frame
	CHECK(!dl_dmrip_frame(f, fl - 1, &cmd, &seq, &p, &len), "frame cut");
	f[DL_HDR] ^= 1;
	CHECK(!dl_dmrip_frame(f, fl, &cmd, &seq, &p, &len), "frame CRC");
	f[DL_HDR] ^= 1;
	CHECK(!dl_dmrip_frame(f, fl + 1, &cmd, &seq, &p, &len), "frame with a byte after");
	uint8_t big[DL_HDR + DL_REQ_MAX + 3] = { 0 };
	size_t bl = dl_frame(big, DL_CMD_PING, 0, DL_REQ_MAX + 1);
	CHECK(!dl_dmrip_frame(big, bl, &cmd, &seq, &p, &len), "frame over DL_REQ_MAX");
}

// Addressing: one radio answers, by its own ID; never a group, the all-call or another radio
static void test_addressing(void)
{
	uint8_t f[DL_HDR + 2], d[64];
	size_t fl = frame(f, DL_CMD_PING, 0, NULL, 0);
	dl_dmrip_msg_t m;
	size_t n = request(d, K, 0, 3101276, 1, 1, f, (uint16_t)fl);
	CHECK(dl_dmrip_check(K, d, n, 3101276, &m) == DL_DMRIP_ACCEPT, "ours");
	CHECK(dl_dmrip_check(K, d, n, 3101277, &m) == DL_DMRIP_NOT_OURS, "another radio's ID inside");
	CHECK(dl_dmrip_check(K, d, n, 0, &m) == DL_DMRIP_NOT_OURS, "no ID of our own");
	n = request(d, K, 0, 0xFFFFFF, 1, 1, f, (uint16_t)fl);
	CHECK(dl_dmrip_check(K, d, n, 0xFFFFFF, &m) == DL_DMRIP_NOT_OURS, "all-call ID");
	n = request(d, K, DL_DMRIP_F_REPLY, 3101276, 1, 1, f, (uint16_t)fl);
	CHECK(dl_dmrip_check(K, d, n, 3101276, &m) == DL_DMRIP_BAD, "a reply is no request");
	n = request(d, K, 0, 3101276, 1, 1, f, (uint16_t)fl);
	d[n - 1] ^= 1;
	CHECK(dl_dmrip_check(K, d, n, 3101276, &m) == DL_DMRIP_BAD, "tag");
}

// The answer's address: the request's source, the way back it came; one no answer can reach gives 12.<ID>
static void test_reply_ip(void)
{
	static const uint32_t ok[] = { 0x0C000000u | 9991234u, 0x0D000000u | 9991234u, 0xC0A80105u };
	for (unsigned k = 0; k < 3; k++) {
		CHECK(dl_dmrip_reply_ip(9991234u, ok[k]) == ok[k], "reply to %08x", (unsigned)ok[k]);
	}
	static const uint32_t unusable[] = { 0, 0xE1000009u, 0xFFFFFFFFu, 0x7F000001u };
	for (unsigned k = 0; k < 4; k++) {
		CHECK(dl_dmrip_reply_ip(9991234u, unusable[k]) == (0x0C000000u | 9991234u), "unusable %08x",
		      (unsigned)unusable[k]);
	}
}

static void test_replay(void)
{
	dl_dmrip_replay_t r = { 0, 0, 0 };
	CHECK(dl_dmrip_replay(&r, 0, 1, 5) == DL_DMRIP_OLD, "no session");
	r.nonce = 77;
	CHECK(dl_dmrip_replay(&r, 76, 1, 5) == DL_DMRIP_OLD, "another session");
	CHECK(dl_dmrip_replay(&r, 77, 1, 5) == DL_DMRIP_NEW && r.last == 1, "new");
	CHECK(dl_dmrip_replay(&r, 77, 1, 5) == DL_DMRIP_REPEAT, "repeat");
	// A second host on the same counter with another request: that request never ran, and the first one's reply
	// isn't the second's
	CHECK(dl_dmrip_replay(&r, 77, 1, 6) == DL_DMRIP_OLD, "another request on the last counter");
	CHECK(dl_dmrip_replay(&r, 77, 1, 5) == DL_DMRIP_REPEAT, "the first one's repeat still");
	CHECK(dl_dmrip_replay(&r, 77, 0, 5) == DL_DMRIP_OLD, "older");
	CHECK(dl_dmrip_replay(&r, 77, 10, 7) == DL_DMRIP_NEW && r.last == 10, "a gap is fine");
	CHECK(dl_dmrip_replay(&r, 77, 10, 7) == DL_DMRIP_REPEAT, "the new request kept");
	CHECK(dl_dmrip_replay(&r, 77, 5, 7) == DL_DMRIP_OLD && r.last == 10, "behind");
	r.last = 0xFFFFFFFFu;
	CHECK(dl_dmrip_replay(&r, 77, 2, 8) == DL_DMRIP_NEW, "wraps");
}

static void test_air_policy(void)
{
	static const uint8_t allowed[] = { DL_CMD_PING, DL_CMD_VERSION, DL_CMD_GET_DIAG, DL_CMD_RESET_COUNTERS,
					   DL_CMD_SCREENSHOT, DL_CMD_GET_LOG, DL_CMD_GET_VIEW, DL_CMD_IDDB,
					   DL_CMD_DETOUR };
	const uint8_t z[2] = { 0, 0 };
	for (unsigned i = 0; i < sizeof(allowed); i++) {
		CHECK(dl_dmrip_air_allowed(allowed[i], z, 1), "0x%02x allowed", allowed[i]);
	}
	// Link-cutting and stored changes: never alone over the air. The chip UID (the default key) and memory (the
	// codec on a codec-merged image): never over the air
	static const uint8_t refused[] = { DL_CMD_INFO_CHIP, DL_CMD_READ_MEM, DL_CMD_RADIO, DL_CMD_PTT, DL_CMD_KEY,
					   DL_CMD_CODEPLUG, DL_CMD_CPKV, DL_CMD_T3,
					   DL_CMD_STORAGE, DL_CMD_HWOPT, DL_CMD_REBOOT, DL_CMD_STUB, DL_CMD_CRASH,
					   DL_CMD_WATCHDOG, DL_CMD_RX2, DL_CMD_DMR_DATA, DL_CMD_APRS, DL_CMD_SNMP,
					   DL_CMD_SPLIT, DL_CMD_AIR, DL_CMD_SUPPORT, DL_CMD_SATELLITE };
	for (unsigned i = 0; i < sizeof(refused); i++) {
		CHECK(!dl_dmrip_air_allowed(refused[i], z, 1), "0x%02x refused", refused[i]);
	}
	for (unsigned c = DL_CMD_DMRIP + 1u; c < 0x80; c++) {
		CHECK(!dl_dmrip_air_allowed((uint8_t)c, z, 1), "unknown 0x%02x refused", c);
	}
	const uint8_t st = DL_DMRIP_OP_STATUS, key = DL_DMRIP_OP_KEY;
	CHECK(dl_dmrip_air_allowed(DL_CMD_DMRIP, &st, 1), "DMRIP status");
	CHECK(!dl_dmrip_air_allowed(DL_CMD_DMRIP, &key, 1), "DMRIP key: the cable only");
	const uint8_t beep[2] = { DL_OP_BEEP, 1 };
	CHECK(dl_dmrip_air_allowed(DL_CMD_RADIO, beep, 2), "RADIO BEEP: a sound, taken directly");
	static const uint8_t direct_ops[] = { 7, 8, 10, 11, 13, 14, 15, 16, 17, 18, DL_OP_BEEP, DL_OP_VIBRATE, DL_OP_VOLUME,
					      DL_OP_VOICE_CAPTURE };
	for (unsigned i = 0; i < sizeof direct_ops; i++) {
		uint8_t p[2] = { direct_ops[i], 0 };
		CHECK(dl_dmrip_air_allowed(DL_CMD_RADIO, p, 2), "RADIO op %u directly", direct_ops[i]);
	}
	// The ones that move the link (a detour's), move it by themselves, transmit, stop the replies, store settings, or
	// change later transmissions until a restart (the voice test signal)
	static const uint8_t detour_ops[] = { 1, 2, 3, 4, 5, 6, 9, 12, 19, 25, 27, 28, DL_OP_SCAN, DL_OP_SCAN_VFO,
					      DL_OP_VOICE_TEST,
					      DL_OP_TONE_BURST, DL_OP_ROAM, DL_OP_DMR_FIELDS, DL_OP_VFO_COPY, DL_OP_DTMF, DL_OP_TX_DRIVE,
					      DL_OP_PA_KEYUP, DL_OP_VFO_SFR, DL_OP_PA_SWEEP, DL_OP_PA_KEYRUN, DL_OP_P25,
					      DL_OP_NXDN };
	for (unsigned i = 0; i < sizeof detour_ops; i++) {
		uint8_t p[2] = { detour_ops[i], 0 };
		CHECK(!dl_dmrip_air_allowed(DL_CMD_RADIO, p, 2), "RADIO op %u not directly", detour_ops[i]);
	}
	// Registers: reads directly, never the SoC's; RF chip writes and the audio path's register writes only as items
	const uint8_t rd[3] = { 0, 0, 0x41 }, wr[7] = { 0, 0, 0x41, 1, 0, 0, 0 }, soc_rd[5] = { DL_CHIP_SOC, 0, 0x3C, 2, 0x40 };
	const uint8_t soc_wr[9] = { DL_CHIP_SOC, 0x10, 0x3C, 2, 0x40, 0, 0, 0, 0 };
	CHECK(dl_dmrip_air_allowed(DL_CMD_CHIP_REG, rd, 3), "CHIP_REG read directly");
	CHECK(!dl_dmrip_air_allowed(DL_CMD_CHIP_REG, wr, 7), "CHIP_REG write: a detour's");
	CHECK(!dl_dmrip_air_allowed(DL_CMD_CHIP_REG, soc_rd, 5), "SoC register read: never");
	CHECK(!dl_dmrip_air_allowed(DL_CMD_CHIP_REG, soc_wr, 9), "SoC register write: never");
	CHECK(!dl_dmrip_air_allowed(DL_CMD_CHIP_REG, z, 0), "CHIP_REG empty");
	for (unsigned op = 0; op < 16; op++) {
		uint8_t p[4] = { (uint8_t)op, 0, 0, 0 };
		bool want = op == DL_PROBE_OP_STATUS || op == DL_PROBE_OP_REG_READ;
		CHECK(dl_dmrip_air_allowed(DL_CMD_AUDIO_PROBE, p, sizeof(p)) == want, "AUDIO_PROBE op %u directly", op);
		want = want || op == DL_PROBE_OP_REG_WRITE;
		CHECK(dl_detour_item_allowed(DL_CMD_AUDIO_PROBE, p, sizeof(p)) == want, "AUDIO_PROBE op %u as an item", op);
	}
	CHECK(dl_detour_item_allowed(DL_CMD_CHIP_REG, rd, 3), "CHIP_REG read as an item");
	CHECK(dl_detour_item_allowed(DL_CMD_CHIP_REG, wr, 7), "CHIP_REG write as an item");
	CHECK(!dl_detour_item_allowed(DL_CMD_CHIP_REG, soc_rd, 5), "SoC register read: not as an item");
	CHECK(!dl_detour_item_allowed(DL_CMD_CHIP_REG, soc_wr, 9), "SoC register write: not as an item");
	const uint8_t vol[2] = { DL_OP_VOLUME, 8 }, set_beep[4] = { DL_OP_SETTING, 11, 50, 0 },
		      set_inhibit[4] = { DL_OP_SETTING, 5, 1, 0 }, txlim[2] = { DL_OP_TX_LIMIT, 0 };
	CHECK(dl_dmrip_air_allowed(DL_CMD_RADIO, vol, 2), "RADIO VOLUME directly");
	CHECK(!dl_dmrip_air_allowed(DL_CMD_RADIO, set_beep, 4), "SETTING beep level: stored, a detour's");
	CHECK(!dl_dmrip_air_allowed(DL_CMD_RADIO, set_inhibit, 4), "SETTING TX inhibit: would stop the replies");
	for (unsigned id = 0; id < 40; id++) {
		uint8_t p[4] = { DL_OP_SETTING, (uint8_t)id, 0, 0 };
		// Zone share offers only: the mic gain, encode options and trim change later transmissions until a restart
		bool want = id == DL_SETTING_ZS_OFFERS;
		CHECK(dl_dmrip_air_allowed(DL_CMD_RADIO, p, sizeof(p)) == want, "SETTING %u directly", id);
	}
	CHECK(!dl_dmrip_air_allowed(DL_CMD_RADIO, txlim, 2), "TX limit: would stop the replies");
	CHECK(!dl_dmrip_air_allowed(DL_CMD_RADIO, beep, 0), "RADIO without an op");
	CHECK(!dl_dmrip_air_allowed(DL_CMD_DMRIP, &st, 0), "DMRIP empty");
}

// --- a detour's items ---

static uint16_t item(uint8_t *at, uint8_t cmd, const uint8_t *p, uint8_t len)
{
	at[0] = cmd;
	at[1] = len;
	memcpy(at + 2, p, len);
	return (uint16_t)(2u + len);
}

static void test_items(void)
{
	// What the return undoes (7.4)
	static const uint8_t ok_ops[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 15, 16, 17, 18, DL_OP_DMR_FIELDS,
					  DL_OP_VFO_COPY, DL_OP_P25, DL_OP_NXDN, DL_OP_BEEP };
	for (unsigned i = 0; i < sizeof(ok_ops); i++) {
		uint8_t p[4] = { ok_ops[i], 0, 0, 0 };
		CHECK(dl_detour_item_allowed(DL_CMD_RADIO, p, sizeof(p)), "RADIO op %u", ok_ops[i]);
	}
	// What it cannot undo, or what hands the radio over: refused
	static const uint8_t bad_ops[] = { 0, 13, 14, 19, 20, 21, 23, 25, 26, 27, 28, 29, 30, 33, 34, 35, 36, 37, 38,
					   39, 40, 41, 44, 255 };
	for (unsigned i = 0; i < sizeof(bad_ops); i++) {
		uint8_t p[4] = { bad_ops[i], 0, 0, 0 };
		CHECK(!dl_detour_item_allowed(DL_CMD_RADIO, p, sizeof(p)), "RADIO op %u refused", bad_ops[i]);
	}
	for (unsigned id = 0; id < 40; id++) {
		uint8_t p[4] = { DL_OP_SETTING, (uint8_t)id, 0, 0 };
		bool want = (id >= 1 && id <= 28) || id == 30 || id == 33 || id == 34;
		CHECK(dl_detour_item_allowed(DL_CMD_RADIO, p, sizeof(p)) == want, "SETTING %u", id);
	}
	const uint8_t w[2] = { 100, 0 }, ptt[1] = { 1 };
	CHECK(dl_detour_item_allowed(DL_DETOUR_ITEM_WAIT, w, 2), "wait");
	CHECK(!dl_detour_item_allowed(DL_DETOUR_ITEM_WAIT, w, 1), "wait short");
	CHECK(dl_detour_item_allowed(DL_CMD_PTT, ptt, 1), "PTT");
	CHECK(!dl_detour_item_allowed(DL_CMD_CHIP_REG, ptt, 0), "register request empty");
	CHECK(!dl_detour_item_allowed(DL_CMD_READ_MEM, ptt, 1), "no reads as items");
	CHECK(!dl_detour_item_allowed(DL_CMD_DETOUR, ptt, 1), "no detour in a detour");

	uint8_t b[32];
	uint16_t n = 0;
	const uint8_t vfo[5] = { 5, 0x08, 0x2B, 0x7E, 0x1B };	// VFO_SET
	n += item(b + n, DL_CMD_RADIO, vfo, sizeof(vfo));
	n += item(b + n, DL_DETOUR_ITEM_WAIT, w, 2);
	CHECK(dl_detour_items_check(b, n) == DL_ST_OK, "items");
	CHECK(dl_detour_items_check(b, (uint16_t)(n - 1)) == DL_ST_BAD_ARG, "cut");
	CHECK(dl_detour_items_check(b, 1) == DL_ST_BAD_ARG, "one byte");
	const uint8_t reg[3] = { 0, 0, 0 };
	n += item(b + n, DL_CMD_READ_MEM, reg, 3);
	CHECK(dl_detour_items_check(b, n) == DL_ST_REFUSED, "one bad item refuses the whole run");
	CHECK(dl_detour_items_check(b, 0) == DL_ST_OK, "none");
}

// --- the runner, against a fake radio core ---

static struct {
	bool armed;		// the core holds a home state
	bool refuse;		// mark refused (settings waiting, hotspot)
	bool lose_ret;		// the next return request is lost (a full queue)
	bool clear;
	int marks, rets, releases, dispatched;
	uint32_t backstop;
	uint8_t answer;		// what the next dispatch answers
	int busy_for;		// dispatches answered BUSY first
	uint8_t last_cmd, last_op;
} core;

static uint8_t f_dispatch(uint8_t cmd, const uint8_t *p, uint8_t len)
{
	core.dispatched++;
	core.last_cmd = cmd;
	core.last_op = len ? p[0] : 0;
	if (core.busy_for > 0) {
		core.busy_for--;
		return DL_ST_BUSY;
	}
	return core.answer;
}

static void f_mark(uint32_t backstop_ms)
{
	core.marks++;
	core.backstop = backstop_ms;
	if (!core.refuse) {
		core.armed = true;
	}
}

static void f_ret(void)
{
	core.rets++;
	if (core.lose_ret) {
		core.lose_ret = false;
		return;
	}
	core.armed = false;
}

static bool f_armed(void)
{
	return core.armed;
}

static void f_release(void)
{
	core.releases++;
}

static bool f_clear(void)
{
	return core.clear;
}

static const dl_detour_ops_t OPS = { f_dispatch, f_mark, f_ret, f_armed, f_release, f_clear };

static void reset_core(void)
{
	memset(&core, 0, sizeof(core));
	core.answer = DL_ST_OK;
	core.clear = true;
}

static uint8_t run_req(dl_detour_t *d, uint16_t s, const uint8_t *items, uint16_t n, bool air, uint32_t now)
{
	uint8_t q[DL_REQ_MAX], o[32];
	q[0] = DL_DETOUR_RUN;
	q[1] = (uint8_t)s;
	q[2] = (uint8_t)(s >> 8);
	memcpy(q + 3, items, n);
	size_t r = dl_detour_request(d, &OPS, q, (uint16_t)(3u + n), air, o, sizeof(o), now);
	CHECK(r >= 1, "reply");
	return o[0];
}

static uint8_t op_req(dl_detour_t *d, uint8_t op, uint16_t s, uint32_t now)
{
	uint8_t q[3] = { op, (uint8_t)s, (uint8_t)(s >> 8) }, o[32];
	dl_detour_request(d, &OPS, q, 3, false, o, sizeof(o), now);
	return o[0];
}

// Poll every 5 ms from *now to until
static void run_until(dl_detour_t *d, uint32_t *now, uint32_t until)
{
	while ((int32_t)(*now - until) < 0) {
		*now += 5;
		dl_detour_poll(d, &OPS, *now);
	}
}

static void test_runner(void)
{
	dl_detour_t d;
	uint32_t now = 1000;
	uint8_t b[DL_REQ_MAX];
	uint16_t n = 0;
	const uint8_t vfo[5] = { 5, 0x08, 0x2B, 0x7E, 0x1B }, ptt[1] = { 1 }, w[2] = { 0xF4, 0x01 };	// 500 ms
	n += item(b + n, DL_CMD_RADIO, vfo, sizeof(vfo));
	n += item(b + n, DL_DETOUR_ITEM_WAIT, w, 2);
	n += item(b + n, DL_CMD_PTT, ptt, 1);

	// The deadline always fires: items run, then the return at the deadline
	reset_core();
	dl_detour_init(&d);
	CHECK(run_req(&d, 10, b, n, false, now) == DL_ST_OK, "run");
	CHECK(core.marks == 1 && core.backstop == 10000u + DL_DETOUR_BACKSTOP_MS, "mark with the backstop");
	CHECK(d.phase == DL_DETOUR_ARMING, "arming");
	run_until(&d, &now, 1010);
	CHECK(d.phase == DL_DETOUR_RUNNING, "running");
	run_until(&d, &now, 1100);
	CHECK(core.dispatched == 1 && core.last_cmd == DL_CMD_RADIO, "first item");
	run_until(&d, &now, 1700);
	CHECK(core.dispatched == 2 && core.last_cmd == DL_CMD_PTT && d.index == 3, "after the wait, PTT");
	run_until(&d, &now, 10990);
	CHECK(core.rets == 0 && core.armed, "still away before the deadline");
	run_until(&d, &now, 11010);
	CHECK(core.rets == 1 && core.releases >= 1 && !core.armed, "back at the deadline, PTT released");
	run_until(&d, &now, 11100);
	CHECK(d.phase == DL_DETOUR_IDLE && d.result == DL_ST_OK && !d.asked, "idle after");

	// Longer than the most: refused, nothing marked
	reset_core();
	dl_detour_init(&d);
	CHECK(run_req(&d, DL_DETOUR_MAX_S + 1, b, n, false, now) == DL_ST_BAD_ARG, "over the most refused");
	CHECK(run_req(&d, 0, b, n, false, now) == DL_ST_BAD_ARG, "no deadline refused");
	CHECK(core.marks == 0 && d.phase == DL_DETOUR_IDLE, "nothing marked");
	uint8_t bad[8];
	const uint8_t reg[3] = { 0, 0, 0 };
	uint16_t bn = item(bad, DL_CMD_READ_MEM, reg, 3);
	CHECK(run_req(&d, 10, bad, bn, false, now) == DL_ST_REFUSED && core.marks == 0, "a memory read refused");
	CHECK(op_req(&d, DL_DETOUR_EXTEND, 10, now) == DL_ST_UNAVAILABLE, "extend with none");

	// Extend: capped the same way; return now
	reset_core();
	dl_detour_init(&d);
	now = 50000;
	run_req(&d, 5, b, 0, false, now);
	run_until(&d, &now, now + 20);
	CHECK(op_req(&d, DL_DETOUR_EXTEND, DL_DETOUR_MAX_S + 1, now) == DL_ST_BAD_ARG, "extend over the most");
	CHECK(op_req(&d, DL_DETOUR_EXTEND, 60, now) == DL_ST_OK && core.marks == 2, "extend");
	CHECK(d.deadline_ms == now + 60000u && core.backstop == 60000u + DL_DETOUR_BACKSTOP_MS, "new deadline");
	run_until(&d, &now, now + 6000);
	CHECK(core.rets == 0, "the old deadline passed: still away");
	CHECK(op_req(&d, DL_DETOUR_RETURN, 0, now) == DL_ST_OK && core.rets == 1 && d.asked, "return now");
	run_until(&d, &now, now + 20);
	CHECK(d.phase == DL_DETOUR_IDLE, "idle");
	CHECK(op_req(&d, DL_DETOUR_RETURN, 0, now) == DL_ST_OK && core.rets == 1, "return with none: nothing");

	// A run while one runs: new items, the deadline from now, no second home state
	reset_core();
	dl_detour_init(&d);
	now = 90000;
	run_req(&d, 5, b, 0, false, now);
	run_until(&d, &now, now + 3000);
	CHECK(run_req(&d, 5, b, n, false, now) == DL_ST_OK && core.marks == 2 && d.phase == DL_DETOUR_RUNNING,
	      "run again: same detour");
	uint32_t again = now;
	run_until(&d, &now, again + 4900);
	CHECK(core.rets == 0, "the first deadline moved");
	run_until(&d, &now, again + 5100);
	CHECK(core.rets == 1, "the second deadline fires");

	// BUSY is tried again for a while, then fails the run, which returns at once
	reset_core();
	dl_detour_init(&d);
	now = 200000;
	core.busy_for = 3;
	run_req(&d, 30, b, n, false, now);
	run_until(&d, &now, now + 100);
	CHECK(core.dispatched >= 4 && d.index >= 1 && d.failed == 0, "BUSY retried: %d %u %u %u", core.dispatched, d.index,
	      d.failed, d.phase);
	core.busy_for = 1000000;
	run_until(&d, &now, now + 3000);
	CHECK(d.failed == 3 && d.result == DL_ST_BUSY && core.rets == 1, "BUSY too long: item 3 failed, back");
	core.busy_for = 0;
	core.answer = DL_ST_BAD_ARG;
	run_until(&d, &now, now + 100);
	run_req(&d, 30, b, n, false, now);
	run_until(&d, &now, now + 100);
	CHECK(d.failed == 1 && d.result == DL_ST_BAD_ARG && core.rets == 2 && d.phase != DL_DETOUR_RUNNING,
	      "a failed item: back at once");

	// Arming refused (settings waiting, hotspot): the run ends BUSY, nothing runs
	reset_core();
	dl_detour_init(&d);
	now = 300000;
	core.refuse = true;
	run_req(&d, 30, b, n, false, now);
	run_until(&d, &now, now + DL_DETOUR_ARM_MS + 20);
	CHECK(d.phase == DL_DETOUR_IDLE && d.result == DL_ST_BUSY && core.dispatched == 0, "not armed: BUSY");

	// A lost return request is posted again until the core is back
	reset_core();
	dl_detour_init(&d);
	now = 400000;
	run_req(&d, 2, b, 0, false, now);
	core.lose_ret = true;
	run_until(&d, &now, now + 2100);
	CHECK(core.rets == 1 && core.armed && d.phase == DL_DETOUR_RETURNING, "first return lost");
	run_until(&d, &now, now + DL_DETOUR_REPOST_MS + 20);
	CHECK(core.rets == 2 && !core.armed, "posted again");
	run_until(&d, &now, now + 20);
	CHECK(d.phase == DL_DETOUR_IDLE, "idle");

	// The core's backstop ended it: the runner follows, PTT up
	reset_core();
	dl_detour_init(&d);
	now = 500000;
	run_req(&d, 30, b, 0, false, now);
	run_until(&d, &now, now + 50);
	core.armed = false;
	int rel = core.releases;
	run_until(&d, &now, now + 20);
	CHECK(d.phase == DL_DETOUR_IDLE && core.releases > rel, "the core's backstop: idle");

	// Over the air: the first item waits for the RUN reply to leave, at most DL_DETOUR_CLEAR_MS
	reset_core();
	dl_detour_init(&d);
	now = 600000;
	core.clear = false;
	run_req(&d, 30, b, n, true, now);
	run_until(&d, &now, now + 1000);
	CHECK(d.phase == DL_DETOUR_CLEAR && core.dispatched == 0, "waits for the reply");
	core.clear = true;
	run_until(&d, &now, now + 20);
	CHECK(d.phase == DL_DETOUR_RUNNING && core.dispatched == 1, "then runs");
	core.clear = false;
	CHECK(run_req(&d, 30, b, n, true, now) == DL_ST_OK && d.phase == DL_DETOUR_CLEAR, "a second run waits too");
	run_until(&d, &now, now + DL_DETOUR_CLEAR_MS + 20);
	CHECK(d.phase == DL_DETOUR_RUNNING, "not past DL_DETOUR_CLEAR_MS");
	op_req(&d, DL_DETOUR_RETURN, 0, now);
	run_until(&d, &now, now + 20);

	// A deadline every way it can end: the core is back each time
	for (uint16_t s = 1; s <= 3; s++) {
		reset_core();
		dl_detour_init(&d);
		now = 700000u * s;
		run_req(&d, s, b, n, (s & 1) != 0, now);
		run_until(&d, &now, now + s * 1000u + 600);
		CHECK(!core.armed && core.rets >= 1 && d.phase == DL_DETOUR_IDLE, "%u s: back", s);
	}

	// Status
	reset_core();
	dl_detour_init(&d);
	now = 900000;
	run_req(&d, 20, b, n, false, now);
	run_until(&d, &now, now + 100);
	uint8_t q = DL_DETOUR_STATUS, o[32];
	size_t r = dl_detour_request(&d, &OPS, &q, 1, false, o, sizeof(o), now);
	uint32_t left = (uint32_t)o[2] | ((uint32_t)o[3] << 8) | ((uint32_t)o[4] << 16) | ((uint32_t)o[5] << 24);
	CHECK(r == 13 && o[0] == DL_ST_OK && o[1] == DL_DETOUR_RUNNING && left > 19800 && left <= 19900, "status %u",
	      (unsigned)left);
}

// --- GET_VIEW's page ---

static void test_view_page(void)
{
	static const char text[] = "section=x\na.one=1\na.two=22\nb.three=333\na.four=4444\n";
	char t[128];
	size_t n;

	memcpy(t, text, sizeof(text));
	n = dl_view_page(t, sizeof(text) - 1, "", 0, 0, 1000);
	CHECK(n == sizeof(text) - 1 && memcmp(t, text, n) == 0, "everything");

	memcpy(t, text, sizeof(text));
	n = dl_view_page(t, sizeof(text) - 1, "a.", 2, 0, 1000);
	CHECK(n == 39 && memcmp(t, "section=x\na.one=1\na.two=22\na.four=4444\n", n) == 0, "prefix: %.*s", (int)n, t);

	// A page cut by its limit: whole lines, then more= with the next offset
	memcpy(t, text, sizeof(text));
	n = dl_view_page(t, sizeof(text) - 1, "", 0, 0, 10 + 8 + 9 + DL_VIEW_MORE_MAX);
	CHECK(n == 10 + 8 + 9 + 8 && memcmp(t, "section=x\na.one=1\na.two=22\nmore=17\n", n) == 0, "page 1: %.*s",
	      (int)n, t);
	memcpy(t, text, sizeof(text));
	n = dl_view_page(t, sizeof(text) - 1, "", 0, 17, 1000);
	CHECK(memcmp(t, "section=x\nb.three=333\na.four=4444\n", n) == 0, "page 2: %.*s", (int)n, t);
	memcpy(t, text, sizeof(text));
	n = dl_view_page(t, sizeof(text) - 1, "a.", 2, 8, 1000);
	CHECK(memcmp(t, "section=x\na.two=22\na.four=4444\n", n) == 0, "prefix, offset: %.*s", (int)n, t);
	memcpy(t, text, sizeof(text));
	n = dl_view_page(t, sizeof(text) - 1, "zz", 2, 0, 1000);
	CHECK(n == 10, "no match: the section line");

	// The reply buffer cut the section: truncated=1 stays on the last page, whatever the prefix
	static const char cut[] = "section=x\na.one=1\nb.two=2\ntruncated=1\n";
	memcpy(t, cut, sizeof(cut));
	n = dl_view_page(t, sizeof(cut) - 1, "a.", 2, 0, 1000);
	CHECK(memcmp(t, "section=x\na.one=1\ntruncated=1\n", n) == 0, "truncated kept: %.*s", (int)n, t);

	// A line longer than the page goes whole on a page of its own, so the next offset moves on
	static const char lng[] = "section=x\na.long=0123456789012345678901234567890123456789\na.next=1\nb.x=2\nb.y=3\n";
	memcpy(t, lng, sizeof(lng));
	n = dl_view_page(t, sizeof(lng) - 1, "a.", 2, 0, 10 + 20 + DL_VIEW_MORE_MAX);
	CHECK(memcmp(t, "section=x\na.long=0123456789012345678901234567890123456789\nmore=48\n", n) == 0, "long: %.*s",
	      (int)n, t);
	memcpy(t, lng, sizeof(lng));
	n = dl_view_page(t, sizeof(lng) - 1, "a.", 2, 48, 10 + 20 + DL_VIEW_MORE_MAX);
	CHECK(memcmp(t, "section=x\na.next=1\n", n) == 0, "after the long line: %.*s", (int)n, t);

	// The same on a later page: lines skipped below the offset don't count as sent
	static const char lng2[] = "section=x\na.s=1\na.long=0123456789012345678901234567890123456789\nb.x=2\nb.y=3\n";
	memcpy(t, lng2, sizeof(lng2));
	n = dl_view_page(t, sizeof(lng2) - 1, "a.", 2, 0, 10 + 20 + DL_VIEW_MORE_MAX);
	CHECK(memcmp(t, "section=x\na.s=1\nmore=6\n", n) == 0, "page 1 before the long line: %.*s", (int)n, t);
	memcpy(t, lng2, sizeof(lng2));
	n = dl_view_page(t, sizeof(lng2) - 1, "a.", 2, 6, 10 + 20 + DL_VIEW_MORE_MAX);
	CHECK(memcmp(t, "section=x\na.long=0123456789012345678901234567890123456789\n", n) == 0,
	      "the long line whole on page 2: %.*s", (int)n, t);
}

int main(void)
{
	test_siphash();
	test_datagram();
	test_addressing();
	test_reply_ip();
	test_replay();
	test_air_policy();
	test_items();
	test_runner();
	test_view_page();
	printf("# dlDmripTest: %d checks, %s\n", checks, failures ? "FAILED" : "PASSED");
	return failures ? 1 : 0;
}
