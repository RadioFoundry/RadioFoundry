// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Zone and channel control over the status server (docs/spec/tcp-services.md 5.2), for a device or program on the
// host (USB network or PPP). /api/select says where the radio is and what it is on; a select or a step changes it as a
// knob would and answers only {"ok":true}. With CONFIG_NET_RADIO_NOTIFY a subscribed host port gets that same JSON
// as a UDP datagram when it changes, so a client doesn't poll. With CONFIG_NET_RADIO_ZONES /api/zone lists a zone's
// channels in pages, with CONFIG_NET_RADIO_DUMP /api/zones every zone and channel in one streamed reply. The control
// itself, shared with the UDP requests, is radio_ctl.c.
#include "config.h"
#if CONFIG_NET_RADIO_API	// the whole file
#include <string.h>
#include "net/net_svc.h"
#include "net/tcp.h"
#include "net/radio_ctl.h"
#include "codeplug/codeplug.h"
#include "dmr/ipGateway.h"
#include "hal_time.h"

#define ROOM_ZONE	560u	// a zone: its name escaped and 80 channel numbers
#define ROOM_ITEM	420u	// the most one list item takes (both names escaped, every field); a page ends before it
				// would not fit
#define NOTIFY_PORT	80u	// notices come from the status server's port

#if CONFIG_NET_RADIO_ZONES
static const char *const mode_names[] = { "fm", "dmr", "p25", "nxdn" };
#endif
static uint32_t lists;		// zone pages and dumps asked for

#if CONFIG_NET_RADIO_NOTIFY
// GET /api/select?notify=PORT: subscribe (or renew) that host port; 0 cancels
static int notify_req(int32_t port, uint32_t from_ip, uint32_t now, const radio_view_t *v, dl_text_t *t)
{
	if (port < 0 || port > 65535) {
		return 400;
	}
	radio_ctl_subscribe(from_ip, NOTIFY_PORT, (uint16_t)port, now);
	if (port == 0) {
		dl_text(t, "{\"ok\":true}\n");
		return 200;
	}
	radio_ctl_where(t, v, true);
	dl_text(t, ",\"notify\":%u}\n", RADIO_CTL_LEASE_S);	// the lease: "this firmware sends notices"
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
	net_json_str(t, ch->name);
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
			net_json_str(t, c.name);
		}
	}
#endif
}

// GET /api/zone?zone=Z[&from=N]: one zone in pages, each item a channel with its position:
// {"zone":Z,"name":..,"size":n,"codeplug":..,"from":N,"items":[...],"next":..}
static int zone_json(const char *q, dl_text_t *t)
{
	int32_t z, from = 0;
	if (q == NULL || !radio_ctl_query_int(q, "zone", &z) || z < 0 || z >= codeplug_zone_count() ||
	    (strstr(q, "from=") != NULL && (!radio_ctl_query_int(q, "from", &from) || from < 0))) {
		return 400;
	}
	if (!codeplug_zone((uint16_t)z, &zone)) {
		return 404;
	}
	dl_text(t, "{\"zone\":%u,\"name\":", (unsigned)z);
	net_json_str(t, zone.name);
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
	if (q != NULL && ((strstr(q, "zones=") != NULL && (!radio_ctl_query_int(q, "zones", &zn) || zn < 1)) ||
			  (strstr(q, "per_zone=") != NULL && (!radio_ctl_query_int(q, "per_zone", &pz) || pz < 1)))) {
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
			net_json_str(t, zone.name);
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

// GET /api/select?zone=Z[&pos=P]: that zone (its first channel, or position P), leaving the VFO
static int select_req(const char *q, bool head, uint32_t now, const radio_view_t *v)
{
	int32_t z, p = -1;
	if (!radio_ctl_query_int(q, "zone", &z) || z < 0 || z >= codeplug_zone_count() ||
	    (strstr(q, "pos=") != NULL && !radio_ctl_query_int(q, "pos", &p)) || p < -1) {
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
	if (!radio_ctl_can(v)) {
		return 409;
	}
	if (!head) {
		radio_ctl_select((uint16_t)z, p, now);
	}
	return 200;
}

// GET /api/step?channel=N or ?zone=N: N channels or zones on (negative: back), wrapping at the ends. From the VFO a
// channel step goes back to the channel the VFO came from as its first step
static int step_req(const char *q, bool head, uint32_t now, const radio_view_t *v)
{
	int32_t n;
	bool zones = strncmp(q, "zone=", 5) == 0;
	if (!radio_ctl_query_int(q, zones ? "zone" : "channel", &n) || n == 0 || n > RADIO_CTL_STEP_MAX ||
	    n < -RADIO_CTL_STEP_MAX) {
		return 400;
	}
	if (!radio_ctl_can(v)) {
		return 409;
	}
	if (head) {
		return 200;
	}
	return radio_ctl_step(zones, n, now, v) ? 200 : 409;
}

int http_radio(const char *path, const char *query, uint32_t from_ip, bool head, dl_text_t *t)
{
	uint32_t now = hal_millis();
	const char *q = (query != NULL) ? query : "";
#if CONFIG_NET_RADIO_DUMP
	if (strcmp(path, "/api/zones") == 0) {
		lists++;
		return dump_start(q, head, t);
	}
#endif
#if CONFIG_NET_RADIO_ZONES
	if (strcmp(path, "/api/zone") == 0) {
		lists++;
		return zone_json(q, t);
	}
#endif
	bool sel = strcmp(path, "/api/select") == 0;
	if (!sel && strcmp(path, "/api/step") != 0) {
		return 0;
	}
	const radio_view_t *v = net_radio_view();
	if (sel && *q == '\0') {
		radio_ctl_where(t, v, true);	// where the radio is, read often (a knob's display): from the view alone
		dl_text(t, "}\n");
		return 200;
	}
	if (from_ip != ipGatewayHostIP()) {
		return 403;	// changes only from the host (USB network or PPP), as rigctld's PTT and web control
	}
#if CONFIG_NET_RADIO_NOTIFY
	int32_t port;
	if (sel && strncmp(q, "notify=", 7) == 0) {
		if (!radio_ctl_query_int(q, "notify", &port)) {
			return 400;
		}
		if (head) {
			return 200;	// HEAD changes nothing
		}
		return notify_req(port, from_ip, now, v, t);
	}
#endif
	int code = sel ? select_req(q, head, now, v) : step_req(q, head, now, v);
	if (code == 200) {
		dl_text(t, "{\"ok\":true}\n");
	}
	return code;
}

void http_radio_view(dl_text_t *t)
{
	dl_text(t, "http.radio_lists=%u\n", (unsigned)lists);
}

#endif	// CONFIG_NET_RADIO_API
