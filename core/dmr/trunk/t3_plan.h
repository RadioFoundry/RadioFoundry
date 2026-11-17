// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// DMR Tier III channel plans (ETSI TS 102 361-4 Annex C): a logical physical channel number (LPCN, 1..4094) to the
// MS's receive and transmit frequencies. Three sources, looked up in this order:
//   1. learned: C_BCAST Chan_Freq (BC_AP) and grant CG_AP blocks heard on the selected system, kept in RAM only and
//      dropped when the network (model or NET) changes (when the system's record allows learning)
//   2. table: the programmed per-channel list (Annex C.1.1.3, the flexible plan)
//   3. fixed: the programmed formula (Annex C.1.1.2)
// A grant whose channel none of these knows can't be followed; the engine counts it (t3.unmapped, t3.last_unmapped).
#ifndef RFOUNDRY_T3_PLAN_H
#define RFOUNDRY_T3_PLAN_H

#include <stdbool.h>
#include <stdint.h>
#include "dmr/trunk/t3_pdu.h"

#define T3_LEARN_MAX	64
#define T3_CC_ANY	0xFF	// color code: use the control channel's

// lcn and cc first: 12 bytes, not 16 (64 learned channels, and every table and call copy)
typedef struct {
	uint16_t lcn;
	uint8_t cc;		// color code, T3_CC_ANY = the control channel's
	uint32_t rx_hz;		// the MS receives here
	uint32_t tx_hz;		// the MS transmits here
} t3_chan_t;
_Static_assert(sizeof(t3_chan_t) == 12, "t3_chan_t packs into 12 bytes");

// Annex C.1.1.2: fMS_Tx = base + (CHAN - first) x separation; fMS_Rx = fMS_Tx -/+ split
typedef struct {
	uint32_t base_hz;	// MS transmit frequency of channel `first` (0 = no fixed plan)
	uint16_t first;		// normally 1
	uint8_t sep;		// separation code (table C.1)
	bool tx_low;		// TXRX_SPLIT (table C.2): false MS Tx is above MS Rx, true below
	uint32_t split_hz;	// DUPLEX_SPLIT in Hz (0 = simplex pairs)
} t3_fixed_plan_t;

typedef struct {
	t3_fixed_plan_t fixed;
	const t3_chan_t *table;	// sorted or not; searched linearly
	uint16_t ntable;
	bool learn;		// accept Chan_Freq / CG_AP
} t3_plan_t;

typedef struct {
	t3_chan_t e[T3_LEARN_MAX];
	uint8_t n;
	uint8_t next;		// the entry a new channel replaces when full
} t3_learned_t;

// Separation in Hz for a table C.1 code, 0 for a reserved code
uint32_t t3_sep_hz(uint8_t code);

bool t3_fixed_lookup(const t3_fixed_plan_t *f, uint16_t lcn, t3_chan_t *out);

void t3_learn_clear(t3_learned_t *l);
// Add or refresh a channel. Returns true when it is new or changed
bool t3_learn_add(t3_learned_t *l, const t3_cdef_t *c);

// Resolve a channel: learned (if plan->learn), table, fixed. False when nothing knows it
bool t3_plan_lookup(const t3_plan_t *plan, const t3_learned_t *l, uint16_t lcn, t3_chan_t *out);

#endif
