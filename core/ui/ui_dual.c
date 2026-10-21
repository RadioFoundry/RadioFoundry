// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Dual watch (docs/ux/dual-watch.md): two channels on Home, A and B, each a channel of any zone or the VFO. One of
// them is selected: PTT transmits on it and the watch goes on after, and Home's keys (Up / Down, the knob, digits,
// quick settings) act on it as they act on the one channel of the single Home. The A/B key, Left / Right or # 1
// select the other. The panes come from the codeplug, not the radio view: the view follows the receiver, which
// alternates between the two on a radio with one
#include "config.h"
#if CONFIG_DUAL_WATCH
#include <string.h>
#include "ui_nav.h"
#include "ui_svc.h"
#include "fmt.h"
#include "font.h"
#include "codeplug/codeplug.h"

#define START_GRACE_MS	500	// the radio view may still show no watch this soon after asking for one
#define PICK_ZONES	0xFFFFu	// SCR_DUAL_ZONE's "zone" for its own list of zones

static ui_dual_ref_t ref[2];
static bool b_known;		// a B channel was picked since power on: the Scan chooser offers it again
static uint32_t started_ms;
static bool was_tx;		// the radio transmitted since the last tick (today's core ends the watch then)
static uint16_t pick_zone;	// the zone SCR_DUAL_PICK lists
static char caller_buf[40];

static ui_dual_t *dual(void) { return &ui_st.dual; }

void ui_dual_reset(void)
{
	memset(ref, 0, sizeof ref);
	b_known = false;
	was_tx = false;
	started_ms = 0;
}

static void ref_here(ui_dual_ref_t *r)
{
	const radio_view_t *v = ui_view();
	r->vfo = v->vfo;
	r->zone = v->zone_index;
	r->pos = v->zone_pos;
}

static bool ref_same(const ui_dual_ref_t *a, const ui_dual_ref_t *b)
{
	return a->vfo == b->vfo && (a->vfo || (a->zone == b->zone && a->pos == b->pos));
}

// The pane the radio is tuned to now, UI_DUAL_NONE for neither
static uint8_t tuned_pane(const radio_view_t *v)
{
	ui_dual_ref_t here;
	ref_here(&here);
	(void)v;
	for (uint8_t i = 0; i < 2; i++)
		if (ref_same(&ref[i], &here))
			return i;
	return UI_DUAL_NONE;
}

static void pane_fill(uint8_t i)
{
	static channel_t ch;	// static: not on the UI task's stack
	static zone_t z;
	ui_dual_pane_t *p = &dual()->pane[i];
	memset(p, 0, sizeof *p);
	memset(&ch, 0, sizeof ch);
	if (ref[i].vfo) {
		p->vfo = true;
		strcpy(p->zone, "VFO");
		if (!ui_svc_vfo_get(&ch) && ui_view()->vfo)
			ch = ui_view()->ch;
	} else {
		if (!codeplug_zone(ref[i].zone, &z) || ref[i].pos >= z.count || !codeplug_channel(z.channels[ref[i].pos], &ch))
			return;
		p->number = z.channels[ref[i].pos];
		strcpy(p->zone, z.name);
		strcpy(p->name, ch.name);
	}
	p->mode = ch.mode;
	p->rx_hz = ch.rx_hz;
	sbuf_t b;
	sb_init(&b, p->detail, sizeof p->detail);
#if CONFIG_DMR
	if (ch.mode == RADIO_MODE_DMR) {
		static contact_t c;
		uint16_t talk = codeplug_channel_talk_contact(&ch);
		if (talk && codeplug_contact(talk, &c)) {
			ui_target_t t;
			memset(&t, 0, sizeof t);
			t.id = c.id;
			t.group = c.type != CONTACT_PRIVATE;
			strcpy(t.name, c.name);
			ui_sb_target(p->target, sizeof p->target, &t);
		}
		sb_str(&b, "TS");
		sb_u32(&b, ch.timeslot ? ch.timeslot : 1u, 1);
		sb_str(&b, " CC");
		sb_u32(&b, ch.color_code, 1);
		return;
	}
#endif
	if (ch.rx_tone.type == TONE_NONE && ch.tx_tone.type == TONE_NONE) {
		sb_str(&b, "No tone");
	} else {
		const subtone_t *t = ch.tx_tone.type != TONE_NONE ? &ch.tx_tone : &ch.rx_tone;
		if (t->type == TONE_CTCSS)
			sb_chr(&b, 'T');
		sb_tone(&b, t);
	}
	sb_str(&b, ch.bw == RADIO_BW_25K ? " 25k" : " 12.5k");
}

