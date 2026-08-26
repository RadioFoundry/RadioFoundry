// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
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

#endif
