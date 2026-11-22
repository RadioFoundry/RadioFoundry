// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Key fill's protocol engine (core/keyfill/keyfill.c, docs/spec/key-fill.md) with two simulated radios: a whole fill for
// each class of key, a radio in the middle, every message changed on the way, replays of an earlier session, every
// time-out, ABORT both ways, a third radio's COMMIT, the receiver's refusals, the wipe of every secret, the payload's
// strict parser, the randomness pool, and a fuzz loop over received messages in every state.
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "keyfill/keyfill.h"
#include "support/entropy.h"
#include "support/hkdf.h"
#include "fuzz.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static uint32_t micros;
uint32_t hal_micros(void)
{
	return micros += 977u;
}

#define S_ID	9991234u
#define R_ID	9995678u
#define M_ID	9990001u

// Test randomness: distinct for each call, never a source of keys outside this test
static uint32_t rng = 0x1234567u;
static void rnd(uint8_t out[KF_RANDOM_LEN])
{
	for (int i = 0; i < KF_RANDOM_LEN; i++) {
		rng = rng * 1103515245u + 12345u;
		out[i] = (uint8_t)(rng >> 16);
	}
}

// The sender's key: a privacy slot (class 1) or an AES-256-sized traffic key (class 2)
static const uint8_t KEY32[32] = { 0x60, 0x3d, 0xeb, 0x10, 0x15, 0xca, 0x71, 0xbe, 0x2b, 0x73, 0xae, 0xf0, 0x85, 0x7d,
				   0x77, 0x81, 0x1f, 0x35, 0x2c, 0x07, 0x3b, 0x61, 0x08, 0xd7, 0x2d, 0x98, 0x10, 0xa3,
				   0x09, 0x14, 0xdf, 0xf4 };
static int payload_calls;
static int payload_class2(uint8_t *out, int cap, void *ctx)
{
	(void)ctx;
	payload_calls++;
	kf_item_t it = { .cls = 2, .type = 5, .alg = 0x25, .key_id = 7, .flags = 0, .id_len = 1, .identity = { 1 },
			 .key_len = 32, .key = KEY32 };
	return kf_item_build(&it, out, cap);
}
static int payload_class1(uint8_t *out, int cap, void *ctx)
{
	(void)ctx;
	static const uint8_t v[4] = { 0, 0, 0, 5 };
	kf_item_t it = { .cls = 1, .type = 1, .slot = 2, .key_len = 4, .key = v };
	return kf_item_build(&it, out, cap);
}
static int payload_none(uint8_t *out, int cap, void *ctx)
{
	(void)out;
	(void)cap;
	(void)ctx;
	return 0;
}

static kf_session_t S, R, M1, M2;
static uint32_t now;

// The air between two sessions: what a sends goes to b if it is addressed to b's ID. Returns the length moved
typedef void (*tamper_fn)(uint8_t *m, int *len);
static tamper_fn tamper;
static uint8_t tamper_type;
static uint8_t last[KF_MSG_MAX];	// the last message moved, as sent
static int last_len;
static bool capture;			// keep each message moved as a fuzz seed
static uint8_t seeds[16][KF_MSG_MAX];
static int seed_len[16], nseeds;

static int hop(kf_session_t *a, kf_session_t *b)
{
	uint8_t m[KF_MSG_MAX];
	uint32_t dst;
	int n = kf_tx(a, m, &dst);
	if (n == 0)
		return 0;
	memcpy(last, m, (size_t)n);
	last_len = n;
	if (capture && nseeds < 16) {
		memcpy(seeds[nseeds], m, (size_t)n);
		seed_len[nseeds++] = n;
	}
	if (tamper != NULL && m[1] == tamper_type)
		tamper(m, &n);
	if (dst == b->own_id)
		kf_rx(b, a->own_id, m, n, now);
	return n;
}

// Run the two until neither has anything to send
static void pump(kf_session_t *a, kf_session_t *b)
{
	for (int i = 0; i < 20; i++)
		if (hop(a, b) == 0 && hop(b, a) == 0)
			return;
}

static bool zeros(const void *p, size_t n)
{
	const uint8_t *b = p;
	for (size_t i = 0; i < n; i++)
		if (b[i])
			return false;
	return true;
}

static bool secrets_wiped(const kf_session_t *s)
{
	return zeros(s->priv, sizeof s->priv) && zeros(s->k_mac, sizeof s->k_mac) && zeros(s->k_enc, sizeof s->k_enc) &&
		zeros(s->t, sizeof s->t) && zeros(s->payload, sizeof s->payload) && zeros(&s->ws, sizeof s->ws) &&
		zeros(s->commit, sizeof s->commit);
}

