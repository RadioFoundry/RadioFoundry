// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Zone and channel control over the status server (docs/spec/tcp-services.md 5.2), for a device or program on the
// host (USB network or PPP). /api/select says where the radio is and what it is on; a select or a step changes it as a
// knob would and answers only {"ok":true}. With CONFIG_NET_RADIO_NOTIFY a subscribed host port gets that same JSON
// as a UDP datagram when it changes, so a client doesn't poll. With CONFIG_NET_RADIO_ZONES /api/zone lists a zone's
// channels in pages, with CONFIG_NET_RADIO_DUMP /api/zones every zone and channel in one streamed reply. The requests
// are the radio core's own (radio_req_zone, radio_req_zone_pos, radio_req_channel_step), so the UI's refusals apply.
// Idle, it costs the radio nothing: the radio's view is read only for a request, a pending step or a subscriber.
#include "config.h"
#if CONFIG_NET_RADIO_API	// the whole file
#include <string.h>
#include "net/net_svc.h"
#include "net/tcp.h"
#include "codeplug/codeplug.h"
#include "dmr/ipGateway.h"
#include "hal_time.h"
#include "input/input.h"

#define REQ_GAP_MS	20u	// radio requests at most this often (the radio core's queue holds 16), as rigctld's
#define ZONE_HOLD_MS	1000u	// a zone step counts from the last zone asked for this long (the view lags the request)
#define STEP_MAX	100	// one request's step, either way
#define ROOM_ZONE	560u	// a zone: its name escaped and 80 channel numbers
#define ROOM_ITEM	420u	// the most one list item takes (both names escaped, every field); a page ends before it
				// would not fit
#define NOTIFY_PORT	80u	// notices come from the status server's port
#define NOTIFY_LEASE_S	120u	// a subscription ends this long after the last subscribe
#define NOTIFY_BEAT_MS	30000u	// a notice this often, changed or not
#define NOTIFY_CHECK_MS	100u	// the radio's view is read for a change this often while subscribed
#define NOTICE_MAX	512	// a notice: both names escaped, every field

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
	uint32_t lists, selects, steps, refused;
#if CONFIG_NET_RADIO_NOTIFY
	uint32_t notices, notice_busy;
#endif
} st;
static uint32_t seq;		// notices sent (in /api/select's replies too, so a client can line them up)

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

