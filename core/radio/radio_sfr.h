// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Standalone SFR (CONFIG_SFR, docs/spec/sfr.md 4): what a channel's SFR settings make of it. A repeater role hands the
// modem to a single frequency repeater (core/dmr/sfrRunner.c) instead of the call stack, with no host; a user role
// is the call stack's SFR user mode (dmr_call_config_t.sfr_user). The radio task calls everything here.
#ifndef RADIO_SFR_H
#define RADIO_SFR_H

#include <stdint.h>
#include <stdbool.h>
#include "codeplug/codeplug.h"
#include "hal_dmr_modem.h"
#include "dmr/sfr.h"
#include "dmr/sfrRunner.h"

// Modem events between the driver's task and the radio task (power of two): two per slot at most, taken every tick
#define RADIO_SFR_EVQ_DEPTH	8

// The channel's SFR role (sfrRole_t, SFR_ROLE_OFF without one) and its run configuration. A role needs a simplex
// channel (RX = TX, or no TX frequency for a user), else it is off. vfo: the VFO has no SFR settings
uint8_t radio_sfr_channel(const channel_t *ch, bool vfo, sfrConfig_t *cfg);

// The call stack's SFR user settings from a user role's configuration: the inbound slot (1 or 2), the sync our bursts
// carry (TS1 / TS2 with TS framing, MS otherwise: an ERDM repeater takes MS sync in), the sync of the repeat (the
// outbound slot's TS1 / TS2, BS or MS) and a second color code (ERDM's outbound one, -1 none)
void radio_sfr_user(const sfrConfig_t *cfg, uint8_t *ts, uint8_t *tx_sync, uint8_t *rx_sync, int8_t *cc2);

// Repeater role: start the runner on the modem (the board already in DMR mode on the frequency, the keying armed).
// The modem starts in DMO, every slot received, the inbound color code. False if the modem didn't start
bool radio_sfr_start(const hal_dmr_modem_t *modem, const sfrConfig_t *cfg, uint16_t beacon_s, uint8_t beacon_len_s,
		     uint32_t now_ms);
void radio_sfr_stop(void);
bool radio_sfr_running(void);
// Every radio tick: the queued modem events, then the runner's poll
void radio_sfr_poll(uint32_t now_ms);

typedef struct {
	bool on;		// the repeater role runs
	bool keyed;		// the outbound slot is being sent
	bool inbound_busy;	// an inbound call is being repeated
	uint8_t role;		// sfrRole_t
	uint8_t inbound_slot;	// 0 TS1, 1 TS2
	uint8_t framing;	// sfrFraming_t, resolved
	uint32_t evq_drops;	// modem events lost (queue full)
	sfrRunnerStats_t stats;
} radio_sfr_view_t;

void radio_sfr_get_view(radio_sfr_view_t *v);

#endif
