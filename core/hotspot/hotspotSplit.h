// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The split duplex extension of the MMDVM hotspot (CONFIG_HOTSPOT_SPLIT, docs/spec/split-duplex.md): two radios act as
// one duplex repeater, the TX half sending the continuous BS outbound (core/dmr/bsOutbound.c on the modem's bs_*
// operations), the RX half receiving both inbound slots and labeling them from the TX half's slot clock, relayed by
// the host (core/dmr/slotEpoch.c). A role starts only with the vendor command 0xD6 SET_ROLE; until then, and after the
// role ends (SET_ROLE 0, the end of the session, the role lease), the radio is the simplex hotspot (or the SFR).
// Registered as mmdvm.c's priority extension (command 0xD6, the version suffix " SPLIT1", the standard commands while
// in a role) and as hotspot.c's priority other mode.
#ifndef RFOUNDRY_HOTSPOT_SPLIT_H
#define RFOUNDRY_HOTSPOT_SPLIT_H

#include <stdbool.h>
#include <stdint.h>
#include "dmr/bsOutbound.h"
#include "dmr/slotEpoch.h"

#define SPLIT_VERSION		1

// Command 0xD6 sub-commands (spec 3)
#define SPLIT_SUB_INFO		0x00
#define SPLIT_SUB_SET_ROLE	0x01
#define SPLIT_SUB_TIME		0x02
#define SPLIT_SUB_GET_EPOCH	0x03
#define SPLIT_SUB_SET_EPOCH	0x04
#define SPLIT_SUB_SET_AT	0x05
#define SPLIT_SUB_STATUS	0x06
#define SPLIT_SUB_ALIGN		0x07
#define SPLIT_SUB_TX_SCHED	0x08
// Events: frames the radio sends without a request (spec 3.10), sub 0x80 and up
#define SPLIT_EV_RX_META	0x89

#define SPLIT_ROLE_NORMAL	0
#define SPLIT_ROLE_TX		1
#define SPLIT_ROLE_RX		2

#define SPLIT_RESULT_OK		0
#define SPLIT_RESULT_UNKNOWN	1
#define SPLIT_RESULT_STATE	2
#define SPLIT_RESULT_INVALID	4

// INFO / STATUS flags (spec 3.1)
#define SPLIT_FLAG_EPOCH	0x01
#define SPLIT_FLAG_KEYED	0x02
#define SPLIT_FLAG_COOLDOWN	0x04
#define SPLIT_FLAG_CLOCK	0x08

#define SPLIT_INFO_LEN		16
#define SPLIT_STATUS_LEN	48

// INFO capabilities (spec 3.1 bytes 10-11)
#define SPLIT_CAP_SCHED		0x0001u
#define SPLIT_CAP_ALIGN		0x0002u
#define SPLIT_CAP_SLEW		0x0004u
#define SPLIT_CAP_RX_META	0x0008u
#define SPLIT_CAP_TRIM		0x0010u
#define SPLIT_CAP_PPS		0x0020u

// SET_ROLE byte 6
#define SPLIT_STREAM_BUFFER	0
#define SPLIT_STREAM_SCHED	1

// SET_ROLE byte 7: options
#define SPLIT_OPT_RX_META	0x01	// RX half: an RX_META event before each burst's uplink frames (spec 3.10)

// RX_META event (spec 3.10)
#define SPLIT_META_LEN		16
#define SPLIT_META_CRC_OK	0x01
#define SPLIT_META_IN_STEP	0x02
#define SPLIT_META_PI		0x04
#define SPLIT_META_LC		0x08
#define SPLIT_META_BS_SYNC	0x10
#define SPLIT_META_EPOCH	0x20	// labeled from a valid epoch
#define SPLIT_META_TIMED	0x40	// t_us is the burst's slot start (the slot clock ran)

// ALIGN reply / STATUS 46 flags (spec 3.8)
#define SPLIT_AL_ALIGNED	0x01
#define SPLIT_AL_SLEWING	0x02
#define SPLIT_AL_SNAP		0x04
#define SPLIT_AL_PERIOD		0x08
#define SPLIT_AL_NOT_APPLIED	0x10
#define SPLIT_AL_BASE		0x20	// STATUS only: GET_EPOCH numbers slots as the last ALIGN