// Up to Compare: R listens, S sends, the public keys go both ways
static void start(kf_payload_fn fn)
{
	uint8_t r[KF_RANDOM_LEN];
	tamper = NULL;
	rnd(r);
	kf_listen(&R, R_ID, r, now);
	rnd(r);
	CHECK(kf_send(&S, S_ID, R_ID, fn, NULL, r, now));
	pump(&S, &R);
}

static void test_fill_class2(void)
{
	payload_calls = 0;
	start(payload_class2);
	CHECK(S.state == KF_ST_COMPARE && R.state == KF_ST_COMPARE);
	CHECK(S.code == R.code && S.code < KF_CODE_MOD);
	CHECK(R.peer_id == S_ID && S.peer_id == R_ID);
	CHECK(payload_calls == 0);	// the key stays in the store until KEY
	// R's user first: READY waits at S until S's user answers
	kf_confirm(&R, true, now);
	pump(&S, &R);
	CHECK(R.state == KF_ST_SENT_READY && S.state == KF_ST_COMPARE && S.peer_ready);
	kf_confirm(&S, true, now);
	CHECK(payload_calls == 1 && S.state == KF_ST_SENT_KEY);
	pump(&S, &R);
	CHECK(R.state == KF_ST_STORING);
	int n;
	const uint8_t *p = kf_payload(&R, &n);
	kf_item_t it;
	CHECK(p != NULL && kf_item_parse(p, n, &it));
	CHECK(it.cls == 2 && it.type == 5 && it.alg == 0x25 && it.key_id == 7 && it.id_len == 1 && it.identity[0] == 1);
	CHECK(it.key_len == 32 && memcmp(it.key, KEY32, 32) == 0);
	const uint8_t kcv[3] = { 0xAB, 0xCD, 0xEF };
	kf_stored(&R, KF_STORED, kcv, now);
	pump(&S, &R);
	CHECK(R.state == KF_ST_DONE && S.state == KF_ST_DONE);
	CHECK(S.kcv_ok && memcmp(S.kcv, kcv, 3) == 0 && R.kcv_ok && memcmp(R.kcv, kcv, 3) == 0);
	CHECK(secrets_wiped(&S) && secrets_wiped(&R));
	CHECK(S.stats.bad_tag == 0 && R.stats.bad_tag == 0);
	// The key never crossed in the clear
	bool clear = false;
	for (int i = 0; i + 32 <= last_len; i++)
		clear |= memcmp(&last[i], KEY32, 32) == 0;
	CHECK(!clear);
}

static void test_fill_class1_sender_first(void)
{
	start(payload_class1);
	// S's user first: it waits for READY, then KEY goes at once
	kf_confirm(&S, true, now);
	CHECK(S.state == KF_ST_WAIT_READY);
	kf_confirm(&R, true, now);
	pump(&S, &R);
	CHECK(R.state == KF_ST_STORING);
	int n;
	const uint8_t *p = kf_payload(&R, &n);
	kf_item_t it;
	CHECK(kf_item_parse(p, n, &it) && it.cls == 1 && it.type == 1 && it.slot == 2 && it.key[3] == 5);
	kf_stored(&R, KF_STORED, (const uint8_t[3]){ 1, 2, 3 }, now);
	pump(&S, &R);
	CHECK(S.state == KF_ST_DONE && S.kcv[2] == 3);
}

// A radio in the middle: it plays the receiver to S (M1) and the sender to R (M2), with its own keys. It can't make
// the codes agree but by chance (1 in a million); here they differ, and the users say no
static void test_middle(void)
{
	uint8_t r[KF_RANDOM_LEN];
	int same = 0;
	for (int round = 0; round < 50; round++) {
		rnd(r);
		kf_listen(&R, R_ID, r, now);
		rnd(r);
		kf_listen(&M1, R_ID, r, now);	// it answers to R's ID
		rnd(r);
		CHECK(kf_send(&S, S_ID, R_ID, payload_class2, NULL, r, now));
		pump(&S, &M1);			// S <-> M1: Compare
		rnd(r);
		CHECK(kf_send(&M2, S_ID, R_ID, payload_class2, NULL, r, now));
		pump(&M2, &R);			// M2 <-> R: Compare
		CHECK(S.state == KF_ST_COMPARE && R.state == KF_ST_COMPARE);
		same += S.code == R.code;
		kf_confirm(&S, false, now);	// the users see different codes
		kf_confirm(&R, false, now);
		CHECK(S.state == KF_ST_FAILED && S.why == KF_WHY_MISMATCH && R.why == KF_WHY_MISMATCH);
		pump(&S, &M1);
		CHECK(M1.state == KF_ST_FAILED && M1.by_peer && M1.why == KF_WHY_MISMATCH);
		CHECK(payload_calls >= 0);
	}
	CHECK(same == 0);
}

