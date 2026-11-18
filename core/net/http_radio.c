// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Zone and channel control over the status server (docs/spec/tcp-services.md 5.2), for a device or program on the
// host (USB network or PPP): the zones, a zone's channels and one channel as JSON, read when asked, and short requests
// to select a zone and position or step through them, as a knob would. The requests are the radio core's own
// (radio_req_zone, radio_req_zone_pos, radio_req_channel_step), so the UI's refusals apply; their replies carry no
// channel data: a client that wants to know reads the list once, or /api/channel.
#include "config.h"
#if CONFIG_NET_RADIO_API	// the whole file
#include <string.h>
#include "net/net_svc.h"
#include "net/tcp.h"
#include "codeplug/codeplug.h"
#include "dmr/ipGateway.h"
#include "hal_time.h"

#define REQ_GAP_MS	20u	// radio requests at most this often (the radio core's queue holds 16), as rigctld's
#define ZONE_HOLD_MS	1000u	// a zone step counts from the last zone asked for this long (the view lags the request)
#define STEP_MAX	100	// one request's step, either way
#define ROOM_ITEM	420u	// the most one list item takes (both names escaped, every field); a page ends before it
				// would not fit

static zone_t zone;		// 80 channels: not on the host link task's stack
static struct {
	int32_t channel_steps;	// channel steps asked for and not yet passed on (a knob's turns add up)
	bool zone_due;		// a zone (and maybe a position) to go to
	uint16_t zone_to, pos_to;
	bool pos_due;
	uint32_t last_req_ms;
	uint32_t zone_asked_ms;	// when zone_to was last asked for (zone steps count from it for ZONE_HOLD_MS)
	bool zone_asked;
} pend;
static struct {
	uint32_t lists, selects, steps, refused;
} st;

static const char *const mode_names[] = { "fm", "dmr", "p25", "nxdn" };

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

static void next_field(dl_text_t *t, uint32_t next, uint32_t count)
{
	if (next < count) {
		dl_text(t, "],\"next\":%u}\n", (unsigned)next);
	} else {
		dl_text(t, "],\"next\":null}\n");
	}
}

// GET /api/zones[?from=N]: {"zones":Z,"zone":current,"from":N,"items":[{"zone":i,"name":..,"channels":n}],"next":..}
static int zones_json(const char *q, dl_text_t *t)
{
	int32_t from = 0;
	uint16_t count = codeplug_zone_count();
	if (q != NULL && strstr(q, "from=") != NULL && (!query_int(q, "from", &from) || from < 0)) {
		return 400;
	}
	dl_text(t, "{\"zones\":%u,\"zone\":%u,\"from\":%u,\"items\":[", count, net_radio_view()->zone_index,
		(unsigned)from);
	uint32_t i = (uint32_t)from;
	for (; i < count && t->len + ROOM_ITEM < t->cap; i++) {
		if (!codeplug_zone((uint16_t)i, &zone)) {
			continue;
		}
		dl_text(t, "%s{\"zone\":%u,\"name\":", (i > (uint32_t)from) ? "," : "", (unsigned)i);
		http_json_str(t, zone.name);
		dl_text(t, ",\"channels\":%u}", zone.count);
	}
	next_field(t, i, count);
	return 200;
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

// A channel's fields after its "pos" and "number": the one structure /api/zone's items and /api/channel share (and a
// VFO setting would take): what a client needs to show it and know what it is
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
		contact_t c;
		if (ch->contact != 0 && codeplug_contact(ch->contact, &c)) {
			static const char *const kinds[] = { "group", "private", "all" };
			dl_text(t, ",\"tg\":%u,\"call\":\"%s\",\"contact\":", (unsigned)c.id,
				(c.type < 3u) ? kinds[c.type] : "?");
			http_json_str(t, c.name);
		}
	}
#endif
}

// One channel of a zone, as a list item
static void channel_json(dl_text_t *t, bool first, uint16_t pos, uint16_t number)
{
	channel_t ch;
	dl_text(t, "%s{\"pos\":%u,\"number\":%u", first ? "" : ",", pos, number);
	if (codeplug_channel(number, &ch)) {
		channel_fields(t, &ch);
	}
	dl_text(t, "}");
}

// GET /api/channel: what the radio is on now, a channel or the VFO (its settings, "vfo":true; zone and pos are then
// the channel it came from). GET /api/channel?zone=Z&pos=P: that channel. One object, a zone item's fields
static int channel_req(const char *q, dl_text_t *t)
{
	int32_t z, p;
	if (q == NULL || *q == '\0') {
		const radio_view_t *v = net_radio_view();
		dl_text(t, "{\"zone\":%u,\"pos\":%u,\"vfo\":%s,\"number\":%u", v->zone_index, v->zone_pos,
			v->vfo ? "true" : "false", v->channel_number);
		channel_fields(t, &v->ch);
		dl_text(t, "}\n");
		return 200;
	}
	if (!query_int(q, "zone", &z) || !query_int(q, "pos", &p) || z < 0 || z >= codeplug_zone_count() || p < 0 ||
	    !codeplug_zone((uint16_t)z, &zone) || p >= zone.count) {
		return 400;
	}
	channel_t ch;
	dl_text(t, "{\"zone\":%u,\"pos\":%u,\"number\":%u", (unsigned)z, (unsigned)p, zone.channels[p]);
	if (codeplug_channel(zone.channels[p], &ch)) {
		channel_fields(t, &ch);
	}
	dl_text(t, "}\n");
	return 200;
}

