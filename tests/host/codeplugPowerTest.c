// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The open-firmware power byte (qdmr's 10-step scale) on radios with POWER_COUNT levels: decode, and encode back
#include <stdio.h>
#include <string.h>
#include "codeplug/codeplug.h"
#include "board.h"

static int failures;

void storage_lock(void) {}
void storage_unlock(void) {}

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static band_t bands[] = { { 400000000, 480000000, true } };

const board_t board = { .name = "test", .bands = bands, .band_count = 1, .power_count = POWER_COUNT };

static uint8_t decode(uint8_t byte)
{
	uint8_t r[CODEPLUG_OPENFW_CHANNEL_SIZE];
	channel_t ch;
	memset(r, 0, sizeof(r));
	channel_t seed = { .rx_hz = 446075000, .tx_hz = 446075000, .power = 0 };
	codeplug_openfw_encode(&seed, r);
	r[0x19] = byte;
	CHECK(codeplug_openfw_decode(r, &ch));
	return ch.power;
}

int main(void)
{
	CHECK(decode(0) == CODEPLUG_POWER_DEFAULT);
#if POWER_COUNT == 2
	// qdmr: 1 min, 3 low, 6 mid, 9 high, 10 max
	CHECK(decode(1) == 0 && decode(3) == 0 && decode(5) == 0);
	CHECK(decode(6) == 1 && decode(9) == 1 && decode(10) == 1);
#elif POWER_COUNT >= 9
	CHECK(decode(1) == 0 && decode(3) == 2 && decode(9) == 8);
	CHECK(decode(10) == POWER_COUNT - 1u && decode(200) == POWER_COUNT - 1u);
#endif
	// Every level encodes to a byte that decodes to the same level
	for (uint8_t level = 0; level < POWER_COUNT; level++) {
		uint8_t r[CODEPLUG_OPENFW_CHANNEL_SIZE];
		channel_t in = { .rx_hz = 446075000, .tx_hz = 446075000, .power = level }, out;
		memset(r, 0, sizeof(r));
		codeplug_openfw_encode(&in, r);
		CHECK(r[0x19] >= 1 && r[0x19] <= 10 + POWER_COUNT);
		CHECK(codeplug_openfw_decode(r, &out) && out.power == level);
	}
	printf(failures ? "# codeplugPowerTest FAILED (%d)\n" : "# codeplugPowerTest PASSED\n", failures);
	return failures ? 1 : 0;
}