// Ask the radio for the watch as ref[] and sel say; false (and a toast saying what to do) when it can't
static bool apply(uint8_t sel)
{
	if (!ui_svc_dual(ref, sel)) {
		ui_toast(TEXT_ALT_OPEN "This radio watches 2 channels of one zone" TEXT_ALT_NEXT "Pick B from this zone"
			 TEXT_ALT_CLOSE);
		return false;
	}
	dual()->sel = sel;
	started_ms = ui_st.now_ms;
	return true;
}

bool ui_dual_start(const ui_dual_ref_t *b)
{
	ui_dual_t *d = dual();
	ui_dual_ref_t keep[2] = { ref[0], ref[1] };
	ref_here(&ref[0]);
	ref[1] = *b;
	if (ref_same(&ref[0], &ref[1])) {
		ui_toast("B is the channel you are on: pick another");
		ref[0] = keep[0];
		ref[1] = keep[1];
		return false;
	}
	if (ui_st.scanning) {
		ui_svc_scan(false);
		ui_st.scanning = false;
	}
	if (!apply(0)) {
		ref[0] = keep[0];
		ref[1] = keep[1];
		return false;
	}
	b_known = true;
	d->on = true;
	d->both = ui_svc_receivers() > 1;
	d->rx = d->last = UI_DUAL_NONE;
	d->caller[0] = 0;
	d->calls = 0;
	pane_fill(0);
	pane_fill(1);
	ui_toast(d->both ? "Dual watch: hearing both" : "Dual watch on: alternating");
	return true;
}

// Off: the radio stays on the selected channel
void ui_dual_stop(void)
{
	ui_dual_t *d = dual();
	if (!d->on)
		return;
	const ui_dual_ref_t *r = &ref[d->sel];
	ui_svc_dual(NULL, 0);
	d->on = false;
	const radio_view_t *v = ui_view();
	if (r->vfo) {
		if (!v->vfo)
			radio_req_vfo(true);
	} else {
		if (v->vfo)
			radio_req_vfo(false);
		if (v->zone_index != r->zone)
			radio_req_zone(r->zone);
		radio_req_zone_pos(r->pos);
	}
	ui_toast(d->sel ? "Dual watch off: on B" : "Dual watch off: on A");
}

void ui_dual_select(uint8_t pane)
{
	ui_dual_t *d = dual();
	if (!d->on || pane == d->sel)
		return;
	if (apply(pane))
		ui_toast(pane ? "Talking on B" : "Talking on A");
}

// Pane `pane` becomes r (a channel of any zone, or the VFO); starts the watch when it is off
static void set_pane(uint8_t pane, const ui_dual_ref_t *r)
{
	ui_dual_t *d = dual();
	if (!d->on) {
		ui_dual_start(r);
		return;
	}
	if (ref_same(r, &ref[!pane])) {
		ui_toast(pane ? "That is A: pick another" : "That is B: pick another");
		return;
	}
	ui_dual_ref_t old = ref[pane];
	ref[pane] = *r;
	if (!apply(d->sel)) {
		ref[pane] = old;
		return;
	}
	if (pane == 1)
		b_known = true;
	if (d->last == pane)
		d->last = UI_DUAL_NONE;	// the last call was on the old channel
	uint8_t keep = 0;		// and so were its calls in the list
	for (uint8_t k = 0; k < d->calls; k++)
		if (d->hist[k].pane != pane)
			d->hist[keep++] = d->hist[k];
	d->calls = keep;
	pane_fill(pane);
}

