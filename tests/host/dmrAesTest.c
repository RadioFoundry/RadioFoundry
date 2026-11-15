// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Public NIST known-answer vectors and protocol lifecycle checks; no production keys.
#include <stdio.h>
#include <string.h>
#include "dmr/call/dmr_aes.h"
#include "dmr/call/dmr_lc.h"
#include "dmr/ambe/ambeFec.h"
#include "fuzz.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static void hex(const char *s, uint8_t *out)
{
	for (unsigned i = 0; s[2 * i]; i++) {
		unsigned value;
		sscanf(s + 2 * i, "%2x", &value);
		out[i] = (uint8_t)value;
	}
}

static void test_nist(void)
{
	uint8_t key[32], round[240], block[16], expected[64], plain[64];
	for (unsigned i = 0; i < 32; i++) key[i] = (uint8_t)i;
	hex("00112233445566778899aabbccddeeff", block);
	hex("8ea2b7ca516745bfeafc49904b496089", expected);
	dmr_aes_expand(round, key);
	dmr_aes_block(round, block);
	CHECK(memcmp(block, expected, 16) == 0);
	// SP 800-38A F.4.5, AES-256 OFB, four successive feedback blocks.
	hex("603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4", key);
	hex("000102030405060708090a0b0c0d0e0f", block);
	hex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51"
	    "30c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710", plain);
	hex("dc7e84bfda79164b7ecd8486985d38604febdc6740d20b3ac88f6ad82a4fb08d"
	    "71ab47a086e86eedf39d1c5bba97c4080126141d67f37be8538f5a8be740e484", expected);
	dmr_aes_expand(round, key);
	for (unsigned f = 0; f < 4; f++) {
		dmr_aes_block(round, block);
		for (unsigned i = 0; i < 16; i++) CHECK((block[i] ^ plain[16 * f + i]) == expected[16 * f + i]);
	}
}

static dmr_aes_keys_t keys(void)
{
	dmr_aes_keys_t k = { .valid = 3, .tx_slot = 1, .id = { 1, 239 } };
	for (unsigned i = 0; i < 32; i++) { k.key[0][i] = (uint8_t)i; k.key[1][i] = (uint8_t)(i ^ 0xA5); }
	return k;
}

static void voice_burst(dmr_aes_t *tx, unsigned i, dmr_burst_t *b)
{
	memset(b, 0, sizeof(*b));
	b->kind = DMR_BURST_VOICE; b->voice_index = (uint8_t)i; b->len = 27;
	uint8_t bits[49] = { 0 };
	for (unsigned f = 0; f < 3; f++) ambeFecEncode(bits, b->payload + 9 * f);
	dmr_aes_voice(tx, b->payload, (uint8_t)i, true);
	if (i == 5) {
		uint32_t a = dmr_aes_announcement(tx->id);
		b->emb_valid = true;
		for (unsigned j = 0; j < 4; j++) b->emb[j] = (uint8_t)(a >> (24 - 8 * j));
	}
}

