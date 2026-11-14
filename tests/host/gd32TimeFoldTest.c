// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The GD32F30x clocks' fold (platform/gd32f30x/time_fold.h): the running microsecond and millisecond counts against a
// 64-bit reference over uneven reads, cycle counter wraps, and the 32-bit wraps of the counts themselves
#include <stdint.h>
#include <stdio.h>
#include "../../platform/gd32f30x/time_fold.h"

static int failures;

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

// Steps of the cycle counter between reads: from back-to-back reads to one just short of a whole wrap
static uint32_t step(uint32_t i)
{
	static const uint32_t steps[] = { 1u, 7u, 119u, 120u, 121u, 1000u, 119999u, 120000u, 120001u, 3333333u,
					  0x7FFFFFFFu, 0xFFFFF000u };
	return steps[i % (sizeof(steps) / sizeof(steps[0]))];
}

static void run(uint32_t per_us, uint32_t start, uint32_t reads)
{
	time_fold_t t;
	time_fold_init(&t, start);
	uint64_t total = 0;
	uint32_t now = start;
	for (uint32_t i = 0; i < reads; i++) {
		uint32_t d = step(i * 7u + 3u);
		now += d;
		total += d;
		time_fold(&t, now, per_us);
		uint64_t us = total / per_us;
		CHECK(t.us == (uint32_t)us);
		CHECK(t.ms == (uint32_t)(us / 1000u));
		CHECK(t.cyc == (uint32_t)(total % per_us));
		CHECK(t.us_ms == (uint32_t)(us % 1000u));
		if (failures > 10) {
			return;
		}
	}
}

int main(void)
{
	run(120u, 0u, 5000u);			// the radio's 120 MHz
	run(8u, 0xFFFFFF00u, 5000u);		// the 8 MHz start-up clock, the counter about to wrap
	run(120u, 0x80000000u, 200000u);	// long enough for the microsecond count to wrap (71 minutes)

	// One fold of nothing changes nothing
	time_fold_t t;
	time_fold_init(&t, 1234u);
	time_fold(&t, 1234u, 120u);
	CHECK(t.us == 0 && t.ms == 0 && t.cyc == 0);

	if (failures) {
		printf("gd32TimeFoldTest: %d failures\n", failures);
		return 1;
	}
	printf("gd32TimeFoldTest: all passed\n");
	return 0;
}
