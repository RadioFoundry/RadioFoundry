// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
/*
 * Host tests for DMR AES-256 voice encryption (core/dmr/dmrAes.c, docs/spec/dmr-voice.md 5.1)
 *
 *   make -C tests/host run-aes
 */

#include <stdio.h>
#include <string.h>
#include "dmr/dmrAes.h"
#include "dmr/dmrPrivacy.h"
#include "dmr/ambe/ambeFec.h"
#include "dmr/call/dmr_call.h"

static int failures = 0;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void hex(const char *s, uint8_t *out, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		unsigned v;

		sscanf(&s[2 * i], "%2x", &v);
		out[i] = (uint8_t)v;
	}
}

// FIPS-197 Appendix C.3
static void test_fips197(void)
{
	uint8_t key[32], pt[16], ct[16], want[16];
	aes256_ctx_t ctx;

	hex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", key, 32);
	hex("00112233445566778899aabbccddeeff", pt, 16);
	hex("8ea2b7ca516745bfeafc49904b496089", want, 16);
	aes256Init(&ctx, key);
	aes256Encrypt(&ctx, pt, ct);
	CHECK(memcmp(ct, want, 16) == 0, "FIPS-197 C.3");
}

// The MI words a reference decoder printed for the PI header MI CD6554E0 (bench 2026-11-18)
static void test_mi(void)
{
	static const uint32_t want[] = { 0xCD6554E0, 0x7B451C0A, 0x899C283D, 0x2190A622, 0x82D6E6E1, 0x170A8391,
					 0x91B0B11F, 0x19F0BC4D };
	uint32_t w = want[0];

	for (unsigned i = 1; i < sizeof(want) / sizeof(want[0]); i++) {
		w = dmrAesMiNext(w);
		CHECK(w == want[i], "MI word %u: %08X", i, (unsigned)w);
	}

	uint8_t iv[16], ivw[16];

	dmrAesIv(0xCD6554E0, 2, iv);
	hex("899C283D2190A62282D6E6E1170A8391", ivw, 16);
	CHECK(memcmp(iv, ivw, 16) == 0, "IV of superframe 2");
}

// A stock radio's AES-256 call (key ID 1, throwaway test key, MI CD6554E0), the talker silent: 28 voice bursts as
// received after the PI header (A of superframe 0 first), from the burst capture. Every frame from burst C on decrypts
// to a near-silence codeword (its first 5 bits set; about 1 in 32 by chance); the first two bursts are the stock
// radio's key-up frames. FNV1A is over all 84 decrypted frames (7 bytes each, the 49 bits right-aligned, LSB first),
// computed by an independent implementation whose output matched a reference decoder's bit for bit
static const char *BCAP1[] = {
	"9d1c20fffa9a8ab79ef8a066ac9a92cff7fc7fab6034708c9c9a16",
	"d26745b66d73513747bb8173b07990637428a30bf6d707baafc903",
	"d7527c532b57b58e1ab1860a361505fca4d7ec16d8aaad1018c3ca",
	"bb95d062bb0995084e9b2fb2f34f90f5fb3823eaafef190e164f1f",
	"33efd973ce00372cccfb37470609b1825d8b2abde6409e1a65a180",
	"84402cb2d6e174fc88b16949fee6f548ec437b66966e417dd98469",
	"09f9f50ada22820e272d23e18377c380a41371aee41b80534d7da6",
	"2843f7ea241634346dcd3a15f0e0466c818cf9ddbf733b7f9fb6f5",
	"0792f2e48768acd84d772df53d22b2e8e041092d39dc92b7db3b6e",
	"23be01b327bbaaffab3d7536e6726b9531404de17df0592c44411e",
	"63473631d54e4d6d1370a737d20165c34cb41750f7ad253afe83a0",
	"fdcd2882ad5f4ee438ae1243c5da5092aa9e2964052ba97043a637",
	"4f02859ec919c9cb8a9521612e6631e8cc407bc9e2170f753acfa0",
	"58278c1dea7f7936a6caff26c867af5ea1edb6dc95c8fe981f432a",
	"d8b18f56aedfeb1ce99287b0cf0ef57fc7fa38fd086c10ef452ac8",
	"3b99cc29763500feef8f7d91760f6dfe077eb1aa2ca67c2bbd5ec5",
	"d641b0166e421d4d24fa382d144bf8b5bb239237f4b828da716b0d",
	"c0574edc2ee469c9ce1d346b3ec6c94b05bfcbdc0db62448063a30",
	"b6b15c207f675f062143d52918cf7c9ba356df6cbf42abae6d0f53",
	"422797af149e4dade2e818a0033525bbc1113529994e5240f696e4",
	"fdbad48e9a2011bcb55a27be4cfd1f6e63900084a6b78f486b79fa",
	"7c0d1bb7b9b42d1090603fc309ada2a02e68b6d8a163773ebefc93",
	"d4ec187961486686e45c3fc10ebcef2cdbb07f1b056701c49720c0",
	"c0c802ef630dd009c82cd3460fd8a31b3678414e01d764df457584",
	"c5433bde55b3fe3e688535422ee9acc729873130d8fc617b21fe23",
	"dcf11f45c0170c79180b455185b6822e64439c4c444b2cff29588c",
	"32b24532706f2d8aa607a8bbb55ecbe6fb2042f8f705b47dedc577",
	"d0493fb05e5b5393f940af88fbaf1368356aa4d4ab204dd85de052"
};
static const char *KEY_A = "15168A8DF27104730488F2BB1E351641F2E1BEC86901EBCB6E01778B64301E2D";
#define FNV1A	0xBB8C46718CBBEF0DULL