// Up / Down and the knob on Home: the selected channel moves through its zone (the other channel is skipped), or the
// VFO steps its frequency
static void step_selected(int dir)
{
	ui_dual_t *d = dual();
	ui_dual_ref_t *r = &ref[d->sel];
	if (r->vfo) {
		radio_req_vfo_step((int8_t)dir);
		apply(d->sel);
		pane_fill(d->sel);
		return;
	}
	static zone_t z;
	if (!codeplug_zone(r->zone, &z) || z.count < 2)
		return;
	ui_dual_ref_t next = *r;
	for (uint16_t n = 0; n < z.count; n++) {
		next.pos = (uint16_t)((next.pos + z.count + dir) % z.count);
		if (!ref_same(&next, &ref[!d->sel]))
			break;
	}
	ui_dual_ref_t old = *r;
	*r = next;
	if (!apply(d->sel))
		*r = old;
	pane_fill(d->sel);
}

bool ui_dual_home_key(const input_event_t *ev)
{
	ui_dual_t *d = dual();
	if (!d->on)
		return false;
	bool press = ev->gesture == GESTURE_PRESS;
	bool nav = ev->gesture == GESTURE_PRESS || ev->gesture == GESTURE_REPEAT || ev->gesture == GESTURE_LONG;
	switch (ev->action) {
	case ACT_NAV_UP:
	case ACT_NAV_DOWN:
		if (!nav || ui_st.entry)
			return false;
		step_selected(ev->action == ACT_NAV_UP ? 1 : -1);
		return true;
	case ACT_ENC_STEP:
		if (ui_st.entry)
			return false;
		for (int n = ev->value < 0 ? -ev->value : ev->value; n > 0; n--)
			step_selected(ev->value > 0 ? 1 : -1);
		return true;
	case ACT_NAV_LEFT:
	case ACT_NAV_RIGHT:
		if (!press)
			return false;
		ui_dual_select(ev->action == ACT_NAV_RIGHT ? 1 : 0);
		return true;
	case ACT_CONTEXT:
		if (!press)
			return false;
		ui_push(SCR_DUAL, 0);
		return true;
	case ACT_BACK:
		// A banner and VFO entry go first, as on the single Home
		if (!press || ui_st.entry || ui_st.banner.kind != UI_BANNER_NONE)
			return false;
		ui_dual_stop();
		return true;
	default:
		return false;
	}
}

// The A/B key (a side key function, or a board's A/B key): the other channel; dual watch starts when it is off
void ui_dual_ab_key(void)
{
	ui_dual_t *d = dual();
	if (d->on)
		ui_dual_select(!d->sel);
	else if (b_known)
		ui_dual_start(&ref[1]);
	else
		ui_push(SCR_DUAL_PICK, 1);
}

// The Dual watch key (a side key function): on with the last B (or the picker), or off
void ui_dual_key(void)
{
	if (dual()->on)
		ui_dual_stop();
	else if (b_known)
		ui_dual_start(&ref[1]);
	else
		ui_push(SCR_DUAL_PICK, 1);
}

static void caller_text(const radio_view_t *v)
{
	sbuf_t b;
	sb_init(&b, caller_buf, sizeof caller_buf);
#if CONFIG_DMR
	if (v->ch.mode == RADIO_MODE_DMR && (v->dmr.call.src || v->dmr.call.dst)) {
		const radio_dmr_call_t *c = &v->dmr.call;
		if (c->src_name[0])
			sb_str(&b, c->src_name);
		else
			sb_u32(&b, c->src, 1);
		if (c->group) {
			sb_str(&b, " TG ");
			sb_u32(&b, c->dst, 1);
		} else {
			sb_str(&b, " to you");
		}
		return;
	}
#endif
	sb_str(&b, v->ch.mode == RADIO_MODE_DMR ? "DMR call" : "FM signal");
}

