// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Zone and channel control over UDP (docs/spec/tcp-services.md 5.3): the requests of /api/select and /api/step as one
// datagram each to port NET_RADIO_UDP_PORT, from the computer only (bound to the link, never the air). Every request
// is answered with where the radio is, one datagram, and makes its sender the change notices' subscriber for
// RADIO_CTL_LEASE_S. No TCP and no HTTP: a radio short of RAM can leave them out and still be controlled. Whoever is on
// the link is the radio's operator, so there is no host check. A lost step isn't sent again: the user steps again;
// where the radio is comes in every reply, every notice and the heartbeat.
#include "config.h"
#if CONFIG_NET_RADIO_UDP	// the whole file
#include <string.h>
#include "net/net_svc.h"
#include "net/radio_ctl.h"
#include "codeplug/codeplug.h"
#include "dmr/ipGateway.h"
#include "hal_time.h"

#define REQ_MAX		40	// the longest request ("select?zone=65535&pos=65535" and room)

static struct {
	uint32_t requests, bad, reply_busy;
} st;

// "select", "select?zone=Z[&pos=P]", "select?notify=0", "step?channel=N" or "step?zone=N": 0, or the error for the reply
static int request(const char *r, uint32_t now, const radio_view_t *v)
{
	bool sel = strncmp(r, "select", 6) == 0;
	const char *q = r + (sel ? 6 : 4);
	if (!sel && strncmp(r, "step?", 5) != 0) {
		return 400;
	}
	if (*q == '\0') {
		return sel ? 0 : 400;	// where the radio is
	}
	if (*q++ != '?') {
		return 400;
	}
	int32_t z, p = -1, n;
	if (sel) {
		if (strcmp(q, "notify=0") == 0) {
			return 0;	// the caller cancels the notices
		}
		if (!radio_ctl_query_int(q, "zone", &z) || z < 0 || z >= codeplug_zone_count() ||
		    (strstr(q, "pos=") != NULL && !radio_ctl_query_int(q, "pos", &p)) || p < -1 ||
		    (p >= 0 && z == v->zone_index && !v->vfo && p >= v->zone_size)) {
			return 400;	// another zone's size isn't read (no zone in RAM): the radio ignores a position past its end
		}
		if (!radio_ctl_can(v)) {
			return 409;
		}
		radio_ctl_select((uint16_t)z, p, now);
		return 0;
	}
	bool zones = strncmp(q, "zone=", 5) == 0;
	if (!radio_ctl_query_int(q, zones ? "zone" : "channel", &n) || n == 0 || n > RADIO_CTL_STEP_MAX ||
	    n < -RADIO_CTL_STEP_MAX) {
		return 400;
	}
	if (!radio_ctl_can(v)) {
		return 409;
	}
	return radio_ctl_step(zones, n, now, v) ? 0 : 409;
}

// Where the radio is, the lease, and why not, if not. channel: false for a long name that wouldn't fit
static void reply_json(dl_text_t *t, const radio_view_t *v, bool channel, uint32_t lease, int err)
{
	radio_ctl_where(t, v, channel);
	dl_text(t, ",\"lease\":%u", (unsigned)lease);
	if (err != 0) {
		dl_text(t, ",\"err\":%d", err);
	}
	dl_text(t, "}\n");
}

static void udp_in(bool air, uint32_t src, uint16_t sport, uint32_t dst, uint16_t dport, const uint8_t *data, int length)
{
	(void)dst;
	(void)dport;
	if (air || length <= 0 || sport == 0) {
		return;
	}
	char r[REQ_MAX + 1] = "";
	int n = (length <= REQ_MAX) ? length : 0;	// too long: an empty request, refused
	memcpy(r, data, (size_t)n);
	r[n] = '\0';
	while (n > 0 && (r[n - 1] == '\n' || r[n - 1] == '\r')) {
		r[--n] = '\0';	// a line from nc or a script
	}
	uint32_t now = hal_millis();
	const radio_view_t *v = net_radio_view();
	st.requests++;
	int err = (n == 0) ? 400 : request(r, now, v);
	st.bad += err == 400;
	uint32_t lease = (strcmp(r, "select?notify=0") == 0) ? 0 : RADIO_CTL_LEASE_S;
	radio_ctl_subscribe(src, NET_RADIO_UDP_PORT, lease ? sport : 0, now);
	// The reply is where the radio is now: a step shows in the notice that follows
	char *buf = (char *)ipGatewayUdpPayload();
	dl_text_t t;
	dl_text_init(&t, buf, IPGW_UDP_PAYLOAD_MAX);
	reply_json(&t, v, true, lease, err);
	if (t.full) {
		dl_text_init(&t, buf, IPGW_UDP_PAYLOAD_MAX);
		reply_json(&t, v, false, lease, err);
	}
	bool sent = ipGatewayUdpSend(src, NET_RADIO_UDP_PORT, sport, (const uint8_t *)buf, (int)t.len);
	st.reply_busy += !sent;
	radio_ctl_replied(sent, v, now);	// lost (the link was busy): a notice brings it instead
}

void radio_udp_start(void)
{
	ipGatewayUdpBind(NET_RADIO_UDP_PORT, udp_in);	// from the link only: nobody on the air controls the radio
}

void radio_udp_view(dl_text_t *t)
{
	dl_text(t, "radio_udp.requests=%u\nradio_udp.bad=%u\nradio_udp.reply_busy=%u\n", (unsigned)st.requests,
		(unsigned)st.bad, (unsigned)st.reply_busy);
}

#endif	// CONFIG_NET_RADIO_UDP
