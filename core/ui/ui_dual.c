// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
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
#include "gfx.h"
#include "codeplug/codeplug.h"

#define START_GRACE_MS	500	// the radio view may still show no watch this soon after asking for one
#define BACK_AGAIN_MS	2000	// Back on Home, then Back again within this: the watch stops
#define TALK_BACK_MS	10000	// talk back: PTT stays on the other channel this long after its call or our reply
#define PICK_ZONES	0xFFFFu	// SCR_DUAL_ZONE's "zone" for its own list of zones

static ui_dual_ref_t ref[2];
static bool b_known;		// ref[1] holds the last B (picked, or the saved one found: b_last)
static bool a_known;		// ref[0] holds the last A (a watch ran since power on)
static uint32_t started_ms;
static bool was_tx;		// the radio transmitted since the last tick (today's core ends the watch then)
static uint16_t pick_zone;	// the zone SCR_DUAL_PICK lists
static char caller_buf[48];
static uint32_t back_ms;	// Back pressed once on Home (0: not lately)
static uint32_t cp_serial;	// the codeplug the panes were found in
static bool resume;		// power on: the watch comes back if it ran at power off (UI_DUAL_ON)
static ui_dual_ref_t chan[2];	// a pane's channel before it became the VFO: the VFO key goes back to it
static bool chan_ok[2];
#if CONFIG_ZONE_EDIT
static uint16_t chan_num[2];	// its channel number: found again by it when the codeplug changes
static uint32_t zones_seen;	// zone_edit_shifts() when the zone indexes here were last followed
static uint32_t serial_seen;	// codeplug_serial() then (the watch on: find_again keeps its own)
#endif
static uint8_t stops_seen, sel_seen;	// the view's dual_stops and dual_sel at the last tick (seen_ok: once read)
static bool seen_ok;
static uint32_t off_ms;		// the UI stopped the watch (the radio view may still show it this soon after)

// The radio's view of a pane, the last time the receiver was on it: the selected channel as the screens and the
// quick settings show it (its changes for now included) while the receiver is on the other one
typedef struct {
	bool valid;
	bool vfo, temporary, am, vox_on;
	uint16_t zone_index, zone_pos, zone_size, channel_number, tot_s;
	char zone_name[CODEPLUG_NAME_LEN + 1];
	channel_t ch;
	uint8_t power, temp, squelch;
#if CONFIG_DMR
	bool active, rmo, alias_tx, target_private, target_manual, hear_all_latch;
	uint8_t cc, ts;
	uint32_t target_id;
	char target_name[CODEPLUG_NAME_LEN + 1];
#endif
} snap_t;
static snap_t snap[2];
static bool refill[2];		// snap[i] changed what pane i shows

static ui_dual_t *dual(void) { return &ui_st.dual; }

void ui_dual_reset(void)
{
	memset(ref, 0, sizeof ref);
	b_known = false;
	a_known = false;
	was_tx = false;
	started_ms = 0;
	back_ms = 0;
	resume = true;
	memset(snap, 0, sizeof snap);
	memset(chan_ok, 0, sizeof chan_ok);
#if CONFIG_ZONE_EDIT
	zones_seen = zone_edit_shifts();
	serial_seen = codeplug_serial();
#endif
	seen_ok = false;
	off_ms = 0;
	dual()->back_to = UI_DUAL_NONE;
}

// A channel by its code (UI_SET_DUAL_B: a codeplug number, or the VFO), found in zone `first` first, then in any zone
static bool find_code(int32_t code, uint16_t first, ui_dual_ref_t *r)
{
	if (code == UI_DUAL_B_VFO || code == UI_DUAL_B_VFO2) {
		*r = (ui_dual_ref_t){ true, 0, (uint16_t)(code - UI_DUAL_B_VFO) };
		return true;
	}
	if (code <= 0 || code > CODEPLUG_CHANNELS_MAX)
		return false;
	static zone_t z;	// static: not on the UI task's stack
	uint16_t n = codeplug_zone_count();
	for (uint16_t k = 0; k < n; k++) {
		uint16_t zi = (uint16_t)((first + k) % n);
		if (!codeplug_zone(zi, &z))
			continue;
		for (uint16_t p = 0; p < z.count && p < CODEPLUG_ZONE_MAX; p++) {
			if (z.channels[p] == code) {
				*r = (ui_dual_ref_t){ false, zi, p };
				return true;
			}
		}
	}
	return false;
}

// The last B: picked since power on, else the saved one (UI_SET_DUAL_B), found in A's zone first, then in any zone.
// False: none, or its channel is gone
static bool b_last(void)
{
	if (b_known)
		return true;
	int32_t code = ui_svc_setting_get(UI_SET_DUAL_B);
	if (code & UI_DUAL_B_SEL)
		return false;	// the code is A's (a watch with B selected that didn't come back)
	return b_known = find_code(code & UI_DUAL_CODE, ui_view()->zone_index, &ref[1]);
}

static int32_t pane_code(uint8_t i)
{
	return ref[i].vfo ? UI_DUAL_B_VFO + ref[i].pos : dual()->pane[i].number;
}

// Kept across power off: B, or while the watch runs, that it runs, which pane is selected (the radio powers up on
// that one), and the other one. Talk back's selection is for now: the one before it is kept
static void state_save(void)
{
	const ui_dual_t *d = dual();
	uint8_t sel = d->back_to != UI_DUAL_NONE ? d->back_to : d->sel;
	if (!d->on)
		ui_svc_setting_set(UI_SET_DUAL_B, b_known ? pane_code(1) : 0);
	else
		ui_svc_setting_set(UI_SET_DUAL_B, pane_code(!sel) | UI_DUAL_ON | (sel ? UI_DUAL_B_SEL : 0));
}