// GET /api/zone?zone=Z[&from=N]: {"zone":Z,"name":..,"channels":n,"from":N,"items":[...],"next":..}
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
	dl_text(t, ",\"channels\":%u,\"from\":%u,\"items\":[", zone.count, (unsigned)from);
	uint32_t i = (uint32_t)from;
	for (; i < zone.count && t->len + ROOM_ITEM < t->cap; i++) {
		channel_json(t, i == (uint32_t)from, (uint16_t)i, zone.channels[i]);
	}
	next_field(t, i, zone.count);
	return 200;
}

// The radio would take a zone or channel request now (as radio_get_zone_pos: not transmitting, not a hotspot)
static bool can_select(void)
{
	const radio_view_t *v = net_radio_view();
	return v->state != RS_TX && !v->hotspot;
}

static void apply(uint32_t now)
{
	const radio_view_t *v = net_radio_view();
	if ((now - pend.last_req_ms) < REQ_GAP_MS) {
		return;
	}
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
		pend.zone_due = pend.pos_due = false;
		pend.last_req_ms = now;
	} else if (pend.channel_steps != 0) {
		if (v->vfo) {
			radio_req_vfo(false);	// channel steps are channels: back to the channel first
		}
		radio_req_channel_step((int8_t)((pend.channel_steps > 127) ? 127 : (pend.channel_steps < -127) ? -127 :
						pend.channel_steps));
		pend.channel_steps = 0;
		pend.last_req_ms = now;
	}
}

// GET /api/select?zone=Z[&pos=P]: that zone (its first channel, or position P), leaving the VFO. GET /api/select
// alone reads the two (http_radio)
static int select_req(const char *q, bool head, uint32_t now)
{
	int32_t z, p = -1;
	if (q == NULL || !query_int(q, "zone", &z) || z < 0 || z >= codeplug_zone_count() ||
	    (strstr(q, "pos=") != NULL && !query_int(q, "pos", &p))) {
		return 400;
	}
	if (p >= 0) {
		// the current zone's size from the view: the frequent case reads no flash
		const radio_view_t *v = net_radio_view();
		uint16_t size = (z == v->zone_index && !v->vfo) ? v->zone_size :
			codeplug_zone((uint16_t)z, &zone) ? zone.count : 0;
		if (p >= size) {
			return 400;
		}
	}
	if (p < -1) {
		return 400;
	}
	if (!can_select()) {
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

// GET /api/step?channel=N or ?zone=N: N channels or zones on (negative: back), wrapping at the ends
static int step_req(const char *q, bool head, uint32_t now)
{
	int32_t n;
	bool zones = q != NULL && strncmp(q, "zone=", 5) == 0;
	if (q == NULL || !query_int(q, zones ? "zone" : "channel", &n) || n == 0 || n > STEP_MAX || n < -STEP_MAX) {
		return 400;
	}
	if (!can_select()) {
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
		uint16_t base = (pend.zone_asked && (now - pend.zone_asked_ms) < ZONE_HOLD_MS) ? pend.zone_to :
			net_radio_view()->zone_index;
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
	int code;
	if (strcmp(path, "/api/zones") == 0) {
		st.lists++;
		return zones_json(query, t);
	}
	if (strcmp(path, "/api/zone") == 0) {
		st.lists++;
		return zone_json(query, t);
	}
	if (strcmp(path, "/api/channel") == 0) {
		st.lists++;
		return channel_req(query, t);
	}
	bool sel = strcmp(path, "/api/select") == 0;
	if (!sel && strcmp(path, "/api/step") != 0) {
		return 0;
	}
	if (sel && (query == NULL || *query == '\0')) {
		// where the radio is: two indexes, read often (a knob's display), so nothing else
		const radio_view_t *v = net_radio_view();
		dl_text(t, "{\"zone\":%u,\"pos\":%u%s}\n", v->zone_index, v->zone_pos, v->vfo ? ",\"vfo\":true" : "");
		return 200;
	}
	if (from_ip != ipGatewayHostIP()) {
		return 403;	// control only from the host (USB network or PPP), as rigctld's PTT and web control
	}
	code = sel ? select_req(query, head, now) : step_req(query, head, now);
	if (code == 200) {
		dl_text(t, "{\"ok\":true}\n");
	}
	return code;
}

void http_radio_poll(uint32_t now_ms)
{
	apply(now_ms);
}

void http_radio_view(dl_text_t *t)
{
	dl_text(t, "http.radio_lists=%u\nhttp.radio_selects=%u\nhttp.radio_steps=%u\nhttp.radio_refused=%u\n",
		(unsigned)st.lists, (unsigned)st.selects, (unsigned)st.steps, (unsigned)st.refused);
}

#endif	// CONFIG_NET_RADIO_API
