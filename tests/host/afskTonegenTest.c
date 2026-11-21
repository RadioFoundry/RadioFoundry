// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// AFSK through an RF chip's tone generator (drivers/audio/afsk_tonegen): the tones per symbol, the timer, refusals
#include <stdio.h>
#include <string.h>
#include "audio/afsk_tonegen/afsk_tonegen.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static bool keyed = true, began, ended, running;
static uint32_t period;
static void (*tick)(void);
static uint16_t log_hz[64];
static unsigned writes;

static bool begin(uint16_t hz, uint8_t level)
{
	(void)level;
	if (!keyed) {
		return false;
	}
	began = true;
	writes = 0;
	log_hz[writes++] = hz;
	return true;
}
static void tone(uint16_t hz) { log_hz[writes++] = hz; }
static void end(void) { ended = true; }
static void timer_start(uint32_t us, void (*fn)(void)) { period = us; tick = fn; running = true; }
static void timer_stop(void) { running = false; }

static const afsk_tonegen_board_t hw = { begin, tone, end, timer_start, timer_stop };

int main(void)
{
	static const uint8_t bits[] = { 0x0D };		// 1 0 1 1 0 0 0 0, LSB first
	CHECK(!afsk_tonegen.tx_start(bits, 8, 1200, 1200, 2200, 50));	// not attached
	afsk_tonegen_attach(&hw);
	CHECK(afsk_tonegen.tx_start(bits, 8, 1200, 1200, 2200, 50));
	CHECK(began && running && period == 833 && afsk_tonegen.tx_busy());
	CHECK(!afsk_tonegen.tx_start(bits, 8, 1200, 1200, 2200, 50));	// already sending
	for (int i = 0; i < 8 && running; i++) {
		tick();
	}
	CHECK(!running && !afsk_tonegen.tx_busy());
	// One write per change only: 1200, 2200, 1200, (1200), 2200, ...
	CHECK(writes == 4 && log_hz[0] == 1200 && log_hz[1] == 2200 && log_hz[2] == 1200 && log_hz[3] == 2200);
	afsk_tonegen.tx_stop();
	CHECK(ended);
	keyed = false;
	CHECK(!afsk_tonegen.tx_start(bits, 8, 1200, 1200, 2200, 50) && !afsk_tonegen.tx_busy() && !running);
	if (fails) {
		printf("afskTonegenTest: %d failures\n", fails);
		return 1;
	}
	printf("afskTonegenTest: all passed\n");
	return 0;
}
