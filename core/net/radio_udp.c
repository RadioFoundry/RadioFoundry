// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Zone and channel control over UDP (docs/spec/tcp-services.md 8): select a zone and channel, or step them as a knob
// would, one datagram each to port NET_RADIO_UDP_PORT, from the link only (never the air). Every request is answered
// with where the radio is, one datagram, and makes its sender the change notices' subscriber for LEASE_S. No TCP and
// no HTTP: a radio short of RAM leaves them out and is still controlled. Whoever is on the USB network or PPP link
// owns the radio, so there is no host check. A lost step isn't sent again: the user steps again; where the radio is
// comes in every reply, every notice and the heartbeat.
//
// The requests are radio_ctl.c's, which /ws takes too; this file is only their UDP carrier. Idle, it costs the radio
// nothing: the radio's view is read only for a request or a subscriber. Replies and notices are written straight into
// the IP gateway's frame buffer: no buffer of its own.
#include "config.h"
#if CONFIG_NET_RADIO_UDP	// the whole file
#include <string.h>
#include "net/net_svc.h"
#include "codeplug/codeplug.h"
#include "dmr/ipGateway.h"
#include "hal_time.h"

#define REQ_MAX		40	// the longest request ("select?zone=65535&pos=65535" and room)
#define LEASE_S		120u	// a subscription ends this long after the sender's last request
#define BEAT_MS		30000u	// a notice this often, changed or not
#define CHECK_MS	100u	// the radio's view is read for a change this often while subscribed

static const char *const mode_names[] = { "fm", "dmr", "p25", "nxdn" };

// One subscriber (the last sender), until lease_ms
static struct {
	uint32_t ip;
	uint16_t port;		// 0: none
	uint32_t lease_ms, check_ms, sent_ms;
	radio_ctl_key_t key;	// what the last reply or notice said
	bool sent;
} sub;

static struct {
	uint32_t requests, bad, notices, busy;
} st;
static uint32_t seq;		// notices sent (in the replies too, so a client can line them up)

// Where the radio is and the channel it is on, from the view alone (no flash read), without the closing brace. The
// talk target is the radio's own (a manual change included); in the VFO its settings, number 0. channel: false leaves
// the keys after "number" out (a long name that wouldn't fit)
static void where(dl_text_t *t, const radio_view_t *v, bool channel)
{
	dl_text(t, "{\"zone\":%u,\"pos\":%u,\"vfo\":%s,\"codeplug\":%u,\"seq\":%u,\"zone_name\":", v->zone_index,
		v->zone_pos, v->vfo ? "true" : "false", (unsigned)codeplug_serial(), (unsigned)seq);
	dl_json_str(t, v->zone_name);
	dl_text(t, ",\"number\":%u", v->channel_number);
	if (!channel) {
		return;
	}
	dl_text(t, ",\"name\":");
	dl_json_str(t, v->ch.name);
	dl_text(t, ",\"mode\":\"%s\",\"rx_hz\":%u", (v->ch.mode < 4u) ? mode_names[v->ch.mode] : "?",
		(unsigned)v->ch.rx_hz);
#if CONFIG_DMR
	if (v->ch.mode == RADIO_MODE_DMR) {
		dl_text(t, ",\"cc\":%u,\"ts\":%u", v->ch.color_code, v->ch.timeslot);
		if (v->dmr.target_id != 0) {
			dl_text(t, ",\"tg\":%u,\"call\":\"%s\"", (unsigned)v->dmr.target_id,
				v->dmr.target_private ? "private" : "group");
		}
	}
#endif
}

// Where the radio is, into the gateway's frame buffer: a reply (lease >= 0) with its lease and error, or a notice
static int put_where(char *buf, const radio_view_t *v, int32_t lease, int err)
{
	dl_text_t t;
	for (int pass = 0; pass < 2; pass++) {
		dl_text_init(&t, buf, IPGW_UDP_PAYLOAD_MAX);
		where(&t, v, pass == 0);
		if (lease >= 0) {
			dl_text(&t, ",\"lease\":%u", (unsigned)lease);
		}
		if (err != 0) {
			dl_text(&t, ",\"err\":%d", err);
		}
		dl_text(&t, "}\n");
		if (!t.full) {
			break;	// else again without the channel's keys: the indexes still go
		}
	}
	return (int)t.len;
}

static void notify_poll(uint32_t now)
{
	if (sub.port == 0) {
		return;
	}
	if ((int32_t)(now - sub.lease_ms) >= 0) {
		sub.port = 0;	// not renewed
		return;
	}
	if ((now - sub.check_ms) < CHECK_MS) {
		return;	// a fast knob turn sends where it came to rest, not every detent
	}
	sub.check_ms = now;
	if (ipGatewayHostIP() == 0) {
		return;	// the link is down
	}
	const radio_view_t *v = net_radio_view();
	radio_ctl_key_t k;
	radio_ctl_key(v, &k);
	if (sub.sent && memcmp(&k, &sub.key, sizeof(k)) == 0 && (now - sub.sent_ms) < BEAT_MS) {
		return;
	}
	char *buf = (char *)ipGatewayUdpPayload();
	seq++;
	int len = put_where(buf, v, -1, 0);
	if (!ipGatewayUdpSend(sub.ip, NET_RADIO_UDP_PORT, sub.port, (const uint8_t *)buf, len)) {
		seq--;
		st.busy++;	// the link was busy: again at the next check
		return;
	}
	st.notices++;
	sub.key = k;
	sub.sent = true;
	sub.sent_ms = now;
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
	// The sender subscribes (a new one gets a notice at once), or cancels; "select" alone asks where the radio is
	uint32_t lease = (strcmp(r, "select?notify=0") == 0) ? 0 : LEASE_S;
	int err = (n == 0) ? 400 : (lease == 0 || strcmp(r, "select") == 0) ? 0 : radio_ctl_request(r, now, v);
	st.bad += err == 400;
	if (src != sub.ip || sport != sub.port) {
		sub.sent = false;
	}
	sub.ip = src;
	sub.port = lease ? sport : 0;
	sub.lease_ms = now + LEASE_S * 1000u;
	// The reply goes to the sender's address and port, and is where the radio is now: a step shows in the notice
	// that follows
	char *buf = (char *)ipGatewayUdpPayload();
	int len = put_where(buf, v, (int32_t)lease, err);
	bool sent = ipGatewayUdpSend(src, NET_RADIO_UDP_PORT, sport, (const uint8_t *)buf, len);
	st.busy += !sent;
	// A reply that went counts as a notice, so none repeats it; a lost one (the link was busy) brings a notice instead
	sub.sent = sent;
	if (sent) {
		radio_ctl_key(v, &sub.key);
		sub.sent_ms = now;
	}
}

void radio_udp_start(void)
{
	ipGatewayUdpBind(NET_RADIO_UDP_PORT, udp_in);	// from the link only: nobody on the air controls the radio
}

void radio_udp_poll(uint32_t now)
{
	notify_poll(now);
}

void radio_udp_view(dl_text_t *t)
{
	dl_text(t, "radio_udp.requests=%u\nradio_udp.bad=%u\nradio_udp.notify=%u\nradio_udp.notices=%u\nradio_udp.busy=%u\n",
		(unsigned)st.requests, (unsigned)st.bad, (unsigned)sub.port, (unsigned)st.notices, (unsigned)st.busy);
}

#endif	// CONFIG_NET_RADIO_UDP
