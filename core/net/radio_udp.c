// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Zone and channel control over UDP (docs/spec/tcp-services.md 8): select a zone and channel, or step them as a knob
// would, one datagram each to port NET_RADIO_UDP_PORT, from the link only (never the air). Every request is answered
// with where the radio is, one datagram, and makes its sender the change notices' subscriber for LEASE_S. No TCP and
// no HTTP: a radio short of RAM leaves them out and is still controlled. Whoever is on the USB network or PPP link
// owns the radio, so there is no host check. A lost step isn't sent again: the user steps again; where the radio is
// comes in every reply, every notice and the heartbeat.
//
// The requests are the radio core's own (radio_req_zone, radio_req_zone_pos, radio_req_channel_step), so the UI's
// refusals apply. Idle, it costs the radio nothing: the radio's view is read only for a request, a pending step or a
// subscriber. Replies and notices are written straight into the IP gateway's frame buffer: no buffer of its own.
#include "config.h"
#if CONFIG_NET_RADIO_UDP	// the whole file
#include <string.h>
#include "net/net_svc.h"
#include "codeplug/codeplug.h"
#include "dmr/ipGateway.h"
#include "input/input.h"
#include "hal_time.h"

#define REQ_MAX		40	// the longest request ("select?zone=65535&pos=65535" and room)
#define STEP_MAX	100	// one request's step, either way
#define LEASE_S		120u	// a subscription ends this long after the sender's last request
#define REQ_GAP_MS	20u	// radio requests at most this often (the radio core's queue holds 16), as rigctld's
#define ZONE_HOLD_MS	1000u	// a zone step counts from the last zone asked for this long (the view lags the request)
#define BEAT_MS		30000u	// a notice this often, changed or not
#define CHECK_MS	100u	// the radio's view is read for a change this often while subscribed

static const char *const mode_names[] = { "fm", "dmr", "p25", "nxdn" };

static struct {
	int32_t channel_steps;	// channel steps asked for and not yet passed on (a knob's turns add up)
	bool zone_due;		// a zone (and maybe a position) to go to
	uint16_t zone_to, pos_to;
	bool pos_due;
	uint32_t last_req_ms;
	uint32_t zone_asked_ms;	// when zone_to was last asked for (zone steps count from it for ZONE_HOLD_MS)
	bool zone_asked;
	uint32_t vfo_left_ms;	// when a step asked to leave the VFO (the view shows it a little later)
	bool vfo_left;
} pend;

// What a notice says, to send the next one only when it changes
typedef struct {
	uint16_t zone, pos, number;
	bool vfo, priv;
	uint8_t ts, mode;
	uint32_t codeplug, tg, rx_hz;	// rx_hz and mode: the VFO tuned, which no index shows
} notice_key_t;

// One subscriber (the last sender), until lease_ms
static struct {
	uint32_t ip;
	uint16_t port;		// 0: none
	uint32_t lease_ms, check_ms, sent_ms;
	notice_key_t key;	// what the last reply or notice said
	bool sent;
} sub;

static struct {
	uint32_t requests, bad, refused, selects, steps, notices, busy;
} st;
static uint32_t seq;		// notices sent (in the replies too, so a client can line them up)

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

// A codeplug name as a JSON string. Names are Latin-1, which is U+0000-U+00FF: a byte from 0x7F up is written as
// \u00XX, so the reply stays ASCII and valid JSON (RFC 8259 8.1)
static void json_str(dl_text_t *t, const char *s)
{
	char tmp[8];

	dl_text(t, "\"");
	for (; *s != '\0'; s++) {
		unsigned char ch = (unsigned char)*s;
		if (ch == '"' || ch == '\\') {
			dl_text(t, "\\%c", ch);
		} else if (ch < 0x20 || ch >= 0x7F) {
			dl_fmt(tmp, sizeof(tmp), "\\u%04x", ch);
			dl_text(t, "%s", tmp);
		} else {
			dl_text(t, "%c", ch);
		}
	}
	dl_text(t, "\"");
}

// Where the radio is and the channel it is on, from the view alone (no flash read), without the closing brace. The
// talk target is the radio's own (a manual change included); in the VFO its settings, number 0. channel: false leaves
// the keys after "number" out (a long name that wouldn't fit)
static void where(dl_text_t *t, const radio_view_t *v, bool channel)
{
	dl_text(t, "{\"zone\":%u,\"pos\":%u,\"vfo\":%s,\"codeplug\":%u,\"seq\":%u,\"zone_name\":", v->zone_index,
		v->zone_pos, v->vfo ? "true" : "false", (unsigned)codeplug_serial(), (unsigned)seq);
	json_str(t, v->zone_name);
	dl_text(t, ",\"number\":%u", v->channel_number);
	if (!channel) {
		return;
	}
	dl_text(t, ",\"name\":");
	json_str(t, v->ch.name);
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

// As radio_get_zone_pos: not transmitting, not a hotspot
static bool can(const radio_view_t *v)
{
	if (v->state != RS_TX && !v->hotspot) {
		return true;
	}
	st.refused++;
	return false;
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
	st.selects++;
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
	st.steps++;
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

static void notice_key(const radio_view_t *v, notice_key_t *k)
{
	memset(k, 0, sizeof(*k));	// padding too: the key is compared with memcmp
	k->zone = v->zone_index;
	k->pos = v->zone_pos;
	k->number = v->channel_number;
	k->vfo = v->vfo;
	k->codeplug = codeplug_serial();
	k->rx_hz = v->ch.rx_hz;
	k->mode = v->ch.mode;
#if CONFIG_DMR
	k->ts = v->ch.timeslot;
	k->tg = v->dmr.target_id;
	k->priv = v->dmr.target_private;
#endif
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
	notice_key_t k;
	notice_key(v, &k);
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
	int err = (n == 0) ? 400 : request(r, now, v);
	st.bad += err == 400;
	// The sender subscribes (a new one gets a notice at once), or cancels
	uint32_t lease = (strcmp(r, "select?notify=0") == 0) ? 0 : LEASE_S;
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
		notice_key(v, &sub.key);
		sub.sent_ms = now;
	}
}

void radio_udp_start(void)
{
	ipGatewayUdpBind(NET_RADIO_UDP_PORT, udp_in);	// from the link only: nobody on the air controls the radio
}

void radio_udp_poll(uint32_t now)
{
	apply(now);
	notify_poll(now);
}

void radio_udp_view(dl_text_t *t)
{
	dl_text(t, "radio_udp.requests=%u\nradio_udp.bad=%u\nradio_udp.refused=%u\nradio_udp.selects=%u\n"
		"radio_udp.steps=%u\nradio_udp.notify=%u\nradio_udp.notices=%u\nradio_udp.busy=%u\n", (unsigned)st.requests,
		(unsigned)st.bad, (unsigned)st.refused, (unsigned)st.selects, (unsigned)st.steps, (unsigned)sub.port,
		(unsigned)st.notices, (unsigned)st.busy);
}

#endif	// CONFIG_NET_RADIO_UDP