// Every UI tick: which pane hears a call, the last call's pane, the watch going on after a transmission. True: redraw
bool ui_dual_tick(const radio_view_t *v, uint32_t now)
{
	ui_dual_t *d = dual();
	if (!d->on)
		return false;
	bool dirty = false;
	if (v->state == RS_TX)
		was_tx = true;
	if (v->scan != 3 /* SCAN_DUAL */ && now - started_ms >= START_GRACE_MS && v->state != RS_TX) {
		if (was_tx) {
			apply(d->sel);	// today's radio core ends the watch on PTT; the watch goes on (#96 moves this to the core)
		} else {
			d->on = false;	// ended by the radio (hotspot mode, a channel change from the debug link)
			ui_toast("Dual watch stopped");
			return true;
		}
	}
	if (v->state != RS_TX)
		was_tx = false;
	uint8_t rx = UI_DUAL_NONE;
	if (v->state == RS_RX)
		rx = tuned_pane(v);
	if (rx != UI_DUAL_NONE) {
		caller_text(v);
		if (strcmp(caller_buf, d->caller) != 0) {
			strcpy(d->caller, caller_buf);
			dirty = true;
		}
	}
	if (rx != d->rx) {
		if (d->rx != UI_DUAL_NONE) {
			d->last = d->rx;
			d->last_ms = now;
			const unsigned n = sizeof d->hist / sizeof d->hist[0];
			memmove(&d->hist[1], &d->hist[0], (n - 1) * sizeof d->hist[0]);
			d->hist[0].pane = d->rx;
			d->hist[0].ms = now;
			strncpy(d->hist[0].caller, d->caller, sizeof d->hist[0].caller - 1);
			d->hist[0].caller[sizeof d->hist[0].caller - 1] = 0;
			if (d->calls < n)
				d->calls++;
		}
		d->rx = rx;
		dirty = true;
	}
	return dirty;
}

// ======================= Scan: dual watch or the zone (quick 7, Tools > Scan) =======================

static uint16_t watch_count(void) { return 2; }

static void watch_item(uint16_t i, ui_item_t *it)
{
	item_num_set(it, (uint16_t)(i + 1u));
	if (i == 0) {
		strcpy(it->label, "Dual watch");
		sbuf_t b;
		sb_init(&b, it->value, sizeof it->value);
		if (b_known) {
			sb_str(&b, "+ ");
			if (ref[1].vfo)
				sb_str(&b, "VFO");
			else
				sb_str(&b, dual()->pane[1].name);
		} else {
			sb_str(&b, "Pick B");
		}
	} else {
		strcpy(it->label, ui_view()->vfo ? "Scan the band" : "Scan this zone");
	}
}

static void watch_select(uint16_t i, bool hold)
{
	(void)hold;
	if (i == 0) {
		if (b_known) {
			ui_go_home();
			ui_dual_start(&ref[1]);
		} else {
			ui_push(SCR_DUAL_PICK, 1);
		}
	} else {
		ui_go_home();
		ui_scan_toggle();
	}
}

static void watch_context(uint16_t i)
{
	if (i == 0)
		ui_push(SCR_DUAL_PICK, 1);	// another B
}

const ui_scr_def_t ui_watch_screen = {
	.name = "watch", .flags = SF_LIST | SF_NUMBERED | SF_FIELD, .prefix = "Scan",
	.hint = TEXT_ALT_OPEN "1-2 start  # change B  Back" TEXT_ALT_NEXT "1-2 start  # change B" TEXT_ALT_CLOSE,
	.count = watch_count, .item = watch_item, .select = watch_select, .context = watch_context,
};

// ======================= Dual watch actions (# on Home while it runs) =======================

enum { DA_SELECT, DA_CHANGE_A, DA_CHANGE_B, DA_CALLER, DA_STOP, DA_COUNT };

static uint16_t actions_count(void) { return DA_COUNT; }

static void pane_value(sbuf_t *b, uint8_t i)
{
	const ui_dual_pane_t *p = &dual()->pane[i];
	if (p->vfo) {
		sb_str(b, "VFO ");
		sb_freq(b, p->rx_hz);
	} else {
		sb_u32(b, p->number, 1);
		sb_chr(b, ' ');
		sb_str(b, p->name);
	}
}

