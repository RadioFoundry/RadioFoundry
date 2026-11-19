// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Zone and channel control shared by its HTTP and UDP requests (radio_ctl.h, docs/spec/tcp-services.md 5.2, 5.3). The
// requests are the radio core's own (radio_req_zone, radio_req_zone_pos, radio_req_channel_step), so the UI's refusals
// apply. Idle, it costs the radio nothing: the radio's view is read only for a request, a pending step or a
// subscriber. A notice is written straight into the IP gateway's frame buffer: no buffer of its own.
#include "config.h"
#if CONFIG_NET_RADIO_CTL	// the whole file
#include <string.h>
#include "net/radio_ctl.h"
#include "net/net_svc.h"
#include "codeplug/codeplug.h"
#include "dmr/ipGateway.h"
#include "input/input.h"

#define REQ_GAP_MS	20u	// radio requests at most this often (the radio core's queue holds 16), as rigctld's
#define ZONE_HOLD_MS	1000u	// a zone step counts from the last zone asked for this long (the view lags the request)
#define NOTIFY_BEAT_MS	30000u	// a notice this often, changed or not
#define NOTIFY_CHECK_MS	100u	// the radio's view is read for a change this often while subscribed

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
static struct {
	uint32_t selects, steps, refused;
#if CONFIG_NET_RADIO_NOTICES
	uint32_t notices, notice_busy;
#endif
} st;
static uint32_t seq;		// notices sent

bool radio_ctl_query_int(const char *q, const char *key, int32_t *out)
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

// The talk target is the radio's own (a manual change included); in the VFO its settings, number 0
void radio_ctl_where(dl_text_t *t, const radio_view_t *v, bool channel)
{
	dl_text(t, "{\"zone\":%u,\"pos\":%u,\"vfo\":%s,\"codeplug\":%u,\"seq\":%u,\"zone_name\":", v->zone_index,
		v->zone_pos, v->vfo ? "true" : "false", (unsigned)codeplug_serial(), (unsigned)seq);
	net_json_str(t, v->zone_name);
	dl_text(t, ",\"number\":%u", v->channel_number);
	if (!channel) {
		return;
	}
	dl_text(t, ",\"name\":");
	net_json_str(t, v->ch.name);
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

// As radio_get_zone_pos: not transmitting, not a hotspot
bool radio_ctl_can(const radio_view_t *v)
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

void radio_ctl_select(uint16_t z, int32_t pos, uint32_t now)
{
	st.selects++;
	pend.zone_due = true;
	pend.zone_to = z;
	pend.pos_due = pos >= 0;
	pend.pos_to = (uint16_t)((pos >= 0) ? pos : 0);
	pend.channel_steps = 0;	// a jump replaces steps not yet passed on
	pend.zone_asked = true;
	pend.zone_asked_ms = now;
	apply(now);
}

bool radio_ctl_step(bool zones, int32_t n, uint32_t now, const radio_view_t *v)
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

#if CONFIG_NET_RADIO_NOTICES
// What a notice says, to send the next one only when it changes
typedef struct {
	uint16_t zone, pos, number;
	bool vfo, priv;
	uint8_t ts, mode;
	uint32_t codeplug, tg, rx_hz;	// rx_hz and mode: the VFO tuned, which no index shows
} notice_key_t;

// One subscriber, until lease_ms
static struct {
	uint32_t ip;
	uint16_t from, to;	// the radio's port, the subscriber's
	uint32_t lease_ms, check_ms, sent_ms;
	notice_key_t key;	// what the last notice said
	bool sent;
} sub;

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
	if (sub.to == 0) {
		return;
	}
	if ((int32_t)(now - sub.lease_ms) >= 0) {
		sub.to = 0;	// not renewed
		return;
	}
	if ((now - sub.check_ms) < NOTIFY_CHECK_MS) {
		return;	// a fast knob turn sends where it came to rest, not every detent
	}
	sub.check_ms = now;
	if (ipGatewayHostIP() == 0) {
		return;	// the link is down
	}
	const radio_view_t *v = net_radio_view();
	notice_key_t k;
	notice_key(v, &k);
	if (sub.sent && memcmp(&k, &sub.key, sizeof(k)) == 0 && (now - sub.sent_ms) < NOTIFY_BEAT_MS) {
		return;
	}
	char *buf = (char *)ipGatewayUdpPayload();
	dl_text_t t;
	seq++;
	dl_text_init(&t, buf, IPGW_UDP_PAYLOAD_MAX);
	radio_ctl_where(&t, v, true);
	dl_text(&t, "}\n");
	if (t.full) {
		dl_text_init(&t, buf, IPGW_UDP_PAYLOAD_MAX);	// a long name: the indexes still go
		radio_ctl_where(&t, v, false);
		dl_text(&t, "}\n");
	}
	if (!ipGatewayUdpSend(sub.ip, sub.from, sub.to, (const uint8_t *)buf, (int)t.len)) {
		seq--;
		st.notice_busy++;	// the link was busy: again at the next check
		return;
	}
	st.notices++;
	sub.key = k;
	sub.sent = true;
	sub.sent_ms = now;
}

void radio_ctl_subscribe(uint32_t ip, uint16_t from, uint16_t to, uint32_t now)
{
	if (ip != sub.ip || from != sub.from || to != sub.to) {
		sub.sent = false;	// a new subscriber gets a notice at once
	}
	sub.ip = ip;
	sub.from = from;
	sub.to = to;
	sub.lease_ms = now + RADIO_CTL_LEASE_S * 1000u;
}

uint32_t radio_ctl_seq(void)
{
	return seq;
}

void radio_ctl_replied(bool sent, const radio_view_t *v, uint32_t now)
{
	sub.sent = sent;
	if (sent) {
		notice_key(v, &sub.key);
		sub.sent_ms = now;
	}
}
#endif

void radio_ctl_poll(uint32_t now)
{
	apply(now);
#if CONFIG_NET_RADIO_NOTICES
	notify_poll(now);
#endif
}

void radio_ctl_view(dl_text_t *t)
{
	dl_text(t, "radio_ctl.selects=%u\nradio_ctl.steps=%u\nradio_ctl.refused=%u\n", (unsigned)st.selects,
		(unsigned)st.steps, (unsigned)st.refused);
#if CONFIG_NET_RADIO_NOTICES
	dl_text(t, "radio_ctl.notify=%u\nradio_ctl.notices=%u\nradio_ctl.notice_busy=%u\n", sub.to, (unsigned)st.notices,
		(unsigned)st.notice_busy);
#endif
}

#endif	// CONFIG_NET_RADIO_CTL