// /api/select's JSON and a notice's: where the radio is and the channel it is on (the VFO's settings, number 0), the
// talk target the radio's own (a manual change included), all from the view: no flash read. channel: false leaves
// the channel's keys after "number" out, for a notice that wouldn't fit
static void where_json(dl_text_t *t, const radio_view_t *v, bool channel)
{
	dl_text(t, "{\"zone\":%u,\"pos\":%u,\"vfo\":%s,\"codeplug\":%u,\"seq\":%u,\"zone_name\":", v->zone_index,
		v->zone_pos, v->vfo ? "true" : "false", (unsigned)codeplug_serial(), (unsigned)seq);
	http_json_str(t, v->zone_name);
	dl_text(t, ",\"number\":%u", v->channel_number);
	if (channel) {
		dl_text(t, ",\"name\":");
		http_json_str(t, v->ch.name);
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
	dl_text(t, "}\n");
}

#if CONFIG_NET_RADIO_NOTIFY
// What a notice says, to send the next one only when it changes
typedef struct {
	uint16_t zone, pos, number;
	bool vfo, priv;
	uint8_t ts, mode;
	uint32_t codeplug, tg, rx_hz;	// rx_hz and mode: the VFO tuned, which no index shows
} notice_key_t;

// One subscriber: the host's port, until lease_ms
static struct {
	uint16_t port;
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
	if (sub.port == 0) {
		return;
	}
	if ((int32_t)(now - sub.lease_ms) >= 0) {
		sub.port = 0;	// not renewed
		return;
	}
	if ((now - sub.check_ms) < NOTIFY_CHECK_MS) {
		return;	// a fast knob turn sends where it came to rest, not every detent
	}
	sub.check_ms = now;
	uint32_t host = ipGatewayHostIP();
	if (host == 0) {
		return;	// the link is down
	}
	const radio_view_t *v = net_radio_view();
	notice_key_t k;
	notice_key(v, &k);
	if (sub.sent && memcmp(&k, &sub.key, sizeof(k)) == 0 && (now - sub.sent_ms) < NOTIFY_BEAT_MS) {
		return;
	}
	static char notice[NOTICE_MAX];
	dl_text_t t;
	seq++;
	dl_text_init(&t, notice, sizeof(notice));
	where_json(&t, v, true);
	if (t.full || t.len > IPGW_UDP_PAYLOAD_MAX) {
		dl_text_init(&t, notice, sizeof(notice));
		where_json(&t, v, false);
	}
	if (!ipGatewayUdpSend(host, NOTIFY_PORT, sub.port, (const uint8_t *)notice, (int)t.len)) {
		seq--;
		st.notice_busy++;	// the USB IN endpoint was busy: again at the next check
		return;
	}
	st.notices++;
	sub.key = k;
	sub.sent = true;
	sub.sent_ms = now;
}

// GET /api/select?notify=PORT: subscribe (or renew) that host port; 0 cancels
static int notify_req(int32_t port, uint32_t now, const radio_view_t *v, dl_text_t *t)
{
	if (port < 0 || port > 65535) {
		return 400;
	}
	if (port == 0) {
		sub.port = 0;
		dl_text(t, "{\"ok\":true}\n");
		return 200;
	}
	if (sub.port != (uint16_t)port) {
		sub.sent = false;	// a new subscriber gets a notice at once
	}
	sub.port = (uint16_t)port;
	sub.lease_ms = now + NOTIFY_LEASE_S * 1000u;
	where_json(t, v, true);
	t->len--;		// the newline, then the lease: "this firmware sends notices"
	t->buf[t->len - 1] = ',';
	dl_text(t, "\"notify\":%u}\n", NOTIFY_LEASE_S);
	return 200;
}
#endif

static zone_t zone;		// 80 channels: not on the host link task's stack

#if CONFIG_NET_RADIO_ZONES

static void next_field(dl_text_t *t, uint32_t next, uint32_t count)
{
	if (next < count) {
		dl_text(t, "],\"next\":%u}\n", (unsigned)next);
	} else {
		dl_text(t, "],\"next\":null}\n");
	}
}

// A subtone as radio programs write it: "100.0" (CTCSS), "D023N" or "D023I" (DCS)
static void tone_json(dl_text_t *t, const char *key, const subtone_t *s)
{
	if (s->type == TONE_CTCSS) {
		dl_text(t, ",\"%s\":\"%u.%u\"", key, s->value / 10u, s->value % 10u);
	} else if (s->type == TONE_DCS || s->type == TONE_DCS_INV) {
		dl_text(t, ",\"%s\":\"D%u%u%u%c\"", key, (s->value >> 6) & 7u, (s->value >> 3) & 7u, s->value & 7u,
			(s->type == TONE_DCS_INV) ? 'I' : 'N');	// octal digits (dl_text has no %o)
	}
}

// A channel's fields after its "pos" and "number", as /api/zone's items and /api/zones' channels give them (and a VFO
// setting would take): what a client needs to show it and know what it is
static void channel_fields(dl_text_t *t, const channel_t *ch)
{
	dl_text(t, ",\"name\":");
	http_json_str(t, ch->name);
	dl_text(t, ",\"rx_hz\":%u,\"tx_hz\":%u,\"mode\":\"%s\",\"bw\":\"%s\"", (unsigned)ch->rx_hz,
		(unsigned)ch->tx_hz, (ch->mode < 4u) ? mode_names[ch->mode] : "?", (ch->bw == RADIO_BW_25K) ? "25" : "12.5");
	if (ch->power != CODEPLUG_POWER_DEFAULT) {
		dl_text(t, ",\"power\":%u", ch->power);
	}
	if (ch->rx_only) {
		dl_text(t, ",\"rx_only\":true");
	}
	if (ch->mode == RADIO_MODE_FM) {
		tone_json(t, "rx_tone", &ch->rx_tone);
		tone_json(t, "tx_tone", &ch->tx_tone);
	}
#if CONFIG_DMR
	if (ch->mode == RADIO_MODE_DMR) {
		dl_text(t, ",\"cc\":%u,\"ts\":%u", ch->color_code, ch->timeslot);
		// the contact the radio talks to on it: the TX contact, else its RX group list's first talkgroup
		uint16_t talk = codeplug_channel_talk_contact(ch);
		contact_t c;
		if (talk != 0 && codeplug_contact(talk, &c)) {
			static const char *const kinds[] = { "group", "private", "all" };
			dl_text(t, ",\"tg\":%u,\"call\":\"%s\",\"contact\":", (unsigned)c.id,
				(c.type < 3u) ? kinds[c.type] : "?");
			http_json_str(t, c.name);
		}
	}
#endif
}

// GET /api/zone?zone=Z[&from=N]: one zone in pages, each item a channel with its position:
// {"zone":Z,"name":..,"size":n,"codeplug":..,"from":N,"items":[...],"next":..}
static int zone_json(const char *q, dl_text_t *t)
{
	int32_t z, from = 0;
	if (q == NULL || !query_int(q, "zone", &z) || z < 0 || z >= codeplug_zone_count() ||
	    (strstr(q, "from=") != NULL && (!query_int(q, "from", &from) || from < 0))) {
		return 400;
	}
	if (!codeplug_zone((uint16_t)z, &zone)) {
		return 404;
	}
	dl_text(t, "{\"zone\":%u,\"name\":", (unsigned)z);
	http_json_str(t, zone.name);
	dl_text(t, ",\"size\":%u,\"codeplug\":%u,\"from\":%u,\"items\":[", zone.count, (unsigned)codeplug_serial(),
		(unsigned)from);
	uint32_t i = (uint32_t)from;
	for (; i < zone.count && t->len + ROOM_ITEM < t->cap; i++) {
		channel_t ch;
		dl_text(t, "%s{\"pos\":%u,\"number\":%u", (i == (uint32_t)from) ? "" : ",", (unsigned)i, zone.channels[i]);
		if (codeplug_channel(zone.channels[i], &ch)) {
			channel_fields(t, &ch);
		}
		dl_text(t, "}");
	}
	next_field(t, i, zone.count);
	return 200;
}
#endif

#if CONFIG_NET_RADIO_DUMP
// GET /api/zones[?zones=N&per_zone=M]: the whole structure in one reply, streamed (a buffer at a time as TCP takes
// it, never all in RAM): {"codeplug","zone","pos","vfo","zones":[{"zone","name","channels":[numbers]}],
// "channels":[{"number", fields}]}, each channel once, those the zones name. zones and per_zone keep it to the first
// N zones and their first M channels, for a client with little memory
enum { GEN_IDLE, GEN_ZONES, GEN_CHANNELS };
static struct {
	uint8_t phase;
	bool first;		// the next item is the first of its list (no comma)
	uint16_t zone, zones, per_zone;	// the next zone; the limits
	uint16_t number;	// the next channel number to look at
	uint32_t serial;	// the codeplug's change count at the start: a change cuts the reply
	uint8_t used[CODEPLUG_CHANNELS_MAX / 8u];	// channels the zones sent name
} gen;

static int dump_start(const char *q, bool head, dl_text_t *t)
{
	int32_t zn = 0, pz = 0;
	if (q != NULL && ((strstr(q, "zones=") != NULL && (!query_int(q, "zones", &zn) || zn < 1)) ||
			  (strstr(q, "per_zone=") != NULL && (!query_int(q, "per_zone", &pz) || pz < 1)))) {
		return 400;
	}
	const radio_view_t *v = net_radio_view();
	uint16_t count = codeplug_zone_count();
	memset(&gen, 0, sizeof(gen));
	gen.serial = codeplug_serial();
	gen.zones = (zn > 0 && zn < count) ? (uint16_t)zn : count;
	gen.per_zone = (pz > 0 && pz < CODEPLUG_ZONE_MAX) ? (uint16_t)pz : CODEPLUG_ZONE_MAX;
	gen.first = true;
	gen.phase = head ? GEN_IDLE : GEN_ZONES;
	dl_text(t, "{\"codeplug\":%u,\"zone\":%u,\"pos\":%u,\"vfo\":%s,\"zones\":[", (unsigned)gen.serial,
		v->zone_index, v->zone_pos, v->vfo ? "true" : "false");
	return HTTP_RADIO_STREAM;
}

int http_radio_more(dl_text_t *t)
{
	if (gen.phase == GEN_IDLE) {
		return 0;
	}
	if (codeplug_serial() != gen.serial) {
		gen.phase = GEN_IDLE;
		return -1;
	}
	while (gen.phase == GEN_ZONES && t->len + ROOM_ZONE < t->cap) {
		if (gen.zone >= gen.zones) {
			dl_text(t, "],\"channels\":[");
			gen.phase = GEN_CHANNELS;
			gen.number = 1;
			gen.first = true;
			break;
		}
		if (codeplug_zone(gen.zone, &zone)) {
			dl_text(t, "%s{\"zone\":%u,\"name\":", gen.first ? "" : ",", gen.zone);
			http_json_str(t, zone.name);
			dl_text(t, ",\"channels\":[");
			for (uint16_t i = 0; i < zone.count && i < gen.per_zone; i++) {
				uint16_t n = zone.channels[i];
				dl_text(t, "%s%u", i ? "," : "", n);
				if (n >= 1 && n <= CODEPLUG_CHANNELS_MAX) {
					gen.used[(n - 1u) / 8u] |= (uint8_t)(1u << ((n - 1u) % 8u));
				}
			}
			dl_text(t, "]}");
			gen.first = false;
		}
		gen.zone++;
	}
	while (gen.phase == GEN_CHANNELS && t->len + ROOM_ITEM < t->cap) {
		if (gen.number > CODEPLUG_CHANNELS_MAX) {
			dl_text(t, "]}\n");
			gen.phase = GEN_IDLE;
			break;
		}
		uint16_t n = gen.number++;
		channel_t ch;
		if ((gen.used[(n - 1u) / 8u] & (1u << ((n - 1u) % 8u))) == 0 || !codeplug_channel(n, &ch)) {
			continue;
		}
		dl_text(t, "%s{\"number\":%u", gen.first ? "" : ",", n);
		channel_fields(t, &ch);
		dl_text(t, "}");
		gen.first = false;
	}
	return (t->len > 0) ? 1 : 0;
}
#endif

// The radio would take a zone or channel request now (as radio_get_zone_pos: not transmitting, not a hotspot)
static bool can_select(const radio_view_t *v)
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

// GET /api/select?zone=Z[&pos=P]: that zone (its first channel, or position P), leaving the VFO
static int select_req(const char *q, bool head, uint32_t now, const radio_view_t *v)
{
	int32_t z, p = -1;
	if (!query_int(q, "zone", &z) || z < 0 || z >= codeplug_zone_count() ||
	    (strstr(q, "pos=") != NULL && !query_int(q, "pos", &p)) || p < -1) {
		return 400;
	}
	if (p >= 0) {
		// the current zone's size from the view: the frequent case reads no flash
		uint16_t size = (z == v->zone_index && !v->vfo) ? v->zone_size :
			codeplug_zone((uint16_t)z, &zone) ? zone.count : 0;
		if (p >= size) {
			return 400;
		}
	}
	if (!can_select(v)) {
		st.refused++;
		return 409;
	}
	if (!head) {
		st.selects++;
		pend.zone_due = true;
		pend.zone_to = (uint16_t)z;
		pend.pos_due = p >= 0;
		pend.pos_to = (uint16_t)((p >= 0) ? p : 0);
		pend.channel_steps = 0;	// a jump replaces steps not yet passed on
		pend.zone_asked = true;
		pend.zone_asked_ms = now;
		apply(now);
	}
	return 200;
}

// GET /api/step?channel=N or ?zone=N: N channels or zones on (negative: back), wrapping at the ends. From the VFO a
// channel step goes back to the channel the VFO came from as its first step
static int step_req(const char *q, bool head, uint32_t now, const radio_view_t *v)
{
	int32_t n;
	bool zones = strncmp(q, "zone=", 5) == 0;
	if (!query_int(q, zones ? "zone" : "channel", &n) || n == 0 || n > STEP_MAX || n < -STEP_MAX) {
		return 400;
	}
	if (!can_select(v)) {
		st.refused++;
		return 409;
	}
	if (head) {
		return 200;
	}
	st.steps++;
	if (zones) {
		uint16_t count = codeplug_zone_count();
		if (count == 0) {
			return 409;
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
	return 200;
}

int http_radio(const char *path, const char *query, uint32_t from_ip, bool head, dl_text_t *t)
{
	uint32_t now = hal_millis();
	const char *q = (query != NULL) ? query : "";
#if CONFIG_NET_RADIO_DUMP
	if (strcmp(path, "/api/zones") == 0) {
		st.lists++;
		return dump_start(q, head, t);
	}
#endif
#if CONFIG_NET_RADIO_ZONES
	if (strcmp(path, "/api/zone") == 0) {
		st.lists++;
		return zone_json(q, t);
	}
#endif
	bool sel = strcmp(path, "/api/select") == 0;
	if (!sel && strcmp(path, "/api/step") != 0) {
		return 0;
	}
	const radio_view_t *v = net_radio_view();
	if (sel && *q == '\0') {
		where_json(t, v, true);	// where the radio is, read often (a knob's display): from the view alone
		return 200;
	}
	if (from_ip != ipGatewayHostIP()) {
		return 403;	// changes only from the host (USB network or PPP), as rigctld's PTT and web control
	}
#if CONFIG_NET_RADIO_NOTIFY
	int32_t port;
	if (sel && strncmp(q, "notify=", 7) == 0) {
		if (!query_int(q, "notify", &port)) {
			return 400;
		}
		if (head) {
			return 200;	// HEAD changes nothing
		}
		return notify_req(port, now, v, t);
	}
#endif
	int code = sel ? select_req(q, head, now, v) : step_req(q, head, now, v);
	if (code == 200) {
		dl_text(t, "{\"ok\":true}\n");
	}
	return code;
}

void http_radio_poll(uint32_t now_ms)
{
	apply(now_ms);
#if CONFIG_NET_RADIO_NOTIFY
	notify_poll(now_ms);
#endif
}

void http_radio_view(dl_text_t *t)
{
	dl_text(t, "http.radio_lists=%u\nhttp.radio_selects=%u\nhttp.radio_steps=%u\nhttp.radio_refused=%u\n",
		(unsigned)st.lists, (unsigned)st.selects, (unsigned)st.steps, (unsigned)st.refused);
#if CONFIG_NET_RADIO_NOTIFY
	dl_text(t, "http.radio_notify=%u\nhttp.radio_notices=%u\nhttp.radio_notice_busy=%u\n", sub.port,
		(unsigned)st.notices, (unsigned)st.notice_busy);
#endif
}

#endif	// CONFIG_NET_RADIO_API