static void actions_item(uint16_t i, ui_item_t *it)
{
	const ui_dual_t *d = dual();
	uint8_t other = !d->sel;
	sbuf_t b, vb;
	sb_init(&b, it->label, sizeof it->label);
	sb_init(&vb, it->value, sizeof it->value);
	item_num_set(it, (uint16_t)(i + 1u));
	switch (i) {
	case DA_SELECT:
		sb_str(&b, other ? "Talk on B" : "Talk on A");
		pane_value(&vb, other);
		break;
	case DA_CHANGE_A:
		sb_str(&b, "Change A");
		pane_value(&vb, 0);
		break;
	case DA_CHANGE_B:
		sb_str(&b, "Change B");
		pane_value(&vb, 1);
		break;
	case DA_CALLER:
		sb_str(&b, "Caller");
		if (d->caller[0])
			sb_str(&vb, d->caller);
		else
			it->flags |= UI_ITEM_DIM;
		break;
	case DA_STOP:
		sb_str(&b, d->sel ? "Stop, stay on B" : "Stop, stay on A");
		break;
	}
}

static void actions_select(uint16_t i, bool hold)
{
	(void)hold;
	switch (i) {
	case DA_SELECT:
		ui_go_home();
		ui_dual_select(!dual()->sel);
		break;
	case DA_CHANGE_A:
	case DA_CHANGE_B:
		ui_push(SCR_DUAL_PICK, i == DA_CHANGE_B);
		break;
	case DA_CALLER:
		ui_open_caller_actions();
		break;
	case DA_STOP:
		ui_go_home();
		ui_dual_stop();
		break;
	}
}

const ui_scr_def_t ui_dual_screen = {
	.name = "dual", .flags = SF_LIST | SF_NUMBERED | SF_FIELD, .prefix = "Dual watch", .hint = "1-5 pick  Back",
	.count = actions_count, .item = actions_item, .select = actions_select,
};

// ======================= The channel picker for A or B (arg: the pane) =======================
// Raw row 0 is the zone (OK: another zone or the VFO), then the zone's channels. Digits filter by number and name

static uint16_t pick_raw_count(void)
{
	static zone_t z;
	return (uint16_t)(1u + (codeplug_zone(pick_zone, &z) ? z.count : 0u));
}

static bool pick_raw_label(uint16_t raw, char *buf, uint8_t len)
{
	static zone_t z;
	static channel_t ch;
	sbuf_t b;
	sb_init(&b, buf, len);
	if (raw == 0) {
		sb_str(&b, "Zone");
		return true;
	}
	if (!codeplug_zone(pick_zone, &z) || raw - 1u >= z.count || !codeplug_channel(z.channels[raw - 1u], &ch))
		return false;
	sb_u32(&b, z.channels[raw - 1u], 1);
	sb_chr(&b, ' ');
	sb_str(&b, ch.name);
	return true;
}

static void pick_enter(void)
{
	uint8_t pane = (uint8_t)ui_frame()->arg;
	const ui_dual_ref_t *r = &ref[pane];
	bool have = dual()->on || (pane == 1 && b_known);
	pick_zone = have && !r->vfo ? r->zone : ui_view()->zone_index;
	ui_filter_reset();
	// Focus the pane's channel, else the first channel after the zone row
	ui_frame()->focus = ui_filter_count() > 1 ? 1 : 0;
	for (uint16_t i = 0; have && !r->vfo && i < ui_filter_count(); i++)
		if (ui_filter_raw(i) == r->pos + 1u)
			ui_frame()->focus = i;
}