static uint64_t frame_bits(const uint8_t *frame)
{
	uint8_t bits[AMBE_PARAM_BITS];
	uint64_t v = 0;

	ambeFecDecode(frame, bits, NULL);
	for (int i = 0; i < AMBE_PARAM_BITS; i++)
		v = (v << 1) | bits[i];
	return v;
}

static void test_capture(void)
{
	dmrAesState_t st;
	uint8_t key[32];
	unsigned silent = 0;
	uint64_t fnv = 0xcbf29ce484222325ULL;

	hex(KEY_A, key, 32);
	dmrAesStateInit(&st);
	CHECK(!dmrAesHasKeys(&st), "empty keyring");
	CHECK(!dmrAesSetKey(&st, DMR_AES_KEY_SLOTS, DMR_AES_ALG_AES256, 1, key), "bad slot");
	CHECK(!dmrAesSetKey(&st, 0, 0x21, 1, key), "bad algorithm");
	CHECK(dmrAesSetKey(&st, 0, DMR_AES_ALG_AES256, 1, key), "set key");
	CHECK(dmrAesHasKeys(&st) && dmrAesFindKey(&st, DMR_AES_ALG_AES256, 1) == 0, "find key");
	CHECK(dmrAesFindKey(&st, DMR_AES_ALG_AES256, 2) == -1, "other key ID");

	for (unsigned n = 0; n < sizeof(BCAP1) / sizeof(BCAP1[0]); n++) {
		uint8_t v[27];
		struct dmr_crypt_pos pos = { .alg = DMR_AES_ALG_AES256, .key_id = 1, .mi = 0xCD6554E0,
					     .superframe = n / 6, .voice_index = (uint8_t)(n % 6) };

		hex(BCAP1[n], v, 27);
		CHECK(dmrAesCallHook(v, &pos, false, &st), "hook");
		for (int f = 0; f < 3; f++) {
			uint64_t b = frame_bits(&v[f * 9]);

			bool quiet = (b >> 44) == 0x1F;

			CHECK(n < 2 || quiet, "burst %u frame %d: %014llX", n, f, (unsigned long long)b);
			silent += quiet;
			for (int i = 0; i < 7; i++)
				fnv = (fnv ^ ((b >> (8 * i)) & 0xFF)) * 0x100000001b3ULL;
		}
	}
	CHECK(silent == 79, "silent frames %u", silent);
	CHECK(fnv == FNV1A, "decrypted frames %016llX", (unsigned long long)fnv);

	// A wrong key: no silence
	key[0] ^= 1;
	dmrAesSetKey(&st, 0, DMR_AES_ALG_AES256, 1, key);
	key[0] ^= 1;
	silent = 0;
	for (unsigned n = 0; n < sizeof(BCAP1) / sizeof(BCAP1[0]); n++) {
		uint8_t w[27];
		struct dmr_crypt_pos p = { .alg = DMR_AES_ALG_AES256, .key_id = 1, .mi = 0xCD6554E0, .superframe = n / 6,
					   .voice_index = (uint8_t)(n % 6) };

		hex(BCAP1[n], w, 27);
		dmrAesCallHook(w, &p, false, &st);
		for (int f = 0; f < 3; f++)
			silent += (frame_bits(&w[f * 9]) >> 44) == 0x1F;
	}
	CHECK(silent < 10, "wrong key: %u silent frames", silent);

	// No key for that key ID: not played
	uint8_t v[27];
	struct dmr_crypt_pos pos = { .alg = DMR_AES_ALG_AES256, .key_id = 7, .mi = 1, .superframe = 0 };

	hex(BCAP1[0], v, 27);
	CHECK(!dmrAesCallHook(v, &pos, false, &st), "unknown key ID");
	CHECK(dmrAesSetKey(&st, 0, 0, 0, NULL) && !dmrAesHasKeys(&st), "clear key");
}

