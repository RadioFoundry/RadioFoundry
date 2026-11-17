// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Air interface bench access on the debug link (CONFIG_AIR_RAW; docs/spec/debuglink.md 3.28): the board's receive tap
// (board.baseband) as raw samples, and a session on its provider (board.air) with a profile the host sends, its
// receive chunks and a transmission of the host's symbols. Generic over any board with either.
//
// The radio core owns the transmitter. It registers how the bench takes the radio (debuglink_air_core_t); without
// that, the tap and receive still open on a radio idle in DMR, and transmit is unavailable.
#ifndef DEBUGLINK_AIR_H
#define DEBUGLINK_AIR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "config.h"

// What the radio core gives the bench. None may block: each posts to the radio task and returns
typedef struct {
	// Stop the DMR call stack and leave the radio receiving in DMR on the channel's frequency, so board.air or
	// board.baseband may open. True once that is so (the bench asks again until then); false when the radio can't now
	bool (*enter)(void);
	// Back to normal operation (after close)
	void (*leave)(void);
	// on: the core's transmit checks (inhibit, test TX limit, band, battery), the transmitter keyed on the channel's TX
	// frequency at the lowest power level, then board.air->tx_begin; tx_off after max_ms at most, and at the core's
	// time-out. False when refused. off: tx_off (at AIR_EV_TX_DONE, or to end it early)
	bool (*tx)(bool on, uint16_t max_ms);
} debuglink_air_core_t;

#if CONFIG_AIR_RAW
void debuglink_air_set_core(const debuglink_air_core_t *core);
// DL_CMD_AIR
size_t debuglink_cmd_air(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap);
#else
static inline void debuglink_air_set_core(const debuglink_air_core_t *core) { (void)core; }
#endif

#endif