static void pick_item(uint16_t i, ui_item_t *it)
{
	static zone_t z;
	static channel_t ch;
	uint16_t raw = ui_filter_raw(i);
	uint8_t pane = (uint8_t)ui_frame()->arg;
	const ui_dual_t *d = dual();
	sbuf_t b;
	if (raw == 0) {
		pick_raw_label(0, it->label, sizeof it->label);
		if (codeplug_zone(pick_zone, &z)) {
			sb_init(&b, it->value, sizeof it->value);
			sb_str(&b, z.name);
		}
		return;
	}
	if (!codeplug_zone(pick_zone, &z) || raw - 1u >= z.count || !codeplug_channel(z.channels[raw - 1u], &ch))
		return;
	pick_raw_label(raw, it->label, sizeof it->label);
	sb_init(&b, it->value, sizeof it->value);
	ui_dual_ref_t r = { false, pick_zone, (uint16_t)(raw - 1u) };
	// The other channel can't be picked for this pane: named so (when the watch starts, A is the channel on view)
	ui_dual_ref_t other;
	if (d->on)
		other = ref[!pane];
	else
		ref_here(&other);
	bool on_a = ref_same(&r, &other);
	if (on_a) {
		sb_str(&b, pane ? "A" : "B");
		it->flags |= UI_ITEM_DIM;
		return;
	}
	sb_freq(&b, ch.rx_hz);
	sb_str(&b, ch.mode == RADIO_MODE_DMR ? " DMR" : " FM");
	if (d->on && ref_same(&r, &ref[pane]))
		it->flags |= UI_ITEM_TEMP;	// the pane's channel now
}

static void pick_select(uint16_t i, bool hold)
{
	(void)hold;
	uint16_t raw = ui_filter_raw(i);
	uint8_t pane = (uint8_t)ui_frame()->arg;
	if (raw == 0) {
		ui_push(SCR_DUAL_ZONE, pane);
		return;
	}
	ui_dual_ref_t r = { false, pick_zone, (uint16_t)(raw - 1u) };
	ui_go_home();
	set_pane(pane, &r);
}

static const char *pick_title(void)
{
	return ui_frame()->arg ? "B" : "A";
}

const ui_scr_def_t ui_dual_pick_screen = {
	.name = "dual_pick", .flags = SF_LIST | SF_FILTER | SF_FIELD, .prefix = "Watch on",
	.hint = TEXT_ALT_OPEN "OK watch  0-9 find  Back" TEXT_ALT_NEXT "OK watch  Back" TEXT_ALT_CLOSE,
	.count = ui_filter_count, .item = pick_item, .select = pick_select, .title = pick_title,
	.enter = pick_enter, .raw_count = pick_raw_count, .raw_label = pick_raw_label,
};

// ======================= The zone for the picker, or the VFO =======================

static uint16_t zone_count(void) { return (uint16_t)(1u + codeplug_zone_count()); }

static void zone_item(uint16_t i, ui_item_t *it)
{
	static zone_t z;
	if (i == 0) {
		strcpy(it->label, "VFO");
		channel_t ch;
		if (ui_svc_vfo_get(&ch)) {
			sbuf_t b;
			sb_init(&b, it->value, sizeof it->value);
			sb_freq(&b, ch.rx_hz);
		}
		return;
	}
	if (!codeplug_zone((uint16_t)(i - 1u), &z))
		return;
	strcpy(it->label, z.name);
	if (i - 1u == pick_zone)
		it->flags |= UI_ITEM_TEMP;
}

static void zone_enter(void)
{
	ui_frame()->focus = (uint16_t)(pick_zone + 1u);
}

static void zone_select(uint16_t i, bool hold)
{
	(void)hold;
	uint8_t pane = (uint8_t)ui_frame()->arg;
	if (i == 0) {
		ui_dual_ref_t r = { true, 0, 0 };
		ui_go_home();
		set_pane(pane, &r);
		return;
	}
	pick_zone = (uint16_t)(i - 1u);
	ui_pop();	// back to the picker, now on that zone
	ui_filter_reset();
	ui_frame()->focus = ui_filter_count() > 1 ? 1 : 0;
	ui_frame()->top = 0;
}

static const char *zone_title(void)
{
	return ui_frame()->arg ? "B" : "A";
}

const ui_scr_def_t ui_dual_zone_screen = {
	.name = "dual_zone", .flags = SF_LIST | SF_FIELD, .prefix = "Zone for",
	.hint = "OK pick  Back", .count = zone_count, .item = zone_item, .select = zone_select, .enter = zone_enter,
	.title = zone_title,
};
#endif
