// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The per-burst RF task's timing (platform/hrc7000/dmr_rfburst_timing.h): the first burst of a transmission timed
// from the slot engine's tick when the modem raised its RF TX interrupt late, and what the on-grid bursts teach
#include <stdint.h>
#include <stdio.h>
#include "../../platform/hrc7000/dmr_rfburst_timing.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

#define SLOT	30000u
#define SLACK	30u

int main(void)
{
	// What a radio showed at lead 26: the later bursts' RF TX interrupts 27291-27299 us after their tick, the first
	// 27709-27716 us, 0.42 ms late
	uint32_t ref = 0;
	ref = hrc_rfburst_tick_ref(ref, 100000u + 27291u, 100000u, SLOT);
	CHECK(ref == 27291u);
	ref = hrc_rfburst_tick_ref(ref, 160000u + 27299u, 160000u, SLOT);
	CHECK(ref == 27299u);
	// A tick held up by the masking makes the time shorter: the largest stays
	ref = hrc_rfburst_tick_ref(ref, 220000u + 27299u, 220000u + 150u, SLOT);	// the tick 150 us late
	CHECK(ref == 27299u);
	// A time of a slot or more is not an interrupt after its own tick
	CHECK(hrc_rfburst_tick_ref(ref, 300000u + SLOT, 300000u, SLOT) == ref);

	// The first burst 0.42 ms late: timed at the learned point after its tick
	uint32_t tick = 5000000u;
	CHECK(hrc_rfburst_first_on(tick + 27716u, tick, ref, SLACK) == tick + 27299u);
	// Within the slack, or early: its own stamp
	CHECK(hrc_rfburst_first_on(tick + 27299u + SLACK, tick, ref, SLACK) == tick + 27299u + SLACK);
	CHECK(hrc_rfburst_first_on(tick + 27200u, tick, ref, SLACK) == tick + 27200u);
	// Later than the bound: something else (a tick from another slot), its own stamp
	CHECK(hrc_rfburst_first_on(tick + 27299u + HRC_RFBURST_FIRST_LATE_MAX_US + 1u, tick, ref, SLACK) ==
		tick + 27299u + HRC_RFBURST_FIRST_LATE_MAX_US + 1u);
	CHECK(hrc_rfburst_first_on(tick + 27299u + HRC_RFBURST_FIRST_LATE_MAX_US, tick, ref, SLACK) == tick + 27299u);
	// A tick after the stamp (the wrap of stamp - tick): its own stamp
	CHECK(hrc_rfburst_first_on(tick + 100u, tick + 5000u, ref, SLACK) == tick + 100u);
	// Nothing learned yet: its own stamp
	CHECK(hrc_rfburst_first_on(tick + 27716u, tick, 0, SLACK) == tick + 27716u);
	// Across the timer's wrap
	tick = 0xFFFFF000u;
	CHECK(hrc_rfburst_first_on(tick + 27716u, tick, ref, SLACK) == (uint32_t)(tick + 27299u));

	// The grid: a late burst is taken back to it, one on time or early is not; right after a retimed first burst its
	// own stamp starts the grid (a wrong retime is not carried on)
	CHECK(hrc_rfburst_regrid(60000u + SLACK + 1u, 60000u, SLACK, false));
	CHECK(!hrc_rfburst_regrid(60000u + SLACK, 60000u, SLACK, false));
	CHECK(!hrc_rfburst_regrid(59000u, 60000u, SLACK, false));
	CHECK(!hrc_rfburst_regrid(60000u + 420u, 60000u, SLACK, true));

	if (failures == 0) {
		printf("hrc7000RfburstTest: all passed\n");
	}
	return failures != 0;
}