// Each message changed on the way: the session fails before any key is stored, or the codes differ
static void flip_last(uint8_t *m, int *len) { m[*len - 1] ^= 0x01; }
static void flip_body(uint8_t *m, int *len) { (void)len; m[KF_HEADER + 1] ^= 0x80; }
static void cut(uint8_t *m, int *len) { (void)m; *len -= 1; }

static void test_tamper(void)
{
	static const struct { uint8_t type; tamper_fn fn; } cases[] = {
		{ KF_COMMIT, flip_body }, { KF_SPUB, flip_body }, { KF_READY, flip_last }, { KF_KEY, flip_body },
		{ KF_KEY, flip_last }, { KF_DONE, flip_body }, { KF_DONE, flip_last }, { KF_KEY, cut }, { KF_RPUB, flip_body },
	};
	for (unsigned c = 0; c < sizeof cases / sizeof cases[0]; c++) {
		start(payload_class2);
		tamper = cases[c].fn;
		tamper_type = cases[c].type;
		if (cases[c].type == KF_COMMIT || cases[c].type == KF_SPUB || cases[c].type == KF_RPUB) {
			// redo the opening with the change in place
			uint8_t r[KF_RANDOM_LEN];
			rnd(r);
			kf_listen(&R, R_ID, r, now);
			rnd(r);
			kf_send(&S, S_ID, R_ID, payload_class2, NULL, r, now);
			pump(&S, &R);
			if (cases[c].type == KF_RPUB) {
				// a changed public key: both reach Compare with different codes (the users catch it)
				CHECK(S.state == KF_ST_COMPARE && R.state == KF_ST_COMPARE && S.code != R.code);
				continue;
			}
			CHECK(R.state == KF_ST_FAILED && R.why == KF_WHY_CHECK);
			CHECK(S.state == KF_ST_FAILED && S.by_peer);
			continue;
		}
		kf_confirm(&R, true, now);
		kf_confirm(&S, true, now);
		pump(&S, &R);
		if (cases[c].type == KF_DONE && R.state == KF_ST_STORING) {
			kf_stored(&R, KF_STORED, (const uint8_t[3]){ 1, 2, 3 }, now);
			pump(&S, &R);
		}
		CHECK(R.state != KF_ST_DONE || cases[c].type == KF_DONE);
		CHECK(R.state != KF_ST_STORING);
		if (cases[c].type == KF_KEY && cases[c].fn == cut) {
			// a short KEY doesn't parse: dropped, and R times out later
			CHECK(R.state == KF_ST_SENT_READY);
			now += KF_COMPARE_MS + KF_STEP_MS;
			kf_tick(&R, now);
			CHECK(R.state == KF_ST_FAILED && R.why == KF_WHY_TIMEOUT);
			continue;
		}
		if (cases[c].type == KF_DONE) {
			// R stored and is done; the changed DONE fails at S, which tells R (already done: dropped)
			CHECK(S.state == KF_ST_FAILED && S.why == KF_WHY_CHECK);
			continue;
		}
		if (cases[c].type == KF_READY) {
			CHECK(S.state == KF_ST_FAILED && S.why == KF_WHY_CHECK && R.state == KF_ST_FAILED && R.by_peer);
			continue;
		}
		CHECK(R.state == KF_ST_FAILED && R.why == KF_WHY_CHECK && S.state == KF_ST_FAILED && S.by_peer);
	}
	tamper = NULL;
}

