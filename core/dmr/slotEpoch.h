// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
/*
 * The split duplex RX half's slot labels (docs/spec/split-duplex.md 7): the host gives the start E of an outbound TS1
 * slot in this radio's modem clock and the outbound slot length P in this radio's clock units; a burst whose slot
 * started at t is in outbound slot n = round((t - E) / P), and the inbound slot running then has the other number:
 * TS2 for even n, TS1 for odd n (ETSI TS 102 361-1 4.5 figures 4.8 / 4.9, table 5.1).
 *
 * In-step bursts refine the anchor: the residual (t minus the predicted slot start, within +-P/2) is averaged (1/8 per
 * burst) and the anchor follows it, by at most SLOT_EPOCH_TRACK_MAX_US in all since the last epoch. That absorbs the
 * constant delays of the two chips and the drift between the host's updates; the label itself only comes from E.
 * No hardware here.
 */
#ifndef ODR_SLOT_EPOCH_H
#define ODR_SLOT_EPOCH_H

#include <stdbool.h>
#include <stdint.h>

#define SLOT_EPOCH_PERIOD_NS		30000000u
#define SLOT_EPOCH_PERIOD_MIN_NS	29990000u
#define SLOT_EPOCH_PERIOD_MAX_NS	30010000u
#define SLOT_EPOCH_MAX_AGE_MS		120000u	// an epoch older than this is not used
#define SLOT_EPOCH_TRACK_MAX_US		5000
#define SLOT_EPOCH_ADVANCE_US		60000000	// the anchor is moved forward by whole frames past this

typedef struct {
	bool valid;
	uint32_t epoch_us;	// E, modem clock (whole microseconds)
	uint32_t epoch_ns;	// ... and its fraction (0-999 ns), from moving it forward by whole frames
	uint32_t period_ns;	// P
	uint32_t set_ms;	// when the host set it
	int32_t track_us;	// the anchor's correction from in-step bursts
	int32_t avg_us;		// averaged residual (x8)
	int32_t last_residual_us;
	uint32_t sets;
} slotEpoch_t;

void slotEpochInit(slotEpoch_t *e);
// SET_EPOCH: false when period_ns is out of range (0 = the nominal 30 ms)
bool slotEpochSet(slotEpoch_t *e, uint32_t t_us, uint32_t period_ns, uint32_t now_ms);
void slotEpochClear(slotEpoch_t *e);
bool slotEpochValid(const slotEpoch_t *e, uint32_t now_ms);
// The inbound slot (1 or 2) of a burst whose slot started at t_us (modem clock); 0 without a valid epoch.
// *residual_us: where it started relative to the prediction (+-P/2)
int8_t slotEpochLabel(slotEpoch_t *e, uint32_t t_us, uint32_t now_ms, int32_t *residual_us);
// An in-step burst's residual (from slotEpochLabel): move the anchor toward it
void slotEpochTrack(slotEpoch_t *e, int32_t residual_us);

#endif
