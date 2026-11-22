// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
/*
 * Host tests for group key fill's message (core/keyfill/groupkey.c, docs/spec/group-key-fill.md 4): a known message
 * computed apart from this code (Python's hmac and hashlib), the round trip, and every way a message is refused
 *
 *   make -C tests/host run-group-key
 */
#include <stdio.h>
#include <string.h>
#include "keyfill/groupkey.h"

static int failures;
#define CHECK(c) do { if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void hex(const char *s, uint8_t *out, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		unsigned v;
		sscanf(&s[2 * i], "%2x", &v);
		out[i] = (uint8_t)v;
	}
}

// Channel key 00..1F, session 01..08, sender 4670502, key ID 5, key 20..3F
static const char *KAT = "470101020304050607080047442625052045739E8E40DCA5611B24AE230FCDF681CAB522B09209EA05E2478A6B"
			 "5375CFE59B50B76F2AA4D2BACA0C074A472AF6B0";

int main(void)
{
	uint8_t ck[GK_KEY_LEN], other[GK_KEY_LEN], sess[GK_SESSION_LEN], key[GK_KEY_LEN], msg[GK_MSG_LEN], want[GK_MSG_LEN];
	gk_item_t it;

	for (int i = 0; i < GK_KEY_LEN; i++) {
		ck[i] = (uint8_t)i;
		other[i] = (uint8_t)(i ^ 0x80);
		key[i] = (uint8_t)(0x20 + i);
	}
	for (int i = 0; i < GK_SESSION_LEN; i++)
		sess[i] = (uint8_t)(1 + i);

	// The known message, octet for octet
	hex(KAT, want, GK_MSG_LEN);
	gk_build(msg, ck, sess, 4670502u, 5, key);
	CHECK(memcmp(msg, want, GK_MSG_LEN) == 0);

	// Opened: every field back, the key in the clear
	CHECK(gk_open(msg, GK_MSG_LEN, 4670502u, ck, &it) == GK_OK);
	CHECK(it.sender == 4670502u && it.alg == GK_ALG_AES256 && it.key_id == 5);
	CHECK(memcmp(it.key, key, GK_KEY_LEN) == 0 && memcmp(it.session, sess, GK_SESSION_LEN) == 0);
	gk_wipe(&it);
	uint8_t zero[sizeof it];
	memset(zero, 0, sizeof zero);
	CHECK(memcmp(&it, zero, sizeof it) == 0);

	// The key never shows in the message
	CHECK(memmem(msg, GK_MSG_LEN, key, 8) == NULL);

	// Another channel key: the tag fails, nothing comes out
	CHECK(gk_open(msg, GK_MSG_LEN, 4670502u, other, &it) == GK_BAD_TAG);
	CHECK(memcmp(&it, zero, sizeof it) == 0);
	// Another source than the message's sender
	CHECK(gk_open(msg, GK_MSG_LEN, 4670501u, ck, &it) == GK_BAD_SENDER);
	// Short, long, none
	CHECK(gk_open(msg, GK_MSG_LEN - 1, 4670502u, ck, &it) == GK_BAD_FORMAT);
	CHECK(gk_open(msg, GK_MSG_LEN + 1, 4670502u, ck, &it) == GK_BAD_FORMAT);
	CHECK(gk_open(NULL, GK_MSG_LEN, 4670502u, ck, &it) == GK_BAD_FORMAT);

	// Every octet altered, every bit of it: refused, each by the check it reaches first
	unsigned refused = 0;
	for (int i = 0; i < GK_MSG_LEN; i++) {
		for (int bit = 0; bit < 8; bit++) {
			uint8_t m2[GK_MSG_LEN];
			memcpy(m2, msg, sizeof m2);
			m2[i] ^= (uint8_t)(1u << bit);
			uint32_t src = 4670502u;
			if (i >= 10 && i <= 13)	// the sender's ID altered and the packet's source to match: the tag still catches it
				src = ((uint32_t)m2[10] << 24) | ((uint32_t)m2[11] << 16) | ((uint32_t)m2[12] << 8) | m2[13];
			gk_result_t r = gk_open(m2, GK_MSG_LEN, src, ck, &it);
			refused += r != GK_OK;
			if (r == GK_OK)
				printf("FAIL octet %d bit %d accepted\n", i, bit);
		}
	}
	CHECK(refused == GK_MSG_LEN * 8u);

	// Another session: another message for the same key, both open
	uint8_t msg2[GK_MSG_LEN], sess2[GK_SESSION_LEN];
	memcpy(sess2, sess, sizeof sess2);
	sess2[7] ^= 1;
	gk_build(msg2, ck, sess2, 4670502u, 5, key);
	CHECK(memcmp(msg2 + 17, msg + 17, GK_KEY_LEN) != 0);	// another keystream
	CHECK(gk_open(msg2, GK_MSG_LEN, 4670502u, ck, &it) == GK_OK && memcmp(it.key, key, GK_KEY_LEN) == 0);
	gk_wipe(&it);

	if (failures)
		printf("groupKeyTest: %d FAILED\n", failures);
	else
		printf("groupKeyTest: all passed\n");
	return failures ? 1 : 0;
}