// KEY and DONE of an earlier session, played into a later one: dropped (another session ID), or failing their tag
static void test_replay(void)
{
	start(payload_class2);
	kf_confirm(&R, true, now);
	kf_confirm(&S, true, now);
	hop(&R, &S);		// READY
	uint8_t old_key[KF_MSG_MAX];
	uint32_t dst;
	int n = kf_tx(&S, old_key, &dst);	// KEY of session 1, kept and not delivered
	CHECK(n > 0 && old_key[1] == KF_KEY);
	start(payload_class2);
	kf_confirm(&R, true, now);
	pump(&S, &R);
	CHECK(R.state == KF_ST_SENT_READY);
	uint32_t dropped = R.stats.dropped;
	kf_rx(&R, S_ID, old_key, n, now);	// as it was: another session ID
	CHECK(R.state == KF_ST_SENT_READY && R.stats.dropped == dropped + 1);
	old_key[2] = (uint8_t)(R.sid >> 24);	// relabeled with this session's ID: its tag fails
	old_key[3] = (uint8_t)(R.sid >> 16);
	old_key[4] = (uint8_t)(R.sid >> 8);
	old_key[5] = (uint8_t)R.sid;
	kf_rx(&R, S_ID, old_key, n, now);
	CHECK(R.state == KF_ST_FAILED && R.why == KF_WHY_CHECK && R.stats.bad_tag == 1);
}

static void test_timeouts(void)
{
	uint8_t r[KF_RANDOM_LEN];
	uint8_t m[KF_MSG_MAX];
	uint32_t dst;
	// Listening: 5 min, nothing sent (no other radio yet)
	rnd(r);
	kf_listen(&R, R_ID, r, now);
	kf_tick(&R, now + KF_LISTEN_MS - 1);
	CHECK(R.state == KF_ST_LISTEN);
	kf_tick(&R, now + KF_LISTEN_MS);
	CHECK(R.state == KF_ST_FAILED && R.why == KF_WHY_TIMEOUT && kf_tx(&R, m, &dst) == 0);
	// Sent COMMIT: 30 s, then ABORT
	rnd(r);
	kf_send(&S, S_ID, R_ID, payload_class2, NULL, r, now);
	kf_tx(&S, m, &dst);
	kf_tick(&S, now + KF_STEP_MS);
	CHECK(S.state == KF_ST_FAILED && S.why == KF_WHY_TIMEOUT);
	CHECK(kf_tx(&S, m, &dst) == KF_HEADER + 1 && m[1] == KF_ABORT && m[KF_HEADER] == KF_WHY_TIMEOUT && dst == R_ID);
	// Sent RPUB: 30 s
	rnd(r);
	kf_listen(&R, R_ID, r, now);
	rnd(r);
	kf_send(&S, S_ID, R_ID, payload_class2, NULL, r, now);
	hop(&S, &R);
	CHECK(R.state == KF_ST_SENT_RPUB);
	kf_tick(&R, now + KF_STEP_MS);
	CHECK(R.state == KF_ST_FAILED && R.why == KF_WHY_TIMEOUT);
	// Compare: 2 min on each side; READY in time keeps R waiting up to 2.5 min
	start(payload_class2);
	uint32_t t0 = now;
	kf_confirm(&R, true, now);
	pump(&S, &R);
	kf_tick(&S, t0 + KF_COMPARE_MS - 1);
	kf_tick(&R, t0 + KF_COMPARE_MS + KF_STEP_MS - 1);
	CHECK(S.state == KF_ST_COMPARE && R.state == KF_ST_SENT_READY);
	kf_tick(&S, t0 + KF_COMPARE_MS);
	CHECK(S.state == KF_ST_FAILED && S.why == KF_WHY_TIMEOUT);
	pump(&S, &R);
	CHECK(R.state == KF_ST_FAILED && R.by_peer && R.why == KF_WHY_TIMEOUT);
	// Sent KEY: 30 s for DONE
	start(payload_class2);
	kf_confirm(&R, true, now);
	kf_confirm(&S, true, now);
	pump(&S, &R);
	CHECK(S.state == KF_ST_SENT_KEY && R.state == KF_ST_STORING);
	kf_tick(&R, now + 3600000u);	// Storing has no time-out: a storage job always ends
	CHECK(R.state == KF_ST_STORING);
	kf_tick(&S, now + KF_STEP_MS);
	CHECK(S.state == KF_ST_FAILED && S.why == KF_WHY_TIMEOUT);
	kf_wipe(&R);
	now += 1000;
}

