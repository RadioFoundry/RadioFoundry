// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// HR_C7000 DMR transmit where every burst runs a radio's whole RF key-up and key-down (dmr_rfburst.c): the scheme of
// the radios whose modem modulates the reference oscillator (hardware-db25d.md 4.5, hardware-rt10.md 7). The modem's
// RF TX interrupt, raised the RF TX lead before our burst, wakes a task that runs the board's TX-on sequence; the end
// of the burst (the slot engine's tick or the RF RX interrupt after it) wakes it for the TX-off sequence. The
// sequences take a few milliseconds with waits in them, so they run in a task of their own at the highest priority,
// as the original firmware's "rfint" task does; the interrupts only wake it.
//
// The board gives the two sequences and passes hrc_rfburst_switch to the glue as its rf_switch hook. The burst timing
// is in dmr_rfburst_timing.h, plain arithmetic for the host tests.
#ifndef HRC_DMR_RFBURST_H
#define HRC_DMR_RFBURST_H

#include <stdint.h>
#include <stdbool.h>
#include "config.h"
#include "modem/hrc7000/hrc7000.h"
#include "dmr_rfburst_timing.h"

// The sequences' waits: wait(us) sleeps the RF task for us microseconds on the board timer (TIMER_BOARD); short ones
// spin
typedef void (*hrc_rfburst_wait_fn)(uint32_t us);

typedef struct {
	// One burst's TX on, from the RF task, elapsed_us after its RF TX interrupt: the RF to the TX frequency and the
	// PA up. The board's key-up wait counts from the interrupt (less elapsed_us), so a late wake doesn't make the
	// PA late
	void (*on)(hrc_rfburst_wait_fn wait, uint32_t elapsed_us);
	// The burst's TX off, from the RF task, elapsed_us after its RF TX interrupt; now: at once, no hold (an aborted
	// transmission). Returns how late the ramp down started against its point (hrc_rfburst_late_us), in us
	uint32_t (*off)(hrc_rfburst_wait_fn wait, uint32_t elapsed_us, bool now);
} hrc_rfburst_board_t;

typedef struct {
	uint32_t bursts;	// TX-on sequences run
	uint32_t ends;		// TX-off sequences run
	uint32_t rfrx_ints;	// modem RF RX interrupts seen
	uint32_t late;		// TX-offs whose ramp down started after their point
	uint32_t last_late_us;
	char last_by;		// what woke the last TX-off: 'R' RF RX interrupt, 'T' the engine (slot tick), 'A' abort
	// Since the last arm, in us: the RF TX interrupt to the task running the TX-on (largest), the interrupt to the end
	// of the TX-on, the PA up (smallest, largest), and how far an RF TX interrupt was off 60 ms after the one before
	// (largest, before the correction: a late interrupt, as when the modem's interrupts were masked)
	uint32_t wake_max_us;
	uint32_t up_min_us;
	uint32_t up_max_us;
	uint32_t grid_max_us;
	uint32_t regridded;	// bursts whose RF TX interrupt came late and were timed from the grid instead
} hrc_rfburst_stats_t;

#if CONFIG_DMR
// Once, from the radio init, after hrc_dmr_board_init: the RF task, the board timer and the RF RX interrupt
void hrc_rfburst_init(hrc7000_t *modem, const hrc_rfburst_board_t *board);
// The glue's rf_switch hook (hrc_dmr_board_t): wakes the RF task for our burst's TX on, or for its TX off. Does
// nothing while disarmed
void hrc_rfburst_switch(bool tx);
// Arm once the board's TX plan is set (task level): the bursts' RF TX interrupts then key the RF. Disarm waits until
// the RF task has finished the burst it is in and left the RF receiving
void hrc_rfburst_arm(void);
void hrc_rfburst_disarm(void);
bool hrc_rfburst_armed(void);
void hrc_rfburst_stats(hrc_rfburst_stats_t *st);
#endif

#endif
