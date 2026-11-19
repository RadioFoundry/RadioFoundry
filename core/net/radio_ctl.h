// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Zone and channel control, whichever way it is asked for (docs/spec/tcp-services.md 5.2, 5.3): over HTTP
// (http_radio.c, CONFIG_NET_RADIO_API) or UDP (radio_udp.c, CONFIG_NET_RADIO_UDP). Where the radio is as JSON, a select
// or a step passed on to the radio core as a knob would, and the change notices to one subscriber. Internal to
// core/net; everything runs in the host link task.
#ifndef RFOUNDRY_RADIO_CTL_H
#define RFOUNDRY_RADIO_CTL_H

#include <stdbool.h>
#include <stdint.h>
#include "config.h"
#include "debuglink/dl_fmt.h"
#include "radio/radio.h"

#if CONFIG_NET_RADIO_CTL
#define RADIO_CTL_STEP_MAX	100	// one request's step, either way
#define RADIO_CTL_LEASE_S	120u	// a subscription ends this long after the last subscribe

// key=<integer> in a query ("zone=3&pos=0"); false when absent or not a number
bool radio_ctl_query_int(const char *q, const char *key, int32_t *out);
// Where the radio is and the channel it is on, from the view alone (no flash read): the JSON object without its
// closing brace, for the caller to add its own keys and close. channel: false leaves the keys after "number" out
void radio_ctl_where(dl_text_t *t, const radio_view_t *v, bool channel);
// The radio takes a zone or channel request now (not transmitting, not a hotspot); counts a refusal
bool radio_ctl_can(const radio_view_t *v);
// To zone z, its first channel (pos < 0) or position pos, leaving the VFO. The caller checked the range and
// radio_ctl_can
void radio_ctl_select(uint16_t z, int32_t pos, uint32_t now);
// n zones (zones) or channels on, negative back, wrapping; steps add up. The caller checked the range and
// radio_ctl_can. False: no zones
bool radio_ctl_step(bool zones, int32_t n, uint32_t now, const radio_view_t *v);
// Passes on requests held back by the rate limit, and sends the notices
void radio_ctl_poll(uint32_t now);
void radio_ctl_view(dl_text_t *t);

#if CONFIG_NET_RADIO_NOTICES
// Notices from the radio's port from to ip:to, until RADIO_CTL_LEASE_S after this call. to 0 cancels. A new
// subscriber replaces the old one and gets a notice at once
void radio_ctl_subscribe(uint32_t ip, uint16_t from, uint16_t to, uint32_t now);
// The notices sent (a reply carries it too, so a client can line them up)
uint32_t radio_ctl_seq(void);
// A reply that says where the radio is went to the subscriber (sent), so no notice repeats it; or was lost (the link
// was busy), so the next check sends one, changed or not
void radio_ctl_replied(bool sent, const radio_view_t *v, uint32_t now);
#endif
#endif

#endif