static void test_abort_cancel_busy(void)
{
	uint8_t m[KF_MSG_MAX];
	uint32_t dst;
	// Cancel while listening: nothing to tell
	uint8_t r[KF_RANDOM_LEN];
	rnd(r);
	kf_listen(&R, R_ID, r, now);
	kf_cancel(&R, now);
	CHECK(R.state == KF_ST_FAILED && R.why == KF_WHY_CANCELED && kf_tx(&R, m, &dst) == 0 && secrets_wiped(&R));
	// Cancel in Compare: ABORT reaches the other radio
	start(payload_class2);
	kf_cancel(&S, now);
	pump(&S, &R);
	CHECK(R.state == KF_ST_FAILED && R.by_peer && R.why == KF_WHY_CANCELED && secrets_wiped(&R));
	start(payload_class2);
	kf_cancel(&R, now);
	pump(&S, &R);
	CHECK(S.state == KF_ST_FAILED && S.by_peer && S.why == KF_WHY_CANCELED);
	// An ABORT from a third radio, or with another session ID, is ignored
	start(payload_class2);
	uint8_t ab[KF_HEADER + 1] = { KF_VERSION, KF_ABORT, 0, 0, 0, 0, KF_WHY_CANCELED };
	kf_rx(&S, M_ID, ab, sizeof ab, now);
	CHECK(S.state == KF_ST_COMPARE);
	kf_rx(&S, R_ID, ab, sizeof ab, now);	// session ID 0
	CHECK(S.state == KF_ST_COMPARE || S.sid == 0);
	// A third radio's COMMIT during a session: ABORT "busy" to it, the session goes on
	uint8_t r2[KF_RANDOM_LEN];
	rnd(r2);
	CHECK(kf_send(&M2, M_ID, R_ID, payload_class2, NULL, r2, now));
	int n = kf_tx(&M2, m, &dst);
	kf_rx(&R, M_ID, m, n, now);
	CHECK(R.state == KF_ST_COMPARE && R.peer_id == S_ID);
	n = kf_tx(&R, m, &dst);
	CHECK(n == KF_HEADER + 1 && m[1] == KF_ABORT && m[KF_HEADER] == KF_WHY_BUSY && dst == M_ID);
	kf_rx(&M2, R_ID, m, n, now);
	CHECK(M2.state == KF_ST_FAILED && M2.by_peer && M2.why == KF_WHY_BUSY);
	// Our own ID and bad IDs
	CHECK(!kf_send(&M2, S_ID, S_ID, payload_class2, NULL, r2, now));
	CHECK(!kf_send(&M2, S_ID, 0, payload_class2, NULL, r2, now));
	CHECK(!kf_send(&M2, S_ID, 0x1000000u, payload_class2, NULL, r2, now));
}

static void test_refusals(void)
{
	// R can't store it: DONE with the result, S fails with it, no check value
	start(payload_class2);
	kf_confirm(&R, true, now);
	kf_confirm(&S, true, now);
	pump(&S, &R);
	kf_stored(&R, KF_RES_NO_SYSTEM, NULL, now);
	pump(&S, &R);
	CHECK(R.state == KF_ST_FAILED && R.why == KF_WHY_REFUSED && R.result == KF_RES_NO_SYSTEM);
	CHECK(S.state == KF_ST_FAILED && S.why == KF_WHY_REFUSED && S.result == KF_RES_NO_SYSTEM && !S.kcv_ok);
	CHECK(secrets_wiped(&S) && secrets_wiped(&R));
	// The sender's key is gone when KEY is due: ABORT
	start(payload_none);
	kf_confirm(&R, true, now);
	kf_confirm(&S, true, now);
	pump(&S, &R);
	CHECK(S.state == KF_ST_FAILED && R.state == KF_ST_FAILED && R.by_peer);
	// The transport fails: no ABORT
	start(payload_class2);
	kf_send_failed(&S);
	uint8_t m[KF_MSG_MAX];
	uint32_t dst;
	CHECK(S.state == KF_ST_FAILED && S.why == KF_WHY_SEND && kf_tx(&S, m, &dst) == 0);
}