static void test_protocol(void)
{
	dmr_aes_keys_t k = keys();
	dmr_aes_t tx, rx;
	uint8_t iv[16], expected[126], pi[12], out[27];
	CHECK(dmr_aes_iv(0x12345678, iv) == 0xB451463A);
	hex("12345678b451463a41d78991a49a6402", expected);
	CHECK(memcmp(iv, expected, 16) == 0);
	CHECK(dmr_aes_begin(&tx, &k, 1, 0x12345678));
	// Independent OpenSSL AES-256-OFB output, zero input, key octets 00..1F, above IV, first block discarded.
	hex("3c30d1a337dd055d1032b6dd22262c8a72f1177420ac6db511854538ac68451f174265752f5160e0bb40b64bda8ec97c"
	    "904091866f77ac250bacf5d2c0ef28bc6c1eb86b8a6fffc79b976cdd62960aeea328c2d145ca3e31ff1a8bb560ce52"
	    "52b9cffce15b8fa4c548cd480a4b12c42cec300c53dd474cd8fc2ef1a79f67", expected);
	CHECK(memcmp(tx.stream, expected, 126) == 0);
	CHECK(dmr_aes_announcement(1) == 0x44428881u); // Payload (key 1 << 3) | algorithm 5 = 00D.
	dmr_aes_make_pi(&tx, 91, pi);
	CHECK(pi[0] == 0x25 && pi[1] == 0x10 && pi[2] == 1);
	CHECK(dmr_aes_pi(&rx, &k, pi, 91));
	CHECK(!dmr_aes_pi(&rx, &k, pi, 92));
	pi[4] ^= 1;
	CHECK(!dmr_aes_pi(&rx, &k, pi, 91));
	pi[4] ^= 1;
	CHECK(dmr_aes_pi(&rx, &k, pi, 91));
	pi[0] = 0x24;
	uint16_t crc = dmr_crc_ccitt(pi, 10) ^ DMR_CRC_MASK_PI;
	pi[10] = (uint8_t)(crc >> 8); pi[11] = (uint8_t)crc;
	CHECK(!dmr_aes_pi(&rx, &k, pi, 91));
	dmr_aes_make_pi(&tx, 91, pi);
	CHECK(dmr_aes_pi(&rx, &k, pi, 91));
	for (unsigned sf = 0; sf < 3; sf++) {
		for (unsigned i = 0; i < 6; i++) {
			dmr_burst_t b;
			voice_burst(&tx, i, &b);
			if (sf == 0) {
				static const uint8_t offset[18] = { 0, 7, 14, 21, 28, 35, 42, 49, 56, 63, 70, 77, 84, 91, 98, 105, 112, 119 };
				for (unsigned f = 0; f < 3; f++) {
					uint8_t bits[49];
					ambeFecDecode(b.payload + 9 * f, bits, NULL);
					for (unsigned bit = 0; bit < 45; bit++)
						CHECK(bits[bit] == ((expected[offset[3 * i + f] + bit / 8] >> (7 - bit % 8)) & 1));
				}
			}
			CHECK(dmr_aes_receive(&rx, &k, &b, 1 + 12 * sf + 2 * i, 60 * (6 * sf + i), out));
			// Signaling replaces four low-priority C3 parameter bits. All other bits must decrypt exactly.
			for (unsigned f = 0; f < 3; f++) {
				uint8_t decoded[49];
				ambeFecDecode(out + 9 * f, decoded, NULL);
				for (unsigned bit = 0; bit < 45; bit++) CHECK(decoded[bit] == 0);
			}
		}
		dmr_aes_advance(&tx);
	}
	// Complete in-band signaling can acquire the next superframe without its PI.
	dmr_aes_reset(&rx);
	for (unsigned i = 0; i < 6; i++) {
		dmr_burst_t b;
		voice_burst(&tx, i, &b);
		CHECK(!dmr_aes_receive(&rx, &k, &b, 1 + 2 * i, 60 * i, out));
	}
	dmr_aes_advance(&tx);
	dmr_burst_t b;
	voice_burst(&tx, 0, &b);
	CHECK(dmr_aes_receive(&rx, &k, &b, 13, 360, out));
	CHECK(memcmp(rx.stream, tx.stream, 126) == 0);
	// No announcement, missing fragments, or a missing key cannot establish late-entry decryption.
	for (unsigned scenario = 0; scenario < 3; scenario++) {
		dmr_aes_keys_t missing = k;
		if (scenario == 2) missing.valid = 0;
		dmr_aes_reset(&rx);
		for (unsigned i = 0; i < 6; i++) {
			if (scenario == 1 && i == 2) continue;
			voice_burst(&tx, i, &b);
			if (scenario == 0) b.emb_valid = false;
			CHECK(!dmr_aes_receive(&rx, &missing, &b, 1 + 2 * i, 60 * i, out));
		}
		CHECK(!rx.ready && !rx.next_valid);
	}
	// An entire missing superframe is advanced from the modem's slot count.
	CHECK(dmr_aes_begin(&tx, &k, 1, 0x12345678));
	CHECK(dmr_aes_begin(&rx, &k, 1, 0x12345678));
	voice_burst(&tx, 0, &b);
	CHECK(dmr_aes_receive(&rx, &k, &b, 1, 0, out));
	dmr_aes_advance(&tx);
	voice_burst(&tx, 1, &b);
	CHECK(dmr_aes_receive(&rx, &k, &b, 15, 420, out));
	CHECK(memcmp(tx.stream, rx.stream, 126) == 0);
	// A broken slot/index relation must mute, not guess the cipher position.
	voice_burst(&tx, 2, &b);
	CHECK(!dmr_aes_receive(&rx, &k, &b, 20, 480, out));
	CHECK(!rx.ready);
	b.len = 2;
	CHECK(!dmr_aes_receive(&rx, &k, &b, 21, 540, out));
	// Signaling tolerates three Golay errors; four in one word are detected.
	for (unsigned errors = 3; errors <= 4; errors++) {
		dmr_aes_reset(&rx);
		for (unsigned i = 0; i < 6; i++) {
			voice_burst(&tx, i, &b);
			if (i == 0) {
				static const uint8_t positions[4] = { 71, 67, 63, 59 };
				for (unsigned j = 0; j < errors; j++) b.payload[positions[j] / 8] ^= (uint8_t)(0x80u >> (positions[j] % 8));
			}
			CHECK(!dmr_aes_receive(&rx, &k, &b, 1 + 2 * i, 60 * i, out));
		}
		CHECK(rx.next_valid == (errors == 3));
	}
	// A changed on-air key ID must not continue decryption with the old key.
	CHECK(dmr_aes_begin(&tx, &k, 1, 0x12345678));
	CHECK(dmr_aes_begin(&rx, &k, 1, 0x12345678));
	for (unsigned i = 0; i < 6; i++) {
		voice_burst(&tx, i, &b);
		if (i == 5) {
			uint32_t other = dmr_aes_announcement(239);
			for (unsigned j = 0; j < 4; j++) b.emb[j] = (uint8_t)(other >> (24 - 8 * j));
		}
		CHECK(dmr_aes_receive(&rx, &k, &b, 1 + 2 * i, 60 * i, out));
	}
	CHECK(!rx.ready && !rx.key_valid);
	dmr_aes_advance(&tx);
	voice_burst(&tx, 0, &b);
	CHECK(!dmr_aes_receive(&rx, &k, &b, 13, 360, out));
	CHECK(!dmr_aes_begin(&rx, &k, 2, 1));
	CHECK(dmr_aes_begin(&rx, &k, 239, 1));
	dmr_aes_reset(&rx);
	CHECK(!rx.ready && !rx.key_valid && rx.round[0] == 0);
}

