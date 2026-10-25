// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The radio core's side of the hotspot: hotspot_radio_ops_t (core/hotspot/hotspot.h), called from the host link
// task. No RTOS here (the host tests run it with the real state machine): the radio task gives it a way to post
// requests, publishes the state machine's hotspot state after every pass and runs the orphan watch.
//
// Every op only posts a request and reads what the radio task last published, so the host link's MMDVM replies are
// never held up by the radio core (docs/testing/mmdvm-audit.md A-15).
#ifndef RADIO_HOTSPOT_H
#define RADIO_HOTSPOT_H

#include <stdint.h>
#include <stdbool.h>
#include "radio/radio_sm.h"
#include "hotspot/hotspot.h"

// Queue a request for the radio task. must: the request may not be lost (wait a little for queue space)
typedef void (*radio_hs_post_fn)(const radio_req_t *req, bool must);

// The hotspot engine gave up before it took the modem (or its leave was lost): back to the channel after this long
#define RADIO_HS_ORPHAN_MS	3000

void radio_hs_init(radio_hs_post_fn post);
// The ops to register with hotspot_set_radio()
extern const hotspot_radio_ops_t radio_hs_ops;
// Radio task, after every pass: what the ops read (hotspot mode, carrier, RSSI)
void radio_hs_publish(const radio_sm_t *sm);
// Radio task, every poll: in hotspot mode with no hotspot running for RADIO_HS_ORPHAN_MS, back to the channel
void radio_hs_watch(radio_sm_t *sm, uint32_t now_ms);
// Bumped when the hotspot engine reports a change
uint32_t radio_hs_serial(void);

// --- CW ID (SEND_CWID): the cw_id op copies the callsign here and posts RREQ_HS_CWID; the radio task keys it in Morse
// on FM (radio_sm.c, core/radio/morse.c) and reports back. cw_busy is true from the op until the radio task is done ---
typedef struct {
	uint32_t requests;	// cw_id ops taken
	uint32_t sent;		// IDs keyed to the end
	uint32_t refused;	// the radio task couldn't key (not in hotspot mode, no TX frequency, no FM tone, busy)
	uint32_t last_ms;	// the last ID's time on air (carrier up to down)
	uint8_t wpm;		// the speed of the last ID
	bool active;		// keying now (or about to)
} radio_hs_cw_stats_t;
// Radio task: the callsign the last cw_id op left (len bytes, not NUL terminated); 0 when none is waiting
uint8_t radio_hs_cw_take(char *out, uint8_t cap);
// Radio task: keying started (wpm), ended (sent: to the end; ms on air), or refused
void radio_hs_cw_started(uint8_t wpm);
void radio_hs_cw_ended(bool sent, uint32_t ms);
void radio_hs_cw_get_stats(radio_hs_cw_stats_t *out);

#endif