static void test_items(void)
{
	uint8_t buf[KF_PAYLOAD_MAX];
	kf_item_t it, back;
	// The longest: an 8-octet identity and a 32-octet key, 48 octets
	it = (kf_item_t){ .cls = 2, .type = 4, .alg = 1, .key_id = 0x1234, .flags = 0x30, .id_len = 8,
			  .identity = { 1, 2, 3, 4, 5, 6, 7, 8 }, .key_len = 32, .key = KEY32 };
	int n = kf_item_build(&it, buf, sizeof buf);
	CHECK(n == 48);
	CHECK(kf_item_parse(buf, n, &back) && back.key_id == 0x1234 && back.flags == 0x30 && back.identity[7] == 8);
	CHECK(buf[3] == 0x34 && buf[4] == 0x12);	// the key ID little endian, as the record holds it
	// Strict: every length must agree
	CHECK(!kf_item_parse(buf, n - 1, &back));
	CHECK(!kf_item_parse(buf, 0, &back));
	buf[6] = 9;
	CHECK(!kf_item_parse(buf, n, &back));
	buf[6] = 8;
	buf[15] = 33;
	CHECK(!kf_item_parse(buf, n, &back));
	buf[0] = 3;
	CHECK(!kf_item_parse(buf, n, &back));
	it.key_len = 33;
	CHECK(kf_item_build(&it, buf, sizeof buf) == 0);
	const uint8_t v[4] = { 0, 0, 0x12, 0x34 };
	it = (kf_item_t){ .cls = 1, .type = 2, .slot = 4, .key_len = 4, .key = v };
	CHECK(kf_item_build(&it, buf, sizeof buf) == 7);
	CHECK(kf_item_parse(buf, 7, &back) && back.slot == 4 && back.key[3] == 0x34);
	buf[2] = 5;
	CHECK(!kf_item_parse(buf, 7, &back));
	char t[8];
	kf_code_text(482913, t);
	CHECK(strcmp(t, "482 913") == 0);
	kf_code_text(7, t);
	CHECK(strcmp(t, "000 007") == 0);
}

static void test_entropy(void)
{
	uint8_t a[36], b[36];
	CHECK(entropy_bits() == 0);
	for (int i = 0; i < 31; i++)
		entropy_stir(&i, sizeof i, ENTROPY_KEY_BITS);
	CHECK(entropy_bits() == 124 && entropy_bits() < ENTROPY_FULL);
	entropy_stir(NULL, 0, ENTROPY_KEY_BITS);
	CHECK(entropy_bits() >= ENTROPY_FULL);
	entropy_take(a, sizeof a);
	CHECK(entropy_bits() == 0);
	entropy_take(b, sizeof b);
	CHECK(memcmp(a, b, sizeof a) != 0 && !zeros(a, sizeof a));
	for (int i = 0; i < 100; i++)
		entropy_stir(NULL, 0, 4);
	CHECK(entropy_bits() == 256);	// capped
}

// Received messages mutated from real ones, in every state: no crash, no store without the whole exchange
static void test_fuzz(void)
{
	fuzz_t f;
	fuzz_init(&f, 0x4B46);
	// Every message of one whole fill, kept as seeds
	nseeds = 0;
	capture = true;
	start(payload_class2);
	kf_confirm(&R, true, now);
	kf_confirm(&S, true, now);
	pump(&S, &R);
	kf_stored(&R, KF_STORED, (const uint8_t[3]){ 1, 2, 3 }, now);
	pump(&S, &R);
	capture = false;
	int k = nseeds;
	CHECK(k == 6);
	for (long i = 0; i < fuzz_iters(20000); i++) {
		if (i % 500 == 0) {
			// a fresh pair, left in a state that rotates
			start(payload_class2);
			int st = (int)(i / 500 % 3);
			if (st >= 1)
				kf_confirm(&R, true, now);
			if (st == 2) {
				kf_confirm(&S, true, now);
				hop(&R, &S);
			}
		}
		uint8_t buf[KF_MSG_MAX + 8];
		int s = (int)fuzz_below(&f, (uint32_t)k);
		memcpy(buf, seeds[s], (size_t)seed_len[s]);
		int n = fuzz_mutate(&f, buf, seed_len[s], (int)sizeof buf);
		uint8_t *p = fuzz_exact(buf, n);
		kf_session_t *to = fuzz_below(&f, 2) ? &R : &S;
		uint32_t src = fuzz_below(&f, 4) ? (to == &R ? S_ID : R_ID) : fuzz_next(&f) & 0xFFFFFFu;
		kf_rx(to, src, p, n, now);
		free(p);
		// A mutated message never completes a fill
		CHECK(R.state != KF_ST_STORING && R.state != KF_ST_DONE && S.state != KF_ST_DONE);
	}
}

int main(void)
{
	now = 1000;
	test_fill_class2();
	test_fill_class1_sender_first();
	test_middle();
	test_tamper();
	test_replay();
	test_timeouts();
	test_abort_cancel_busy();
	test_refusals();
	test_items();
	test_entropy();
	test_fuzz();
	printf(failures ? "# FAILED (%d failures)\n" : "# PASSED (0 failures)\n", failures);
	return failures ? 1 : 0;
}
