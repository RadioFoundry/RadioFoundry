// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The MMDVM hotspot's SFR mode (CONFIG_SFR): the host turns it on with SET_CONFIG's simplex flag clear and SET_FREQ
// RX = TX (docs/spec/mmdvm-protocol.md 6, gotchas 8.12). Then the radio is a single frequency repeater on the hotspot
// frequency (core/dmr/sfrRunner.c on the hotspot's modem): what radios send on the inbound slot is repeated on the
// outbound slot and goes up to the host as DMR_DATA1 / DMR_DATA2 of the inbound slot; the host's streams for the
// outbound slot go out between the repeats. Registered as mmdvm.c's extension (version suffix, status flag 0x80, the
// space bytes, the extra status bytes, command 0xD5) and as hotspot.c's other mode.
#ifndef RFOUNDRY_HOTSPOT_SFR_H
#define RFOUNDRY_HOTSPOT_SFR_H

#include <stdbool.h>
#include <stdint.h>
#include "dmr/sfr.h"
#include "dmr/sfrRunner.h"

#define HOTSPOT_SFR_STATUS_EXTRA	108	// GET_STATUS frame offsets 13-120 (spec 6.3)

// After hotspot_init() and mmdvm_init()
void hotspot_sfr_init(void);

// The SFR settings the hotspot uses (inbound slot, framing, hang, TOT); the color code is SET_CONFIG's. Defaults:
// TS1 in, TS2 out, TS framing, 1 s hang, 180 s TOT. tot_15s 0: the default TOT
void hotspot_sfr_set_settings(const sfrChannelSettings_t *s, uint8_t tot_15s);

// The idle beacon while SFR mode runs (docs/spec/dmr-roaming.md 7): idle bursts on the outbound slot for len_s every
// interval_s when nothing is repeated; 0 = off (the default). Taken when SFR mode starts. The radio core sets it from
// its settings (rpt_beacon, roam_beacon_s, roam_beacon_len_s)
void hotspot_sfr_set_beacon(uint16_t interval_s, uint8_t len_s);

typedef struct {
	bool on;		// SFR mode active
	bool keyed;
	bool inbound_busy;
	uint8_t inbound_slot;	// 0 TS1, 1 TS2
	uint8_t framing;	// sfrFraming_t, resolved
	sfrRunnerStats_t stats;
} hotspot_sfr_view_t;
void hotspot_sfr_get_view(hotspot_sfr_view_t *v);

#endif
