// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The split duplex extension of the MMDVM hotspot (CONFIG_HOTSPOT_SPLIT, docs/spec/split-duplex.md): two radios act as
// one duplex repeater, the TX half sending the continuous BS outbound (core/dmr/bsOutbound.c on the modem's bs_*
// operations), the RX half receiving both inbound slots and labeling them from the TX half's slot clock, relayed by
// the host (core/dmr/slotEpoch.c). A role starts only with the vendor command 0xD6 SET_ROLE; until then, and after the
// role ends (SET_ROLE 0, the end of the session, the role lease), the radio is the simplex hotspot (or the SFR).
// Registered as mmdvm.c's priority extension (command 0xD6, the version suffix " SPLIT1", the standard commands while
// in a role) and as hotspot.c's priority other mode.
#ifndef ODR_HOTSPOT_SPLIT_H
#define ODR_HOTSPOT_SPLIT_H

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

#define SPLIT_INFO_LEN		10
#define SPLIT_STATUS_LEN	32

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
} hotspot_split_view_t;
void hotspot_split_get_view(hotspot_split_view_t *v);

#endif