static void test_fuzz(void)
{
	fuzz_t f;
	fuzz_init(&f, 0xAE5256);
	dmr_aes_keys_t k = keys();
	dmr_aes_t tx, rx;
	CHECK(dmr_aes_begin(&tx, &k, 1, 0x12345678));
	for (long i = 0; i < fuzz_iters(1000); i++) {
		uint8_t pi[12];
		dmr_aes_make_pi(&tx, 91, pi);
		for (unsigned j = 0; j < 1 + fuzz_below(&f, 4); j++) pi[fuzz_below(&f, 12)] ^= (uint8_t)fuzz_next(&f);
		dmr_aes_reset(&rx);
		bool accepted = dmr_aes_pi(&rx, &k, pi, 91);
		CHECK(!accepted || (rx.ready && rx.key_valid && (rx.id == 1 || rx.id == 239)));
		dmr_burst_t b;
		voice_burst(&tx, fuzz_below(&f, 6), &b);
		for (unsigned j = 0; j < 27; j++) b.payload[j] ^= (uint8_t)fuzz_next(&f);
		b.voice_index = (uint8_t)fuzz_next(&f);
		b.len = (uint8_t)fuzz_next(&f);
		b.emb_valid = true;
		for (unsigned j = 0; j < 4; j++) b.emb[j] = (uint8_t)fuzz_next(&f);
		uint8_t out[27];
		(void)dmr_aes_receive(&rx, &k, &b, (uint32_t)i, (uint32_t)i * 30, out);
		CHECK(rx.mask <= 63);
	}
}

int main(void)
{
	test_nist();
	test_protocol();
	test_fuzz();
	printf("# dmrAesTest %s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
	return failures ? 1 : 0;
}