#define SPLIT_ALIGNED_US	1000	// |phase| under this: aligned
#define SPLIT_SNAP_TOL_US	200	// a snap that lands further off than this is done again (the delay learned)
#define SPLIT_SNAP_TRIES	3	// restarts after the first while the clock is still off the grid

// ALIGN's slewing (spec 3.8): bytes 12-13 the most per slot, byte 14 the mode
#define SPLIT_SLEW_MAX_US	20	// the default and the most per slot, on average
#define SPLIT_SLEW_GAP		3	// boundaries between two steps at least: the last one shows in the phase first
#define SPLIT_ALIGN_SNAP	0	// mode 0: unkeyed snap, keyed slew
#define SPLIT_ALIGN_SLEW	1	// mode 1: slew always, the clock never jumps

// TX_SCHED outcomes (spec 3.9)
#define SPLIT_SCHED_QUEUED	0
#define SPLIT_SCHED_LATE	1
#define SPLIT_SCHED_EARLY	2
#define SPLIT_SCHED_REPLACED	3
#define SPLIT_SCHED_FULL	4

#define SPLIT_SCHED_N		32	// scheduled bursts held (the horizon, INFO byte 13)
#define SPLIT_SCHED_COMMIT_US	45000	// a slot is handed to the driver when its boundary is this close
#define SPLIT_SCHED_KEY_US	150000	// a queued burst keys up when its slot's boundary is this close
#define SPLIT_SCHED_HANG_MS	1000	// a key-up from queued bursts ends this long after the last one went out

// Defaults (spec 3.2)
#define SPLIT_LEASE_S		10
#define SPLIT_POWER_CAP		64
#define SPLIT_MAX_KEY_10S	18
#define SPLIT_MAX_KEY_10S_MAX	60
#define SPLIT_COOLDOWN_S	20
#define SPLIT_HOST_SILENT_MS	2000	// TX half keyed and no frame from the host for this long: unkey
#define SPLIT_UNKEY_DRAIN_MS	1000	// DMR_START 00: unkey once drained, at most this late
#define SPLIT_BUSY_DATA_MS	360	// RX half: a slot is busy this long after a data burst
#define SPLIT_CD_HOLD_MS	200	// RX half: carrier detect after the last burst
#define SPLIT_SPACE_MARGIN	2

// After hotspot_init() and mmdvm_init() (and after hotspot_sfr_init(), whose registrations it keeps)
void hotspot_split_init(void);

// The BS framing experiment (spec 10.1, debug link SPLIT): the variant and raw register pairs the next key-up uses
void hotspot_split_set_variant(uint8_t variant, const uint8_t regs[][2], uint8_t nregs);

// For the debug link's view and the tests (a snapshot)
typedef struct {
	uint8_t role;
	uint8_t flags;			// SPLIT_FLAG_xxx
	uint8_t roles_offered;		// INFO byte 1
	uint8_t variant;
	uint8_t cach_method;		// INFO byte 4
	uint8_t at_host, at_on_air;	// TX: the host's AT mask now, the AT bits of the last CACH per slot
	uint8_t inbound_busy;		// RX: STATUS byte 2
	uint8_t buffered[2];		// TX: bursts buffered per slot
	uint16_t underruns[2];
	uint16_t idle, cach, silence, closed;
	uint16_t slc_taken, slc_bad;
	uint16_t labeled[2], unlabeled;
	int32_t residual_us, track_us;	// RX: last phase residual, the anchor's correction
	uint32_t epoch_us, period_ns;	// RX: the epoch
	uint16_t epoch_age_s;		// RX: 65535 never
	uint16_t keyed_s;		// TX: seconds in this key-up
	uint16_t lease_expiries;
	uint32_t time_requests;
	uint8_t stream;			// TX: SPLIT_STREAM_xxx
	uint8_t align_flags;		// TX: STATUS 46
	int32_t phase_us;		// TX: STATUS 42-45
	int32_t snap_adj_us;		// TX: the learned restart delay
	uint32_t cbase;			// TX: ALIGN counter = driver counter + cbase
	uint16_t sch_sent, sch_late, sch_refused, sch_replaced, sch_cool;
	uint16_t slew_steps;		// TX: steps the driver took while slewing
	int32_t slew_us;		// TX: their sum (µs, + longer slots)
	uint16_t meta_sent;		// RX: RX_META events
} hotspot_split_view_t;
void hotspot_split_get_view(hotspot_split_view_t *v);

#endif
