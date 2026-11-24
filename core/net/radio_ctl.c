// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Zone and channel control (docs/spec/tcp-services.md 8): the requests "select?zone=Z[&pos=P]" and "step?channel=N" /
// "step?zone=N" as /ws (webctl.c) receives them, apart from the WebSocket so a carrier is easy to add. They become
// the radio core's own requests (radio_req_zone, radio_req_zone_pos, radio_req_channel_step), so the UI's refusals
// apply; a knob's quick turns add up and go out at most every REQ_GAP_MS. Idle, it costs the radio nothing: the
// radio's view is read only for a request or a pending step.
#include "config.h"
#if CONFIG_NET_WEBCTL	// the whole file
#include <string.h>
#include "net/net_svc.h"
#include "codeplug/codeplug.h"
#include "input/input.h"

#define STEP_MAX	100	// one request's step, either way
#define REQ_GAP_MS	20u	// radio requests at most this often (the radio core's queue holds 16), as rigctld's
#define ZONE_HOLD_MS	1000u	// a zone step counts from the last zone asked for this long (the view lags the request)

static struct {
	int32_t channel_steps;	// channel steps asked for and not yet passed on (a knob's turns add up)
	uint16_t zone_to, pos_to;
	bool zone_due;		// a zone (and maybe a position) to go to
	bool pos_due;
	bool zone_asked;
	bool vfo_left;
	uint32_t last_req_ms;
	uint32_t zone_asked_ms;	// when zone_to was last asked for (zone steps count from it for ZONE_HOLD_MS)
	uint32_t vfo_left_ms;	// when a step asked to leave the VFO (the view shows it a little later)
} pend;

// key=<integer> in a query ("zone=3&pos=0"); false when absent or not a number
static bool query_int(const char *q, const char *key, int32_t *out)
{
	size_t kn = strlen(key);
	for (const char *p = q; p != NULL && *p != '\0'; p = strchr(p, '&'), p = (p != NULL) ? p + 1 : NULL) {
		if (strncmp(p, key, kn) != 0 || p[kn] != '=') {
			continue;
		}
		const char *v = p + kn + 1;
		bool neg = *v == '-';
		v += (*v == '-' || *v == '+') ? 1 : 0;
		int32_t n = 0;
		int digits = 0;
		for (; *v >= '0' && *v <= '9' && digits < 6; v++, digits++) {
			n = n * 10 + (*v - '0');
		}
		if (digits == 0 || (*v != '\0' && *v != '&')) {
			return false;
		}
		*out = neg ? -n : n;
		return true;
	}
	return false;
}

// As radio_get_zone_pos: not transmitting, not a hotspot
static bool can(const radio_view_t *v)
{
	return v->state != RS_TX && !v->hotspot;
}

static void apply(uint32_t now)
{
	if ((!pend.zone_due && pend.channel_steps == 0) || (now - pend.last_req_ms) < REQ_GAP_MS) {
		return;	// nothing to pass on: the radio's view isn't read
	}
	const radio_view_t *v = net_radio_view();
	if (pend.zone_due) {
		if (v->vfo) {
			radio_req_vfo(false);
		}
		if (pend.zone_to != v->zone_index) {
			radio_req_zone(pend.zone_to);
		}
		if (pend.pos_due) {
			radio_req_zone_pos(pend.pos_to);
		}
		input_switch_hold(pend.zone_to);	// a channel switch (an absolute knob) leaves the host's choice
		pend.zone_due = pend.pos_due = false;
	} else {
		int32_t n = pend.channel_steps;
		// From the VFO: back to the channel it came from, which is the first step (once: the view lags the request)
		if (v->vfo && !(pend.vfo_left && (now - pend.vfo_left_ms) < ZONE_HOLD_MS)) {
			radio_req_vfo(false);
			pend.vfo_left = true;
			pend.vfo_left_ms = now;
			n += (n > 0) ? -1 : 1;
		}
		input_switch_hold(v->zone_index);
		if (n != 0) {
			radio_req_channel_step((int8_t)((n > 127) ? 127 : (n < -127) ? -127 : n));
		}
		pend.channel_steps = 0;
	}
	pend.last_req_ms = now;
}

// To zone z, its first channel (pos < 0) or position pos, leaving the VFO. A jump replaces steps not yet passed on
static void select_zone(uint16_t z, int32_t pos, uint32_t now)
{
	pend.zone_due = true;
	pend.zone_to = z;
	pend.pos_due = pos >= 0;
	pend.pos_to = (uint16_t)((pos >= 0) ? pos : 0);
	pend.channel_steps = 0;
	pend.zone_asked = true;
	pend.zone_asked_ms = now;
	apply(now);
}

// n zones (zones) or channels on, negative back, wrapping; steps add up. False: no zones
static bool step(bool zones, int32_t n, uint32_t now, const radio_view_t *v)
{
	if (zones) {
		uint16_t count = codeplug_zone_count();
		if (count == 0) {
			return false;
		}
		// from the zone last asked for while the view may not show it yet, so quick turns add up
		uint16_t base = (pend.zone_asked && (now - pend.zone_asked_ms) < ZONE_HOLD_MS) ? pend.zone_to : v->zone_index;
		int32_t to = ((int32_t)base + n) % (int32_t)count;
		pend.zone_due = true;
		pend.zone_to = (uint16_t)((to < 0) ? to + count : to);
		pend.pos_due = false;
		pend.channel_steps = 0;
		pend.zone_asked = true;
		pend.zone_asked_ms = now;
	} else {
		pend.channel_steps += n;
	}
	apply(now);
	return true;
}

// "select", "select?zone=Z[&pos=P]", "step?channel=N" or "step?zone=N": 0, 400 (malformed) or 409 (refused now)
int radio_ctl_request(const char *r, uint32_t now, const radio_view_t *v)
{
	bool sel = strncmp(r, "select", 6) == 0;
	const char *q = r + (sel ? 6 : 4);
	if (!sel && strncmp(r, "step?", 5) != 0) {
		return 400;
	}
	if (*q == '\0') {
		return sel ? 0 : 400;	// where the radio is: the caller's answer says
	}
	if (*q++ != '?') {
		return 400;
	}
	int32_t z, p = -1, n;
	if (sel) {
		if (!query_int(q, "zone", &z) || z < 0 || z >= codeplug_zone_count() ||
		    (strstr(q, "pos=") != NULL && !query_int(q, "pos", &p)) || p < -1 ||
		    (p >= 0 && z == v->zone_index && !v->vfo && p >= v->zone_size)) {
			return 400;	// another zone's size isn't read (no zone in RAM): the radio ignores a position past its end
		}
		if (!can(v)) {
			return 409;
		}
		select_zone((uint16_t)z, p, now);
		return 0;
	}
	bool zones = strncmp(q, "zone=", 5) == 0;
	if (!query_int(q, zones ? "zone" : "channel", &n) || n == 0 || n > STEP_MAX || n < -STEP_MAX) {
		return 400;
	}
	if (!can(v)) {
		return 409;
	}
	return step(zones, n, now, v) ? 0 : 409;
}

void radio_ctl_poll(uint32_t now)
{
	apply(now);
}

#endif	// CONFIG_NET_WEBCTL