// TX then RX: the keystream round-trips through the FEC encode
static void test_roundtrip(void)
{
	dmrAesState_t st;
	uint8_t key[32];

	hex(KEY_A, key, 32);
	dmrAesStateInit(&st);
	dmrAesSetKey(&st, 0, DMR_AES_ALG_AES256, 1, key);
	for (uint8_t i = 0; i < 6; i++) {
		uint8_t clear[27], v[27];
		struct dmr_crypt_pos pos = { .alg = DMR_AES_ALG_AES256, .key_id = 1, .mi = 0x12345678, .superframe = 3,
					     .voice_index = i };

		for (int f = 0; f < 3; f++) {
			uint8_t bits[AMBE_PARAM_BITS];

			for (int k = 0; k < AMBE_PARAM_BITS; k++)
				bits[k] = (uint8_t)(((k * 7 + f * 3 + i) >> 2) & 1);
			ambeFecEncode(bits, &clear[f * 9]);
		}
		memcpy(v, clear, 27);
		dmrAesCallHook(v, &pos, true, &st);
		CHECK(memcmp(v, clear, 27) != 0, "TX changes the burst");
		dmrAesCallHook(v, &pos, false, &st);
		// Embedded MI replaces four uncoded speech bits per frame.
		for (int j = 0; j < 27; j++) {
			unsigned mask = j % 9 >= 7 ? 0xee : 0xff;
			CHECK(((v[j] ^ clear[j]) & mask) == 0, "round trip burst %u byte %d", i, j);
		}
	}
}

// Twelve signaling bits per burst from two SDR recordings and superframe 1 of BCAP1.
static void test_tx_late_entry(void)
{
	static const struct {
		uint32_t mi, sf;
		uint16_t burst[6];
	} captures[] = {
		{ 0xDEA24DE5, 6, { 0xD2E, 0x1E9, 0x8CC, 0x302, 0x5BC, 0xB7A } },
		{ 0x9941001C, 4, { 0xB29, 0xC69, 0x373, 0xBFA, 0x37A, 0xFE9 } },
		{ 0xCD6554E0, 1, { 0x8C3, 0x92D, 0x983, 0xB36, 0xE42, 0x44C } },
	};
	dmrAesState_t st;
	uint8_t key[32];

	hex(KEY_A, key, 32);
	dmrAesStateInit(&st);
	dmrAesSetKey(&st, 0, DMR_AES_ALG_AES256, 1, key);
	for (unsigned c = 0; c < sizeof(captures) / sizeof(captures[0]); c++) {
		struct dmr_crypt_pos pos = { .alg = DMR_AES_ALG_AES256, .key_id = 1, .mi = captures[c].mi,
					     .superframe = captures[c].sf };
		for (uint8_t i = 0; i < 6; i++) {
			pos.voice_index = i;
			for (unsigned pattern = 0; pattern < 2; pattern++) {
				uint8_t v[27];
				uint16_t signaling = 0;
				memset(v, pattern ? 0xff : 0, sizeof(v));
				CHECK(dmrAesCallHook(v, &pos, true, &st), "TX hook");
				for (int f = 0; f < 3; f++) {
					const uint8_t *frame = &v[f * 9];
					unsigned nibble = ((frame[8] & 1) << 3) | ((frame[8] & 16) >> 2) |
							  ((frame[7] & 1) << 1) | ((frame[7] & 16) >> 4);
					signaling = (uint16_t)((signaling << 4) | nibble);
				}
				CHECK(signaling == captures[c].burst[i], "late-entry call %u burst %u: %03X", c, i, signaling);
			}
		}
	}
}

