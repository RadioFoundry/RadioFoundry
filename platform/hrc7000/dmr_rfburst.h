// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// HR_C7000 DMR transmit where every burst runs a radio's whole RF key-up and key-down (dmr_rfburst.c): the scheme of
// the radios whose modem modulates the reference oscillator (hardware-db25d.md 4.5, hardware-rt10.md 7). The modem's
// RF TX interrupt, raised the RF TX lead before our burst, wakes a task that runs the board's TX-on sequence; the end
// of the burst (the slot engine's tick or the RF RX interrupt after it) wakes it for the TX-off sequence. The
// sequences take a few milliseconds with waits in them, so they run in a task of their own at the highest priority,
// as the original firmware's "rfint" task does; the interrupts only wake it.
//
// The board gives the two sequences in its hrc_dmr_board_t (burst); the glue starts the task and passes it every
// switch. The burst timing is in dmr_rfburst_timing.h, plain arithmetic for the host tests.
//
// A split duplex hotspot's base-station outbound is continuous: keyed at its first RF TX interrupt and dropped only when
// it stops. The glue says so (continuous), and the task then runs the TX on once and the TX off at once at the end,
// with no hold and no RF RX interrupt ending it.
#ifndef HRC_DMR_RFBURST_H
#define HRC_DMR_RFBURST_H

#include <stdint.h>
#include <stdbool.h>
#include "config.h"
#include "modem/hrc7000/hrc7000.h"
#include "dmr_rfburst_timing.h"

// The sequences' waits, on the board timer (TIMER_BOARD) while the RF task sleeps; short ones spin. wait(us): us
// microseconds from now. at(us): until us after the burst's RF TX interrupt, at once if that is past, so a late wake
// of the task doesn't make a step late
typedef void (*hrc_rfburst_wait_fn)(uint32_t us);

// A board gives only its chip steps; the timing is the task's. The hold until the burst's last symbol (none for an
// aborted transmission or at the end of a continuous outbound) comes before off, from the modem's rf_tx_lead
typedef struct {
	// One burst's TX on, from the RF task: the RF to the TX frequency and the PA up
	void (*on)(hrc_rfburst_wait_fn at, hrc_rfburst_wait_fn wait);
	// The burst's TX off, from the RF task: the PA down and the RF back to receive
	void (*off)(void);
} hrc_rfburst_board_t;

typedef struct {
	uint32_t bursts;	// TX-on sequences run
	uint32_t ends;		// TX-off sequences run
	uint32_t rfrx_ints;	// modem RF RX interrupts seen
	uint32_t late;		// TX-offs whose ramp down started after their point
	uint32_t last_late_us;
	char last_by;		// what woke the last TX-off: 'R' RF RX interrupt, 'T' the engine (slot tick), 'A' abort,
				// 'B' the end of a continuous (base-station) outbound
	// Since the last arm, in us: the RF TX interrupt to the task running the TX-on (largest), the interrupt to the end
	// of the TX-on, the PA up (smallest, largest), and how far an RF TX interrupt was off 60 ms after the one before
	// (largest, before the correction: a late interrupt, as when the modem's interrupts were masked)
	uint32_t wake_max_us;
	uint32_t up_min_us;
	uint32_t up_max_us;
	uint32_t grid_max_us;
	uint32_t regridded;	// bursts whose RF TX interrupt came late and were timed from the grid instead
	uint32_t first_retimed_us;	// how much earlier than its RF TX interrupt the first burst was timed (0: as stamped)
} hrc_rfburst_stats_t;

#if CONFIG_DMR
// Once, from hrc_dmr_board_init for a board with burst sequences: the RF task, the board timer and the RF RX interrupt
void hrc_rfburst_init(hrc7000_t *modem, const hrc_rfburst_board_t *board);
// From the glue's RF switch (modem interrupts): wakes the RF task for our burst's TX on, or for its TX off. continuous:
// a base-station outbound (above); tick_us: hrc_micros() at the slot engine's last tick, the first burst's reference.
// The TX on does nothing while disarmed
void hrc_rfburst_switch(bool tx, bool continuous, uint32_t tick_us);
// Arm once the board's TX plan is set (task level): the bursts' RF TX interrupts then key the RF. Disarm waits until
// the RF task has finished the burst it is in and left the RF receiving
void hrc_rfburst_arm(void);
void hrc_rfburst_disarm(void);
bool hrc_rfburst_armed(void);
void hrc_rfburst_stats(hrc_rfburst_stats_t *st);
#endif

#endif