static void ref_here(const radio_view_t *v, ui_dual_ref_t *r)
{
	r->vfo = v->vfo;
	r->zone = v->zone_index;
	r->pos = v->vfo ? v->vfo_index : v->zone_pos;
}

static bool ref_same(const ui_dual_ref_t *a, const ui_dual_ref_t *b)
{
	return a->vfo == b->vfo && a->pos == b->pos && (a->vfo || a->zone == b->zone);
}

// The VFO for pane i: each channel has its own. The one the radio last used, unless the other pane is on it
static ui_dual_ref_t vfo_for(uint8_t i)
{
	uint16_t u = ui_view()->vfo_index;
	if (ref[!i].vfo && ref[!i].pos == u)
		u = !u;
	return (ui_dual_ref_t){ true, 0, u };
}

// The pane the radio is tuned to now, UI_DUAL_NONE for neither
static uint8_t tuned_pane(const radio_view_t *v)
{
	ui_dual_ref_t here;
	ref_here(v, &here);
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
	const snap_t *s = &snap[i];
	// Calls and signal stay: only what the pane is changes
	bool rx = p->rx, heard = p->heard;
	int16_t rssi = p->rssi_dbm;
	static char caller[sizeof p->caller];
	strcpy(caller, p->caller);
	memset(p, 0, sizeof *p);
	p->rx = rx;
	p->heard = heard;
	p->rssi_dbm = rssi;
	strcpy(p->caller, caller);
	memset(&ch, 0, sizeof ch);
	if (ref[i].vfo) {
		p->vfo = true;
		strcpy(p->zone, "VFO");
		if (!ui_svc_vfo_get((uint8_t)ref[i].pos, &ch) && ui_view()->vfo && ui_view()->vfo_index == ref[i].pos)
			ch = ui_view()->ch;
	} else {
		if (!codeplug_zone(ref[i].zone, &z) || ref[i].pos >= z.count || !codeplug_channel(z.channels[ref[i].pos], &ch))
			return;
		p->number = z.channels[ref[i].pos];
		strcpy(p->zone, z.name);
		strcpy(p->name, ch.name);
	}
	if (s->valid)
		ch = s->ch;	// as the radio has it: changes for now included
	p->mode = ch.mode;
	p->rx_hz = ch.rx_hz;
	sbuf_t b;
	sb_init(&b, p->detail, sizeof p->detail);
#if CONFIG_DMR
	if (ch.mode == RADIO_MODE_DMR) {
		static contact_t c;
		uint16_t talk = codeplug_channel_talk_contact(&ch);
		if (s->valid && s->target_id) {
			ui_target_t t;
			memset(&t, 0, sizeof t);
			t.id = s->target_id;
			t.group = !s->target_private;
			strcpy(t.name, s->target_name);
			ui_sb_target(p->target, sizeof p->target, &t);
		} else if (talk && codeplug_contact(talk, &c)) {
			ui_target_t t;
			memset(&t, 0, sizeof t);
			t.id = c.id;
			t.group = c.type != CONTACT_PRIVATE;
			strcpy(t.name, c.name);
			ui_sb_target(p->target, sizeof p->target, &t);
		}
		sb_str(&b, "TS");
		sb_u32(&b, s->valid && s->ts ? s->ts : ch.timeslot ? ch.timeslot : 1u, 1);
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

static void snap_take(uint8_t i, const radio_view_t *v)
{
	snap_t *s = &snap[i];
	bool was = s->valid;
	uint8_t power = s->power;
	static channel_t ch;
	ch = s->ch;
#if CONFIG_DMR
	uint32_t target = s->target_id;
	uint8_t ts = s->ts;
#endif
	s->valid = true;
	s->vfo = v->vfo;
	s->temporary = v->temporary;
	s->am = v->am;
	s->vox_on = v->vox_on;
	s->zone_index = v->zone_index;
	s->zone_pos = v->zone_pos;
	s->zone_size = v->zone_size;
	s->channel_number = v->channel_number;
	s->tot_s = v->tot_s;
	memcpy(s->zone_name, v->zone_name, sizeof s->zone_name);
	s->ch = v->ch;
	s->power = v->power;
	s->temp = v->temp;
	s->squelch = v->squelch;
#if CONFIG_DMR
	s->active = v->dmr.active;
	s->rmo = v->dmr.rmo;
	s->alias_tx = v->dmr.alias_tx;
	s->target_private = v->dmr.target_private;
	s->target_manual = v->dmr.target_manual;
	s->hear_all_latch = v->dmr.hear_all_latch;
	s->cc = v->dmr.cc;
	s->ts = v->dmr.ts;
	s->target_id = v->dmr.target_id;
	memcpy(s->target_name, v->dmr.target_name, sizeof s->target_name);
	if (v->dmr.reply_active && !v->dmr.target_manual) {
		// A reply target lapses with its window, which runs out while the receiver is away: keep the channel's own
		static contact_t c;
		uint16_t talk = codeplug_channel_talk_contact(&v->ch);
		s->target_id = 0;
		s->target_name[0] = '\0';
		if (talk && codeplug_contact(talk, &c)) {
			s->target_id = c.id;
			s->target_private = c.type == CONTACT_PRIVATE;
			strcpy(s->target_name, c.name);
		}
	}
	if (target != s->target_id || ts != s->ts)
		refill[i] = true;
#endif
	if (!was || power != s->power || memcmp(&ch, &s->ch, sizeof ch) != 0)
		refill[i] = true;
}

static void snap_put(uint8_t i, radio_view_t *v)
{
	const snap_t *s = &snap[i];
	v->vfo = s->vfo;
	v->temporary = s->temporary;
	v->am = s->am;
	v->vox_on = s->vox_on;
	v->zone_index = s->zone_index;
	v->zone_pos = s->zone_pos;
	v->zone_size = s->zone_size;
	v->channel_number = s->channel_number;
	v->tot_s = s->tot_s;
	memcpy(v->zone_name, s->zone_name, sizeof v->zone_name);
	v->ch = s->ch;
	v->power = s->power;
	v->temp = s->temp;
	v->squelch = s->squelch;
#if CONFIG_DMR
	v->dmr.active = s->active;
	v->dmr.rmo = s->rmo;
	v->dmr.alias_tx = s->alias_tx;
	v->dmr.target_private = s->target_private;
	v->dmr.target_manual = s->target_manual;
	v->dmr.hear_all_latch = s->hear_all_latch;
	v->dmr.cc = s->cc;
	v->dmr.ts = s->ts;
	v->dmr.target_id = s->target_id;
	memcpy(v->dmr.target_name, s->target_name, sizeof v->dmr.target_name);
	v->dmr.reply_active = false;	// the reply window is the tuned channel's
#endif
}

// Pane i's settings before the receiver has been there (it stays on the other channel through a call there, and a new
// selection doesn't retune): the channel as stored, no changes for now. The next visit replaces it
static void snap_seed(uint8_t i, const radio_view_t *v)
{
	static zone_t z;
	snap_t *s = &snap[i];
	memset(s, 0, sizeof *s);
	s->vfo = ref[i].vfo;
	if (s->vfo) {
		if (!ui_svc_vfo_get((uint8_t)ref[i].pos, &s->ch))
			return;
		strcpy(s->zone_name, "VFO");
	} else {
		if (!codeplug_zone(ref[i].zone, &z) || ref[i].pos >= z.count || !codeplug_channel(z.channels[ref[i].pos], &s->ch))
			return;
		s->zone_index = ref[i].zone;
		s->zone_pos = ref[i].pos;
		s->zone_size = z.count;
		s->channel_number = z.channels[ref[i].pos];
		strcpy(s->zone_name, z.name);
	}
	s->valid = true;
	s->tot_s = v->tot_s;
	s->power = s->ch.power != CODEPLUG_POWER_DEFAULT ? s->ch.power : v->power;	// the default level, as on the other
	s->squelch = v->squelch;
#if CONFIG_DMR
	s->active = s->ch.mode == RADIO_MODE_DMR;
	s->cc = s->ch.color_code;
	s->ts = s->ch.timeslot ? s->ch.timeslot : 1u;
	static contact_t c;
	uint16_t talk = codeplug_channel_talk_contact(&s->ch);
	if (talk && codeplug_contact(talk, &c)) {
		s->target_id = c.id;
		s->target_private = c.type == CONTACT_PRIVATE;
		strcpy(s->target_name, c.name);
	}
#endif
}

// The view the UI works from: the radio's, but while the receiver is on the other channel, the selected channel's
// settings as the radio last had them. So the quick settings, the talk target, VFO entry and the menus act on the
// selected channel wherever the receiver is
const radio_view_t *ui_dual_view(const radio_view_t *v)
{
	static radio_view_t mixed;	// static: too big for the UI task's stack
	const ui_dual_t *d = dual();
	if (!d->on)
		return v;
	uint8_t t = tuned_pane(v);
	if (t != UI_DUAL_NONE)
		snap_take(t, v);
	if (t == d->sel || t == UI_DUAL_NONE)
		return v;
	if (!snap[d->sel].valid)
		snap_seed(d->sel, v);
	if (!snap[d->sel].valid)
		return v;
	mixed = *v;
	snap_put(d->sel, &mixed);
	return &mixed;
}

// Ask the radio for the watch as ref[] and sel say; false (and a toast saying what to do) when it can't
static bool apply(uint8_t sel)
{
	if (!ui_svc_dual(ref, sel)) {
		ui_toast(TEXT_ALT_OPEN "Can't watch that channel: pick another" TEXT_ALT_NEXT "Can't watch that channel"
			 TEXT_ALT_CLOSE);
		return false;
	}
	dual()->sel = sel;
	started_ms = ui_st.now_ms;
	return true;
}

// The watch is on, with ref[] as asked
static void watch_on(ui_dual_t *d)
{
	d->on = true;
	a_known = true;
	d->both = ui_svc_receivers() > 1;
	d->rx = d->last = d->back_to = UI_DUAL_NONE;
	d->ended[0] = d->ended[1] = false;
	d->caller[0] = 0;
	d->calls = 0;
	memset(snap, 0, sizeof snap);
	memset(&d->pane, 0, sizeof d->pane);
	pane_fill(0);
	pane_fill(1);
	cp_serial = codeplug_serial();
	back_ms = 0;
	state_save();
}

// B with no last B to use: the next channel of a's zone, else the first one of a later zone, else the VFO. False: none
static bool b_default(const ui_dual_ref_t *a, ui_dual_ref_t *out)
{
	static zone_t z;	// static: not on the UI task's stack
	static channel_t ch;
	uint16_t n = codeplug_zone_count();
	uint16_t first = a->vfo ? ui_view()->zone_index : a->zone;
	for (uint16_t k = 0; k < n; k++) {
		uint16_t zi = (uint16_t)((first + k) % n);
		if (!codeplug_zone(zi, &z) || z.count == 0)
			continue;
		uint16_t start = k == 0 && !a->vfo ? (uint16_t)(a->pos + 1u) : 0;
		for (uint16_t j = 0; j < z.count && j < CODEPLUG_ZONE_MAX; j++) {
			ui_dual_ref_t r = { false, zi, (uint16_t)((start + j) % z.count) };
			if (!ref_same(&r, a) && codeplug_channel(z.channels[r.pos], &ch)) {
				*out = r;
				return true;
			}
		}
	}
#if CONFIG_VFO
	if (!a->vfo) {
		*out = (ui_dual_ref_t){ true, 0, ui_view()->vfo_index };
		return true;
	}
#endif
	return false;
}

bool ui_dual_start(const ui_dual_ref_t *b)
{
	ui_dual_t *d = dual();
	ui_dual_ref_t keep[2] = { ref[0], ref[1] };
	ref_here(ui_view(), &ref[0]);
	ref[1] = *b;
	// The last B is where the radio is now (moved there since): the last A takes its place, else the next channel
	if (ref_same(&ref[0], &ref[1]) && a_known && !ref_same(&keep[0], &ref[0]))
		ref[1] = keep[0];
	else if (ref_same(&ref[0], &ref[1]) && !b_default(&ref[0], &ref[1]))
		ref[1] = ref[0];
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
	watch_on(d);
	ui_toast("Dual watch on");
	return true;
}

// Off: the radio stays on the selected channel
void ui_dual_stop(void)
{
	ui_dual_t *d = dual();
	if (!d->on)
		return;
	ui_svc_dual(NULL, 0);	// the radio stops on the selected channel
	off_ms = ui_st.now_ms;
	d->on = false;
	d->back_to = UI_DUAL_NONE;
	ui_toast(d->sel ? "Dual watch off: on B" : "Dual watch off: on A");
	if (d->sel) {
		// The radio stays on B: the next start has it as A and the old A as B, the same pair
		ui_dual_ref_t t = ref[0];
		ref[0] = ref[1];
		ref[1] = t;
		static ui_dual_pane_t p;	// static: not on the UI task's stack
		p = d->pane[0];
		d->pane[0] = d->pane[1];
		d->pane[1] = p;
		d->sel = 0;
	}
	state_save();
}

// Back on Home with nothing else to clear: the first press says how to stop the watch, a second one soon after stops it.
// Back is pressed without much thought; one press must not end it (docs/ux/dual-watch.md)
void ui_dual_back(void)
{
	if (back_ms != 0 && ui_st.now_ms - back_ms < BACK_AGAIN_MS) {
		back_ms = 0;
		ui_dual_stop();
		return;
	}
	back_ms = ui_st.now_ms != 0 ? ui_st.now_ms : 1;
	ui_toast(TEXT_ALT_OPEN "Press Back again to stop dual watch" TEXT_ALT_NEXT "Back again: stop watch"
		 TEXT_ALT_CLOSE);
}

void ui_dual_select(uint8_t pane)
{
	ui_dual_t *d = dual();
	if (!d->on || pane == d->sel)
		return;
	d->back_to = UI_DUAL_NONE;	// the user's choice ends talk back
	if (apply(pane)) {
		state_save();
		ui_toast(pane ? "Talking on B" : "Talking on A");
	}
}

// Talk back (the setting): a call ended on the other channel, so PTT talks there for TALK_BACK_MS, then goes back
static void talk_back(ui_dual_t *d, uint8_t pane, uint32_t now)
{
	uint8_t from = d->back_to != UI_DUAL_NONE ? d->back_to : d->sel;
	if (!apply(pane))
		return;
	d->back_to = from;
	d->back_ms = now + TALK_BACK_MS;
	ui_toast(pane ? "Talk back on B" : "Talk back on A");
}

static void talk_back_poll(ui_dual_t *d, const radio_view_t *v, uint32_t now)
{
	if (d->back_to == UI_DUAL_NONE)
		return;
	if (v->state == RS_TX || d->pane[d->sel].rx) {
		d->back_ms = now + TALK_BACK_MS;	// a reply, or the other station again: the time starts over
		return;
	}
	if ((int32_t)(now - d->back_ms) < 0)
		return;
	uint8_t to = d->back_to;
	d->back_to = UI_DUAL_NONE;
	if (to != d->sel && apply(to))
		ui_toast(to ? "Talking on B" : "Talking on A");
}

// Pane `pane` is another channel now (it was `old`): the last call and the calls list forget it, Home shows the new one
static void pane_changed(uint8_t pane, const ui_dual_ref_t *old)
{
	ui_dual_t *d = dual();
	if (pane == 1)
		b_known = true;
	if (!old->vfo && ref[pane].vfo) {
		chan[pane] = *old;	// the VFO key goes back to it
		chan_ok[pane] = true;
#if CONFIG_ZONE_EDIT
		static zone_t z;	// static: not on the UI task's stack
		chan_num[pane] = codeplug_zone(old->zone, &z) && old->pos < z.count ? z.channels[old->pos] : 0;
		chan_ok[pane] = chan_num[pane] != 0;
#endif
	}
	snap[pane].valid = false;
	if (d->last == pane)
		d->last = UI_DUAL_NONE;	// the last call was on the old channel
	d->ended[pane] = false;
	uint8_t keep = 0;		// and so were its calls in the list
	for (uint8_t k = 0; k < d->calls; k++)
		if (d->hist[k].pane != pane)
			d->hist[keep++] = d->hist[k];
	d->calls = keep;
	pane_fill(pane);
	state_save();
}

// Pane `pane` becomes r (a channel of any zone, or the VFO); starts the watch when it is off. False (with a toast)
// when it can't
static bool set_pane(uint8_t pane, const ui_dual_ref_t *r)
{
	ui_dual_t *d = dual();
	if (!d->on)
		return ui_dual_start(r);
	if (ref_same(r, &ref[!pane])) {
		ui_toast(pane ? "That is A: pick another" : "That is B: pick another");
		return false;
	}
	d->back_to = UI_DUAL_NONE;
	ui_dual_ref_t old = ref[pane];
	ref[pane] = *r;
	if (!apply(d->sel)) {
		ref[pane] = old;
		return false;
	}
	pane_changed(pane, &old);
	return true;
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
	if (!codeplug_zone(r->zone, &z))
		return;
	if (z.count < 2) {
		ui_toast(TEXT_ALT_OPEN "Only one channel in this zone" TEXT_ALT_NEXT "One channel in zone" TEXT_ALT_CLOSE);
		return;
	}
	ui_dual_ref_t next = *r;
	for (uint16_t n = 0; n < z.count; n++) {
		next.pos = (uint16_t)((next.pos + z.count + dir) % z.count);
		if (!ref_same(&next, &ref[!d->sel]))
			break;
	}
	if (ref_same(&next, r)) {	// the zone's only other channel is the other pane's
		ui_toast(d->sel ? TEXT_ALT_OPEN "No other channel here: A has it" TEXT_ALT_NEXT "A has the other channel"
				  TEXT_ALT_CLOSE :
				  TEXT_ALT_OPEN "No other channel here: B has it" TEXT_ALT_NEXT "B has the other channel"
				  TEXT_ALT_CLOSE);
		return;
	}
	ui_dual_ref_t old = *r;
	*r = next;
	if (!apply(d->sel)) {
		*r = old;
		return;
	}
	pane_changed(d->sel, &old);
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
		if (ui_st.updown == 1)	// Settings > Keys > Up/Down: Zone
			ui_dual_zone_step(ev->action == ACT_NAV_UP ? 1 : -1);
		else
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
	default:
		return false;
	}
}

// The A/B key (a side key function, or a board's A/B key): the other channel. It does nothing else: the Dual watch
// key (the same key held, on most radios) starts the watch
void ui_dual_ab_key(void)
{
	ui_dual_t *d = dual();
	if (d->on)
		ui_dual_select(!d->sel);
	else
		ui_toast(TEXT_ALT_OPEN "Turn on dual watch first" TEXT_ALT_NEXT "Dual watch is off" TEXT_ALT_CLOSE);
}

// The zone keys while it runs: the selected channel goes to the next or previous zone (from the VFO, the zone the
// radio was last on), on its first channel that isn't the other one. A zone with no such channel is passed over
static void zone_to(uint16_t zi, int dir)
{
	ui_dual_t *d = dual();
	uint16_t n = codeplug_zone_count();
	static zone_t z;	// static: not on the UI task's stack
	static char msg[CODEPLUG_NAME_LEN + 12];
	for (uint16_t k = 0; k < n; k++, zi = (uint16_t)((zi + n + dir) % n)) {
		if (!codeplug_zone(zi, &z))
			continue;
		ui_dual_ref_t r = { false, zi, 0 };
		while (r.pos < z.count && ref_same(&r, &ref[!d->sel]))
			r.pos++;
		if (r.pos >= z.count)
			continue;
		if (!ref_same(&r, &ref[d->sel]) && !set_pane(d->sel, &r))
			return;
		sbuf_t b;
		sb_init(&b, msg, sizeof msg);
		sb_str(&b, d->sel ? "B zone: " : "A zone: ");
		sb_str(&b, z.name);
		ui_toast(msg);
		return;
	}
	ui_toast("No other zone");
}

bool ui_dual_zone_step(int dir)
{
	ui_dual_t *d = dual();
	if (!d->on)
		return false;
	uint16_t n = codeplug_zone_count();
	if (n < 2 && !ref[d->sel].vfo) {
		ui_toast("Only one zone");
		return true;
	}
	const ui_dual_ref_t *r = &ref[d->sel];
	uint16_t from = r->vfo ? ui_view()->zone_index : r->zone;
	zone_to(r->vfo ? from : (uint16_t)((from + n + dir) % n), dir);
	return true;
}

bool ui_dual_zone_home(void)
{
	if (!dual()->on)
		return false;
	zone_to(0, 1);
	return true;
}

// Dual watch on with the last B (the first time, the next channel: B is changed as while it runs), or off: Quick
// tile 7, the Dual watch key, long SK2
void ui_dual_key(void)
{
	ui_go_home();
	if (dual()->on) {
		ui_dual_stop();
	} else if (b_last()) {
		ui_dual_start(&ref[1]);
	} else {
		ui_dual_ref_t here, b;
		ref_here(ui_view(), &here);
		if (b_default(&here, &b))
			ui_dual_start(&b);
		else
			ui_toast(TEXT_ALT_OPEN "No other channel to watch" TEXT_ALT_NEXT "No other channel" TEXT_ALT_CLOSE);
	}
}

// "Joe Smith TG 91", with "Joe Smith" as the shorter alternate (font.h): the caller's name gives way last
static void caller_text(char *out, size_t len, bool dmr, uint32_t src, const char *name, uint32_t dst, bool group)
{
	sbuf_t b;
	sb_init(&b, out, len);
	if (!dmr || (!src && !dst)) {
		sb_str(&b, dmr ? "DMR call" : "FM signal");
		return;
	}
	sb_str(&b, TEXT_ALT_OPEN);
	for (int alt = 0; alt < 2; alt++) {
		if (name[0])
			sb_str(&b, name);
		else
			sb_u32(&b, src, 1);
		if (alt == 0) {
			if (group) {
				sb_str(&b, " TG ");
				sb_u32(&b, dst, 1);
			} else {
				sb_str(&b, " to you");
			}
			sb_str(&b, TEXT_ALT_NEXT);
		}
	}
	sb_str(&b, TEXT_ALT_CLOSE);
}

// A call that ended on pane i goes to the top of the list
static void call_ended(ui_dual_t *d, uint8_t i, uint32_t now)
{
	const unsigned n = sizeof d->hist / sizeof d->hist[0];
	d->last = i;
	d->last_ms = now;
	d->ended[i] = true;
	d->ended_ms[i] = now;
	strcpy(d->caller, d->pane[i].caller);
	memmove(&d->hist[1], &d->hist[0], (n - 1) * sizeof d->hist[0]);
	d->hist[0].pane = i;
	d->hist[0].ms = now;
	strcpy(d->hist[0].caller, d->pane[i].caller);
	if (d->calls < n)
		d->calls++;
}

// The watch ends without the user only when it can't go on: then the toast says where the radio is
static void watch_lost(ui_dual_t *d, const radio_view_t *v)
{
	static char msg[CODEPLUG_NAME_LEN + 24];
	ui_svc_dual(NULL, 0);
	off_ms = ui_st.now_ms;
	d->on = false;
	d->back_to = UI_DUAL_NONE;
	state_save();
	sbuf_t b;
	sb_init(&b, msg, sizeof msg);
	sb_str(&b, "Dual watch off: on ");
	if (v->vfo)
		sb_freq(&b, v->ch.rx_hz);
	else
		sb_str(&b, v->ch.name);
	ui_toast(msg);
}

// The radio isn't watching: a change to the selected channel retuned it (a quick setting), it transmitted, hotspot
// mode ended, or a menu, the debug link or a codeplug write moved it. Only the user ends the watch, so it goes on: a
// move to the other channel selects it, a move to a third makes that the selected one. False: it couldn't go on
static bool watch_again(ui_dual_t *d, const radio_view_t *v)
{
	if (v->scan != 0) {	// a zone or band scan in its place (the debug link): asked for, so it stands
		d->on = false;
		d->back_to = UI_DUAL_NONE;
		state_save();
		ui_toast("Dual watch off: scanning");
		return false;
	}
	uint8_t t = tuned_pane(v);
	if (!was_tx && t != d->sel) {
		if (t != UI_DUAL_NONE) {
			d->sel = t;
			d->back_to = UI_DUAL_NONE;
			ui_toast(t ? "Talking on B" : "Talking on A");
		} else {
			ui_dual_ref_t old = ref[d->sel];
			ref_here(v, &ref[d->sel]);
			d->back_to = UI_DUAL_NONE;
			pane_changed(d->sel, &old);
		}
	}
	if (!ui_svc_dual(ref, d->sel)) {
		watch_lost(d, v);
		return false;
	}
	started_ms = ui_st.now_ms;
	state_save();
	return true;
}

#if CONFIG_ZONE_EDIT
// The zone editor moved zone indexes (zone_edit.h) or changed a zone: the references kept here follow. The panes'
// zones are mapped (find_again then finds their channels by number, starting there); the VFO key's channel is found
// again by number from its mapped zone; the last B and A, kept while the watch is off, are dropped when their zone
// was deleted or the codeplug changed, and B is then found again from the saved setting (a channel number, b_last)
static void follow_zones(const ui_dual_t *d)
{
	uint32_t to = zone_edit_shifts();
	bool changed = codeplug_serial() != serial_seen;
	if ((to == zones_seen && !changed) || zone_edit_busy())
		return;
	serial_seen = codeplug_serial();
	uint16_t out;
	for (uint8_t i = 0; i < 2; i++) {
		if (!ref[i].vfo && zone_edit_map(zones_seen, to, ref[i].zone, &out) != ZONE_MAP_LOST)
			ref[i].zone = out;
		if (chan_ok[i]) {
			if (zone_edit_map(zones_seen, to, chan[i].zone, &out) != ZONE_MAP_LOST)
				chan[i].zone = out;
			chan_ok[i] = find_code(chan_num[i], chan[i].zone, &chan[i]);
		}
	}
	if (zone_edit_map(zones_seen, to, pick_zone, &out) != ZONE_MAP_LOST)
		pick_zone = out;
	zones_seen = to;
	if (!d->on) {
		b_known = false;	// b_last finds it again by its number
		a_known = false;
	}
}
#endif

// The codeplug changed (the CPS, a channel edit): the panes' channels are found again by number. False: one is gone
// and the watch ended
static bool find_again(ui_dual_t *d)
{
	cp_serial = codeplug_serial();
	for (uint8_t i = 0; i < 2; i++) {
		if (ref[i].vfo)
			continue;
		ui_dual_ref_t r;
		if (!find_code(d->pane[i].number, ref[i].zone, &r) || ref_same(&r, &ref[!i])) {
			watch_lost(d, ui_view());
			return false;
		}
		ref[i] = r;
		pane_fill(i);
	}
	if (!ui_svc_dual(ref, d->sel)) {
		watch_lost(d, ui_view());
		return false;
	}
	started_ms = ui_st.now_ms;
	return true;
}

// Power on: the watch comes back as it was at power off, once the radio is on its channel (the selected one)
static bool resume_poll(ui_dual_t *d, const radio_view_t *v)
{
	if (!resume)
		return false;
	if (v->hotspot || (!v->vfo && v->zone_size == 0))
		return false;	// not on a channel yet
	resume = false;
	int32_t code = ui_svc_setting_get(UI_SET_DUAL_B);
	if (!(code & UI_DUAL_ON))
		return false;
	uint8_t sel = (code & UI_DUAL_B_SEL) ? 1 : 0;
	ui_dual_ref_t other;
	if (!find_code(code & UI_DUAL_CODE, v->zone_index, &other))
		return false;
	ref_here(v, &ref[sel]);
	ref[!sel] = other;
	if (ref_same(&ref[0], &ref[1]) || !ui_svc_dual(ref, sel))
		return false;
	d->sel = sel;
	started_ms = ui_st.now_ms;
	b_known = true;
	watch_on(d);
	ui_toast("Dual watch on");
	return true;
}

// The radio watches without the UI: another task (the debug link's scan dual) started it. It is the same watch as one
// started here, so Home shows it and the rules apply (only a stop asked for ends it)
static bool adopt_poll(ui_dual_t *d, const radio_view_t *v)
{
	ui_dual_ref_t r[2];
	uint8_t sel;
	if (v->scan != 3 /* SCAN_DUAL */ || ui_st.now_ms - off_ms < START_GRACE_MS || !ui_svc_dual_get(r, &sel))
		return false;
	ref[0] = r[0];
	ref[1] = r[1];
	d->sel = sel ? 1 : 0;
	b_known = true;
	started_ms = ui_st.now_ms;
	watch_on(d);
	ui_toast("Dual watch on");
	return true;
}

// The VFO key while it runs: the selected channel becomes its own VFO and back (to the channel it was), the watch goes
// on. False: the watch is off
bool ui_dual_vfo_key(void)
{
	ui_dual_t *d = dual();
	if (!d->on)
		return false;
	uint8_t sel = d->sel;
	if (!ref[sel].vfo) {
		ui_dual_ref_t vfo = vfo_for(sel);
		if (set_pane(sel, &vfo))
			ui_toast(sel ? "B: VFO" : "A: VFO");
		return true;
	}
	if (chan_ok[sel] && !ref_same(&chan[sel], &ref[!sel])) {
		ui_dual_ref_t r = chan[sel];
		if (set_pane(sel, &r))
			ui_toast("Back to channels");
		return true;
	}
	zone_to(ui_view()->zone_index, 1);	// the first channel of the last zone that isn't the other one
	return true;
}

// Every UI tick: which panes hear a call (both at once on a radio with two receivers), the last calls and their
// panes, the watch going on after a transmission. True: redraw
// Not inlined: its frame would add to ui_task's, which is on the UI's deepest drawing path (stack-check.md)
__attribute__((noinline)) bool ui_dual_tick(const radio_view_t *v, uint32_t now)
{
	ui_dual_t *d = dual();
	// Another task (the debug link) stopped the watch or selected the other channel: asked for, as a key would be
	bool stop_asked = seen_ok && v->dual_stops != stops_seen;
	bool sel_asked = seen_ok && v->dual_sel != sel_seen;
	stops_seen = v->dual_stops;
	sel_seen = v->dual_sel;
	seen_ok = true;
#if CONFIG_ZONE_EDIT
	if (zone_edit_busy())
		return false;	// a zone write under way: the codeplug and the zone indexes are read once it is done
	follow_zones(d);
#endif
	if (!d->on)
		return resume_poll(d, v) || adopt_poll(d, v);
	if (v->hotspot)
		return false;	// the host has the radio; the watch comes back after
	if (stop_asked && v->scan != 3 /* SCAN_DUAL */) {
		ui_dual_stop();
		return true;
	}
	bool dirty = false;
	// Followed even when the watch has ended since: a channel request right after the select ends it before this tick
	if (sel_asked && v->dual_sel != d->sel) {
		d->sel = v->dual_sel ? 1 : 0;
		d->back_to = UI_DUAL_NONE;
		state_save();
		ui_toast(d->sel ? "Talking on B" : "Talking on A");
		dirty = true;
	}
	if (v->state == RS_TX)
		was_tx = true;
	if (codeplug_serial() != cp_serial && !find_again(d))
		return true;
	if (v->scan != 3 /* SCAN_DUAL */ && now - started_ms >= START_GRACE_MS && v->state != RS_TX) {
		if (!watch_again(d, v))
			return true;
		dirty = true;
	}
	if (v->state != RS_TX)
		was_tx = false;
	for (uint8_t i = 0; i < 2; i++) {
		if (refill[i]) {
			refill[i] = false;
			pane_fill(i);
			dirty = true;
		}
	}

	// What each pane hears now: its own receiver's report, or the one receiver's on the pane it is tuned to
	static ui_dual_rx_t r[2];	// static: not on the UI task's stack
	memset(r, 0, sizeof r);
	bool own = d->both && ui_svc_dual_rx(0, &r[0]) && ui_svc_dual_rx(1, &r[1]);
	if (!own) {
		memset(r, 0, sizeof r);
		uint8_t t = v->state == RS_RX ? tuned_pane(v) : UI_DUAL_NONE;
		if (t != UI_DUAL_NONE) {
			ui_dual_rx_t *x = &r[t];
			x->active = x->heard = true;
			x->rssi_dbm = v->rssi_dbm;
			x->dmr = v->ch.mode == RADIO_MODE_DMR;
#if CONFIG_DMR
			x->src = v->dmr.call.src;
			x->dst = v->dmr.call.dst;
			x->group = v->dmr.call.group;
			strncpy(x->src_name, v->dmr.call.src_name, sizeof x->src_name - 1);
#endif
		}
	}
	uint8_t sel0 = d->sel, back0 = d->back_to;
	uint8_t heard = UI_DUAL_NONE;
	for (uint8_t i = 0; i < 2; i++) {
		ui_dual_pane_t *p = &d->pane[i];
		ui_dual_rx_t *x = &r[i];
		if (p->rx && !x->active) {
			call_ended(d, i, now);
			if (d->talk_back && i != d->sel && v->state != RS_TX)
				talk_back(d, i, now);
		}
		if (x->active) {
			caller_text(caller_buf, sizeof caller_buf, x->dmr, x->src, x->src_name, x->dst, x->group);
			if (strcmp(caller_buf, p->caller) != 0) {
				strcpy(p->caller, caller_buf);
				dirty = true;
			}
			if (x->heard || heard == UI_DUAL_NONE)
				heard = i;
		}
		bool busy = !own && !x->active && (v->dual_busy & (1u << i)) != 0;
		if (p->rx != x->active || p->heard != x->heard || p->rssi_dbm != x->rssi_dbm || p->busy != busy)
			dirty = true;
		p->busy = busy;
		p->rx = x->active;
		p->heard = x->heard;
		p->rssi_dbm = x->rssi_dbm;
	}
	if (heard != d->rx) {
		d->rx = heard;
		if (heard != UI_DUAL_NONE)
			strcpy(d->caller, d->pane[heard].caller);
		dirty = true;
	}
	talk_back_poll(d, v, now);
	return dirty || d->sel != sel0 || d->back_to != back0;
}

// ======================= Dual watch actions (# on Home while it runs) =======================

enum { DA_SELECT, DA_CHANGE_A, DA_CHANGE_B, DA_CALLER, DA_STOP, DA_COUNT };

static uint16_t actions_count(void) { return DA_COUNT; }

// The last caller and how long ago: "Joe Smith TG 91 10s ago", "Joe Smith 10s ago", then the caller alone
static void caller_age(sbuf_t *b, const char *caller, uint32_t ms)
{
	char alt[48];
	bool more = true;
	sb_str(b, TEXT_ALT_OPEN);
	for (int level = 0; more; level++) {
		gfx_text_alt(caller, level, alt, sizeof alt, &more);
		sb_str(b, alt);
		sb_chr(b, ' ');
		sb_age_long(b, ms);
		sb_str(b, TEXT_ALT_NEXT);
		if (!more)
			sb_str(b, alt);
	}
	sb_str(b, TEXT_ALT_CLOSE);
}

static void pane_value(sbuf_t *b, uint8_t i)
{
	const ui_dual_pane_t *p = &dual()->pane[i];
	if (p->vfo) {
		sb_str(b, "VFO ");
		sb_freq(b, p->rx_hz);
	} else {
		sb_str(b, TEXT_ALT_OPEN);
		sb_u32(b, p->number, 1);
		sb_chr(b, ' ');
		sb_str(b, p->name);
		sb_str(b, TEXT_ALT_NEXT);
		sb_str(b, p->name);
		sb_str(b, TEXT_ALT_CLOSE);
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
		if (!d->caller[0])
			it->flags |= UI_ITEM_DIM;
		else if (d->rx != UI_DUAL_NONE)
			sb_str(&vb, d->caller);		// on the air now
		else
			caller_age(&vb, d->caller, ui_st.now_ms - d->last_ms);
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
	bool have = dual()->on || (pane == 1 && b_last());
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
		ref_here(ui_view(), &other);
	bool on_a = ref_same(&r, &other);
	if (on_a) {
		sb_str(&b, pane ? "A" : "B");
		it->flags |= UI_ITEM_DIM;
		return;
	}
	// "146.940 FM", else "146.940": the channel's name gives way last
	sb_str(&b, TEXT_ALT_OPEN);
	sb_freq(&b, ch.rx_hz);
	sb_str(&b, ch.mode == RADIO_MODE_DMR ? " DMR" : " FM");
	sb_str(&b, TEXT_ALT_NEXT);
	sb_freq(&b, ch.rx_hz);
	sb_str(&b, TEXT_ALT_CLOSE);
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
		if (ui_svc_vfo_get((uint8_t)vfo_for((uint8_t)ui_frame()->arg).pos, &ch)) {
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
		ui_dual_ref_t r = vfo_for(pane);
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