// Our TX against the stock radio's: the captured call decrypted, then encrypted again by our TX path with the same key
// and MI, reproduces the capture except for three signaling bits in its first burst.
static void test_tx_matches_stock(void)
{
	dmrAesState_t st;
	uint8_t key[32];
	unsigned same = 0, n_bursts = sizeof(BCAP1) / sizeof(BCAP1[0]);

	hex(KEY_A, key, 32);
	dmrAesStateInit(&st);
	dmrAesSetKey(&st, 0, DMR_AES_ALG_AES256, 1, key);
	for (unsigned n = 0; n < n_bursts; n++) {
		uint8_t air[27], v[27];
		struct dmr_crypt_pos pos = { .alg = DMR_AES_ALG_AES256, .key_id = 1, .mi = 0xCD6554E0,
					     .superframe = n / 6, .voice_index = (uint8_t)(n % 6) };

		hex(BCAP1[n], air, 27);
		memcpy(v, air, 27);
		dmrAesCallHook(v, &pos, false, &st);
		dmrAesCallHook(v, &pos, true, &st);
		if (n == 0) {
			// The second and third frame nibbles in the capture are 7/5. The PI predicts 5/0.
			air[9 + 7] ^= 1;
			air[18 + 8] ^= 0x10;
			air[18 + 7] ^= 0x10;
		}
		same += memcmp(v, air, 27) == 0;
	}
	CHECK(same == n_bursts, "%u of %u bursts as the stock radio sent them", same, n_bursts);
}

// The TX key and the MIs it makes
static void test_tx_key_and_mi(void)
{
	dmrAesState_t st;
	uint8_t key[32], alg = 0, key_id = 0;
	static uint32_t seen[512];

	hex(KEY_A, key, 32);
	dmrAesStateInit(&st);
	CHECK(!dmrAesTxKey(&st, &alg, &key_id), "no TX key");
	CHECK(!dmrAesSetTx(&st, 0), "empty slot");
	CHECK(!dmrAesSetTx(&st, 1), "bad slot");
	dmrAesSetKey(&st, 0, DMR_AES_ALG_AES256, 7, key);
	CHECK(dmrAesSetTx(&st, 0) && dmrAesTxKey(&st, &alg, &key_id) && alg == 0x25 && key_id == 7, "TX key");
	for (unsigned i = 0; i < 512; i++) {
		seen[i] = dmrAesNewMi(1000, &st);	// the same time every call: the count still makes them differ
		CHECK(seen[i] != 0, "MI 0");
		for (unsigned j = 0; j < i; j++)
			if (seen[j] == seen[i]) {
				CHECK(0, "MI %u repeats %u", i, j);
				break;
			}
	}
	dmrAesSetKey(&st, 0, 0, 0, NULL);
	CHECK(!dmrAesTxKey(&st, &alg, &key_id), "TX key cleared with its slot");
	CHECK(dmrAesSetTx(&st, -1), "TX clear");
}

int main(void)
{
	test_fips197();
	test_mi();
	test_capture();
	test_roundtrip();
	test_tx_matches_stock();
	test_tx_late_entry();
	test_tx_key_and_mi();
	if (failures)
		printf("dmrAesTest: %d FAILED\n", failures);
	else
		printf("dmrAesTest: all passed\n");
	return failures ? 1 : 0;
}
