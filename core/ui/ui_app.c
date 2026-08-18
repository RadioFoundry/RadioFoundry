// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The UI's behavior (docs/ux/information-architecture.md, docs/spec/ui.md): the navigation stack, the keys with
// one meaning everywhere (P3), Home, quick settings, banners and interruptions, key lock, Recent. No RTOS: ui.c
// runs it in the UI task and the host simulator runs it on a PC.
//
// Global keys, before any screen sees them:
// - long BACK: Home, from anywhere (P4). Long QUICK: lock / unlock the keys. Long CONTEXT: Recent.
// - PTT and MONITOR go to the radio core from the input layer; the UI only keeps the focused row's talk target
//   current (ui_svc_focus_target) so a PTT press on a focused talkgroup or contact talks to it once.
// - TALK_TARGET: the talk target picker. EMERGENCY (held): start / cancel emergency.
// - CONTEXT while a banner is up: open what the banner is about.
// Boards without a QUICK key get long SELECT = QUICK on Home; without CONTEXT, the Hub starts with "Last caller"
// (information-architecture.md 7.5).
#include <string.h>
#include "board.h"
#include "ui_app.h"
#include "ui_nav.h"
#include "ui_svc.h"
#include "fmt.h"
#include "input/input.h"
#include "radio/tones.h"
#include "config.h"
#if CONFIG_HOTSPOT
#include "hostlink/hostlink.h"
#include "hotspot/hotspot.h"
#endif

#define UI_STACK_MAX		6
#define TOAST_MS		1800
#define ENTRY_TIMEOUT_MS	10000
#define BANNER_MS		8000
#define REFRESH_MS		1000
#define REFRESH_FAST_MS		250
#define UI_EVENTS		4
#define UI_FILTER_MAX		128
#define UI_FILTER_KEYS		10

// Critical section around the event queue (ui.c: the RTOS's; the simulator: nothing)
void ui_crit_enter(void);
void ui_crit_exit(void);

ui_state_t ui_st;
ui_target_t ui_target;
static const radio_view_t *view;
static radio_view_t view_none;		// until the first tick
static ui_frame_t stack[UI_STACK_MAX];
static uint8_t sp;			// stack[sp] is on screen; stack[0] is Home
static uint32_t toast_until, entry_last, last_refresh, now;
static bool dirty = true;
static uint32_t draws;
static bool has_quick_key, has_context_key;
static char home_hint[32];
static bool hs_diag;	// hotspot mode: the diagnostics page instead of the Hotspot screen
static uint32_t emerg_id;
static char emerg_name[CODEPLUG_NAME_LEN + 1];

// --- small helpers ---

const radio_view_t *ui_view(void) { return view; }
ui_frame_t *ui_frame(void) { return &stack[sp]; }
uint8_t ui_depth(void) { return sp; }
void ui_dirty(void) { dirty = true; }
static void update_focus_target(void);
void ui_focus_changed(void) { update_focus_target(); }

void ui_toast(const char *text)
{
	ui_st.toast = text;
	toast_until = now + TOAST_MS;
	dirty = true;
}

bool ui_has_action(uint8_t action)
{
	for (int i = 0; i < board.keymap_len; i++)
		if (board.keymap[i].short_action == action || board.keymap[i].long_action == action)
			return true;
	return false;
}

static void entry_cancel(void)
{
	ui_st.entry = false;
	ui_st.entry_len = 0;
	ui_st.entry_digits[0] = 0;
	dirty = true;
}

// The talk target the radio uses if PTT is pressed now with this screen's focus
static void update_focus_target(void)
{
	const ui_scr_def_t *d = ui_scr[stack[sp].scr];
	ui_target_t t;
	if (d->target != NULL && d->count != NULL && stack[sp].focus < d->count() && d->target(stack[sp].focus, &t))
		ui_svc_focus_target(t.id, !t.group);
	else
		ui_svc_focus_target(0, false);
}

void ui_push(uint8_t scr, uint16_t arg)
{
	if (sp + 1 >= UI_STACK_MAX)
		sp = UI_STACK_MAX - 2;	// never deeper than the stack: the oldest levels stay, the newest is replaced
	sp++;
	stack[sp].scr = scr;
	stack[sp].arg = arg;
	stack[sp].focus = 0;
	stack[sp].top = 0;
	ui_st.toast = NULL;
	if (ui_scr[scr]->flags & SF_FILTER) {
		ui_filter_reset();
	}
	if (ui_scr[scr]->enter != NULL)
		ui_scr[scr]->enter();	// may focus a row (the current zone in Channels)
	// Else the first row that can be focused
	const ui_scr_def_t *d = ui_scr[scr];
	if ((d->flags & SF_LIST) && d->count != NULL && d->item != NULL && stack[sp].focus == 0) {
		uint16_t n = d->count();
		while (stack[sp].focus < n) {
			ui_item_t it;
			it.flags = 0;
			it.label[0] = it.value[0] = 0;
			it.num = 0;
			d->item(stack[sp].focus, &it);
			if (!(it.flags & UI_ITEM_INFO))
				break;
			stack[sp].focus++;
		}
		if (stack[sp].focus >= n)
			stack[sp].focus = 0;
	}
	update_focus_target();
	dirty = true;
}

void ui_pop(void)
{
	if (sp > 0)
		sp--;
	if (ui_scr[stack[sp].scr]->flags & SF_FILTER)
		ui_filter_rebuild();
	ui_st.quick_edit = false;
	update_focus_target();
	dirty = true;
}

void ui_go_home(void)
{
	sp = 0;
	ui_st.quick_edit = false;
	entry_cancel();
	update_focus_target();
	dirty = true;
}

// --- filtered lists (SF_FILTER): the rows whose label matches the T9 keys typed ---

static uint16_t filter_idx[UI_FILTER_MAX];
static uint16_t filter_n;
static char filter_keys[UI_FILTER_KEYS + 1];

uint16_t ui_filter_count(void) { return filter_n; }
uint16_t ui_filter_raw(uint16_t i) { return i < filter_n ? filter_idx[i] : 0; }
const char *ui_filter_text(void) { return filter_keys; }

void ui_filter_rebuild(void)
{
	const ui_scr_def_t *d = ui_scr[stack[sp].scr];
	filter_n = 0;
	if (d->raw_count == NULL || d->raw_label == NULL)
		return;
	uint16_t n = d->raw_count();
	char label[40];
	for (uint16_t i = 0; i < n && filter_n < UI_FILTER_MAX; i++) {
		if (!d->raw_label(i, label, sizeof label))
			continue;
		if (filter_keys[0] == 0 || ui_t9_match(label, filter_keys))
			filter_idx[filter_n++] = i;
	}
}

void ui_filter_reset(void)
{
	filter_keys[0] = 0;
	ui_filter_rebuild();
}

static void filter_type(char key)
{
	size_t n = strlen(filter_keys);
	if (key == 0) {
		if (n > 0)
			filter_keys[n - 1] = 0;
	} else if (n < UI_FILTER_KEYS) {
		filter_keys[n] = key;
		filter_keys[n + 1] = 0;
	}
	ui_filter_rebuild();
	stack[sp].focus = 0;
	stack[sp].top = 0;
	update_focus_target();
	dirty = true;
}

// --- Recent: every call heard and made, newest first ---

static ui_recent_t recent[UI_RECENT_MAX];
static uint8_t recent_n;
static uint32_t recent_seen_ms;
static bool recent_tx;

uint8_t ui_recent_count(void) { return recent_n; }
const ui_recent_t *ui_recent_get(uint8_t i) { return i < recent_n ? &recent[i] : NULL; }

static void recent_add(const ui_recent_t *r)
{
	// The same station and destination again: move it to the top instead of repeating it
	uint8_t i;
	for (i = 0; i < recent_n; i++)
		if (recent[i].src == r->src && recent[i].dst == r->dst && recent[i].out == r->out)
			break;
	if (i == recent_n && recent_n < UI_RECENT_MAX)
		recent_n++;
	if (i >= recent_n)
		i = recent_n - 1;
	memmove(&recent[1], &recent[0], i * sizeof recent[0]);
	recent[0] = *r;
	dirty = true;
}

void ui_recent_target(const ui_recent_t *r, ui_target_t *t)
{
	memset(t, 0, sizeof *t);
	if (r->out) {
		t->id = r->dst;
		t->group = r->group;
	} else {
		t->id = r->src;
		t->group = false;
		t->heard_tg = r->group ? r->dst : 0;
	}
	strcpy(t->name, r->name);
	t->saved = t->name[0] != 0;
}

#if CONFIG_DMR
static void recent_update(const radio_view_t *v)
{
	const radio_dmr_view_t *d = &v->dmr;
	if (d->last_ms != 0 && d->last_ms != recent_seen_ms && d->last.src != 0 && d->last.src != d->own_id) {
		bool fresh = recent_n == 0 || recent[0].src != d->last.src || recent[0].dst != d->last.dst || recent[0].out;
		recent_seen_ms = d->last_ms;
		if (fresh) {
			ui_recent_t r;
			memset(&r, 0, sizeof r);
			r.src = d->last.src;
			r.dst = d->last.dst;
			r.group = d->last.group;
			r.to_us = d->last.to_us;
			r.ms = d->last_ms;
			strcpy(r.name, d->last.src_name);
			recent_add(&r);
		} else {
			recent[0].ms = d->last_ms;
		}
	}
	bool tx = v->state == RS_TX && d->active;
	if (tx && !recent_tx && d->target_id != 0) {
		ui_recent_t r;
		memset(&r, 0, sizeof r);
		r.src = d->own_id;
		r.dst = d->target_id;
		r.group = !d->target_private;
		r.out = true;
		r.ms = now;
		strcpy(r.name, d->target_name);
		recent_add(&r);
	}
	recent_tx = tx;
}
#endif

bool ui_contact_name(uint32_t id, bool group, char *out)
{
	out[0] = 0;
#if CONFIG_DMR
	contact_t c;
	for (uint16_t i = 1; i <= CODEPLUG_CONTACTS_MAX; i++) {
		if (!codeplug_contact(i, &c))
			continue;
		if (c.id == id && (c.type == CONTACT_GROUP) == group) {
			strcpy(out, c.name);
			return true;
		}
	}
#else
	(void)id;
	(void)group;
#endif
	return false;
}

void ui_sb_target(char *buf, size_t len, const ui_target_t *t)
{
	sbuf_t b;
	sb_init(&b, buf, len);
	if (t->id == 0xFFFFFFu) {
		sb_str(&b, "All call");
		return;
	}
	if (t->group) {
		sb_str(&b, "TG ");
		sb_u32(&b, t->id, 1);
		if (t->name[0]) {
			sb_chr(&b, ' ');
			sb_str(&b, t->name);
		}
	} else if (t->name[0]) {
		sb_str(&b, t->name);
	} else {
		sb_u32(&b, t->id, 1);
	}
}

void ui_sb_age(char *buf, size_t len, uint32_t ms)
{
	sbuf_t b;
	sb_init(&b, buf, len);
	sb_age(&b, ms);
}

// --- talking to a target ---

void ui_talk(const ui_target_t *t, bool permanent)
{
	static char msg[48];
	char name[32];
	ui_sb_target(name, sizeof name, t);
	if (permanent && !ui_svc_talk_to(t->id, !t->group, UI_TALK_PERMANENT)) {
		// No codeplug writer yet: use it for now and say so
		if (ui_svc_talk_to(t->id, !t->group, UI_TALK_TEMPORARY)) {
			ui_go_home();
			ui_toast("For now only: saving needs qdmr");
		} else {
			ui_toast("Can't change the talk target in this build");
		}
		return;
	}
	if (!permanent && !ui_svc_talk_to(t->id, !t->group, UI_TALK_TEMPORARY)) {
		ui_toast("Can't change the talk target in this build");
		return;
	}
	ui_go_home();
	sbuf_t b;
	sb_init(&b, msg, sizeof msg);
	if (permanent) {
		sb_str(&b, name);
		sb_str(&b, " is the default now");
	} else {
		sb_str(&b, "Talking to ");
		sb_str(&b, name);
	}
	ui_toast(msg);
}

void ui_open_actions(const ui_target_t *t)
{
	ui_target = *t;
	ui_push(SCR_ACTIONS, 0);
}

// The station Home's CONTEXT acts on: the current caller, else the last one heard
static bool caller_target(ui_target_t *t)
{
#if CONFIG_DMR
	const radio_dmr_view_t *d = &view->dmr;
	if (view->state == RS_RX && d->active && d->state != RDMR_IDLE && d->call.src != 0) {
		memset(t, 0, sizeof *t);
		t->id = d->call.src;
		t->heard_tg = d->call.group ? d->call.dst : 0;
		strcpy(t->name, d->call.src_name);
		t->saved = t->name[0] != 0;
		return true;
	}
#endif
	for (uint8_t i = 0; i < recent_n; i++) {
		if (!recent[i].out) {
			ui_recent_target(&recent[i], t);
			return true;
		}
	}
	return false;
}

static void open_caller_actions(void)
{
	ui_target_t t;
	if (caller_target(&t))
		ui_open_actions(&t);
	else
		ui_toast("No one heard yet: callers show up here");
}

// --- banners and events ---

static ui_event_t events[UI_EVENTS];
static volatile uint8_t ev_head, ev_tail;

bool ui_notify(const ui_event_t *ev)
{
	bool ok = false;
	ui_crit_enter();
	uint8_t next = (uint8_t)((ev_head + 1u) % UI_EVENTS);
	if (next != ev_tail) {
		events[ev_head] = *ev;
		ev_head = next;
		ok = true;
	}
	ui_crit_exit();
	return ok;
}

static void banner(uint8_t kind, bool warn, uint32_t id, const char *title, const char *text)
{
	ui_banner_t *b = &ui_st.banner;
	b->kind = kind;
	b->warn = warn;
	b->id = id;
	b->until_ms = now + BANNER_MS;
	strncpy(b->title, title, sizeof b->title - 1);
	b->title[sizeof b->title - 1] = 0;
	strncpy(b->text, text != NULL ? text : "", sizeof b->text - 1);
	b->text[sizeof b->text - 1] = 0;
	dirty = true;
}

static void station(sbuf_t *b, const ui_event_t *ev)
{
	if (ev->name[0])
		sb_str(b, ev->name);
	else
		sb_u32(b, ev->id, 1);
}

static void handle_event(const ui_event_t *ev)
{
	char title[UI_BANNER_TEXT];
	sbuf_t b;
	sb_init(&b, title, sizeof title);
	switch (ev->kind) {
	case UI_EV_MESSAGE:
		sb_str(&b, "Msg from ");
		station(&b, ev);
		banner(UI_BANNER_MESSAGE, false, ev->id, title, ev->text);
		ui_st.banner.msg = ev->index;
		break;
	case UI_EV_MESSAGE_SENT:
		if (ev->ok) {
			sb_str(&b, "Delivered to ");
			station(&b, ev);
			banner(UI_BANNER_RESULT, false, ev->id, title, NULL);
		} else {
			sb_str(&b, "No answer from ");
			station(&b, ev);
			banner(UI_BANNER_RESULT, true, ev->id, title, "Message kept: retry from Messages");
		}
		break;
	case UI_EV_CALL_ALERT:
		sb_str(&b, "Call alert from ");
		station(&b, ev);
		banner(UI_BANNER_CALL_ALERT, false, ev->id, title, "PTT replies");
		break;
	case UI_EV_PRIVATE_CALL:
		sb_str(&b, "Private call from ");
		station(&b, ev);
		banner(UI_BANNER_PRIVATE, false, ev->id, title, "PTT replies to them");
		break;
	case UI_EV_RADIO_CHECK:
		station(&b, ev);
		if (ev->ok) {
			char t[24];
			sbuf_t tb;
			sb_init(&tb, t, sizeof t);
			sb_str(&tb, "answered (");
			sb_u32(&tb, ev->ms / 1000u, 1);
			sb_chr(&tb, '.');
			sb_u32(&tb, (ev->ms % 1000u) / 100u, 1);
			sb_str(&tb, " s)");
			banner(UI_BANNER_RESULT, false, ev->id, title, t);
		} else {
			sb_str(&b, ": no answer");
			banner(UI_BANNER_RESULT, true, ev->id, title, "Off, out of range or elsewhere");
		}
		break;
	case UI_EV_EMERGENCY:
		emerg_id = ev->id;
		strcpy(emerg_name, ev->name);
		if (stack[sp].scr != SCR_EMERGENCY)
			ui_push(SCR_EMERGENCY, 0);
		break;
	case UI_EV_HOTSPOT_STARTED:
		banner(UI_BANNER_INFO, false, 0, "Hotspot started by host", "Back to stop");
		break;
	case UI_EV_SCAN_STOPPED:
		break;
	default:
		break;
	}
	dirty = true;
}

static void banner_open(void)
{
	ui_banner_t *b = &ui_st.banner;
	ui_target_t t;
	memset(&t, 0, sizeof t);
	t.id = b->id;
	t.saved = ui_contact_name(b->id, false, t.name);
	uint8_t kind = b->kind;
	uint16_t msg = b->msg;
	b->kind = UI_BANNER_NONE;
	if (kind == UI_BANNER_MESSAGE)
		ui_push(SCR_MESSAGE, msg);
	else
		ui_open_actions(&t);
}

// --- VFO frequency entry (digits on Home in the VFO; task-flows.md 12) ---

static void entry_value(const char *digits, int len, uint32_t *lo, uint32_t *hi)
{
	// Digits are positional from 100 MHz down to 10 Hz: the prefix spans [lo, hi]
	uint32_t v = 0, unit = 10;
	for (int i = 0; i < UI_ENTRY_MAX; i++)
		v = v * 10u + (uint32_t)(i < len ? digits[i] - '0' : 0);
	uint32_t span = unit;
	for (int i = len; i < UI_ENTRY_MAX; i++)
		span *= 10u;
	*lo = v * unit;
	*hi = *lo + span - unit;
}

static bool entry_possible(const char *digits, int len)
{
	uint32_t lo, hi;
	entry_value(digits, len, &lo, &hi);
	for (int i = 0; i < board.band_count; i++)
		if (lo <= board.bands[i].hi_hz && hi >= board.bands[i].lo_hz)
			return true;
	return false;
}

static void entry_digit(char d)
{
	static char hint[40];
	if (ui_st.entry_len >= UI_ENTRY_MAX) {
		ui_toast("That's all the digits: OK tunes");
		return;
	}
	ui_st.entry_digits[ui_st.entry_len] = d;
	if (!entry_possible(ui_st.entry_digits, ui_st.entry_len + 1)) {
		ui_st.entry_digits[ui_st.entry_len] = 0;
		char bands[32];
		ui_bands_text(bands, sizeof bands);
		strcpy(hint, "Bands ");
		strncat(hint, bands, sizeof hint - strlen(hint) - 1);
		ui_toast(hint);
		return;
	}
	ui_st.entry_len++;
	ui_st.entry_digits[ui_st.entry_len] = 0;
	ui_st.entry = true;
	dirty = true;
}

static void entry_commit(void)
{
	if (ui_st.entry_len < UI_ENTRY_MHZ) {
		ui_toast("Type at least the MHz digits");
		return;
	}
	uint32_t lo, hi;
	entry_value(ui_st.entry_digits, ui_st.entry_len, &lo, &hi);
	for (int i = 0; i < board.band_count; i++) {
		if (lo >= board.bands[i].lo_hz && lo <= board.bands[i].hi_hz) {
			radio_req_vfo_set(lo);
			entry_cancel();
			return;
		}
	}
	ui_toast("Not in a band: type more digits");
}

// --- input helpers ---

static int8_t nav_dir(const input_event_t *ev)
{
	switch (ev->action) {
	case ACT_NAV_UP: return 1;
	case ACT_NAV_DOWN: return -1;
	case ACT_ENC_STEP: return ev->value > 0 ? 1 : ev->value < 0 ? -1 : 0;
	default: return 0;
	}
}

static bool is_nav(const input_event_t *ev)
{
	return nav_dir(ev) != 0 && (ev->gesture == GESTURE_PRESS || ev->gesture == GESTURE_REPEAT ||
		ev->gesture == GESTURE_LONG || ev->action == ACT_ENC_STEP);
}

static int steps(const input_event_t *ev)
{
	if (ev->action != ACT_ENC_STEP)
		return 1;
	int n = ev->value < 0 ? -ev->value : ev->value;
	return n > 8 ? 8 : n;
}

static int digit_of(const input_event_t *ev)
{
	return ev->action >= ACT_DIGIT_0 && ev->action <= ACT_DIGIT_9 ? ev->action - ACT_DIGIT_0 : -1;
}

static void save_channel(void)
{
	if (!view->temporary && !ui_st.privacy) {
		ui_toast("Nothing to save");
	} else if (ui_svc_save_channel()) {
		ui_toast("Saved to channel");
	} else {
		ui_toast("Saving needs the codeplug writer: use qdmr");
	}
}

// --- Home (information-architecture.md 3) ---

static bool target_manual(void)
{
#if CONFIG_DMR
	return view->dmr.target_manual;
#else
	return false;
#endif
}

static void zone_step(int dir)
{
	static zone_t z;	// large (80 channels): not on the task's stack
	static char msg[CODEPLUG_NAME_LEN + 8];
	uint16_t n = codeplug_zone_count();
	if (n < 2) {
		ui_toast("Only one zone");
		return;
	}
	uint16_t next = (uint16_t)((view->zone_index + n + dir) % n);
	radio_req_zone(next);
	if (codeplug_zone(next, &z)) {
		strcpy(msg, "Zone: ");
		strncat(msg, z.name, CODEPLUG_NAME_LEN);
		ui_toast(msg);
	}
}

static void scan_toggle(void)
{
	bool on = !ui_st.scanning;
	if (!ui_svc_scan(on)) {
		ui_toast("Scan isn't in this build yet");
		return;
	}
	ui_st.scanning = on;
	ui_toast(on ? "Scanning this zone" : "Scan stopped");
}

bool ui_home_key(const input_event_t *ev)
{
	bool press = ev->gesture == GESTURE_PRESS;
	int d = digit_of(ev);
	if (is_nav(ev)) {
		if (ui_st.entry)
			entry_cancel();
		for (int i = steps(ev); i > 0; i--) {
			if (view->vfo)
				radio_req_vfo_step(nav_dir(ev));
			else
				radio_req_channel_step(nav_dir(ev));
		}
		return true;
	}
	if (d >= 0 && ev->gesture == GESTURE_LONG && !view->vfo) {
		static char msg[48];
		sbuf_t b;
		sb_init(&b, msg, sizeof msg);
		sb_str(&b, "Speed dial ");
		sb_u32(&b, (uint32_t)d, 1);
		sb_str(&b, " is empty: set it in qdmr");
		ui_toast(msg);
		return true;
	}
	if (d >= 0 && (press || ev->gesture == GESTURE_LONG)) {
		if (view->vfo) {
			entry_last = now;
			entry_digit((char)('0' + d));
		} else {
			char q[2] = { (char)('0' + d), 0 };
			ui_push(SCR_FIND, 0);
			ui_find_set(q);
		}
		return true;
	}
	if (ev->gesture == GESTURE_LONG) {
		if (ev->action == ACT_SELECT) {
			if (!has_quick_key)
				ui_push(SCR_QUICK, 0);	// few-key boards: long SELECT is QUICK
			else
				save_channel();
			return true;
		}
		return false;
	}
	if (!press)
		return false;
	switch (ev->action) {
	case ACT_SELECT:
		if (ui_st.entry)
			entry_commit();
		else
			ui_push(SCR_HUB, 0);
		return true;
	case ACT_BACK:
		// Clear temporary state, one thing per press, never anything harmful (P4)
		if (ui_st.entry) {
			entry_last = now;
			if (--ui_st.entry_len == 0)
				entry_cancel();
			else
				ui_st.entry_digits[ui_st.entry_len] = 0;
		} else if (ui_st.banner.kind != UI_BANNER_NONE) {
			ui_st.banner.kind = UI_BANNER_NONE;
		} else if (ui_st.scanning) {
			ui_svc_scan(false);
			ui_st.scanning = false;
			ui_toast("Scan stopped");
#if CONFIG_DMR
		} else if (view->ch.mode == RADIO_MODE_DMR && view->dmr.reply_active) {
			radio_req_reply_cancel();
			ui_toast("Reply ended");
		} else if (ui_st.hear_all) {
			ui_svc_hear_all(false);
			ui_st.hear_all = false;
			ui_toast("Hear all off");
#endif
		} else if (view->temporary || ui_st.privacy || (CONFIG_DMR && target_manual())) {
			radio_req_clear_temporary();
			ui_svc_talk_to(0, false, UI_TALK_DEFAULT);
			if (ui_st.privacy && ui_svc_privacy(false))
				ui_st.privacy = false;
			ui_toast("Changes undone");
		}
		dirty = true;
		return true;
	case ACT_QUICK:
		ui_push(SCR_QUICK, 0);
		return true;
	case ACT_CONTEXT:
		if (ui_st.scanning)
			ui_push(SCR_SCAN, 0);	// skip / stay / always skip (task-flows.md 11)
		else
			open_caller_actions();
		return true;
	case ACT_USER_1:
	case ACT_USER_4:
		zone_step(ev->action == ACT_USER_4 ? 1 : -1);	// "Zone picker" side keys
		return true;
	case ACT_USER_3:
		radio_req_power(1);	// top key: power cycle (information-architecture.md 7.1)
		return true;
	default:
		return false;
	}
}

void ui_home_draw(void)
{
	ui_draw_home(view, &ui_st);
}

// --- Quick settings (information-architecture.md 5) ---

void ui_quick_enter(void)
{
	ui_st.quick_focus = 0;
	ui_st.quick_edit = false;
	entry_cancel();
}

static void quick_change(uint8_t kind, int8_t dir)
{
	switch (kind) {
	case QT_POWER:
		radio_req_power(dir);
		break;
	case QT_SQUELCH:
		if ((dir > 0 && view->squelch < 9) || (dir < 0 && view->squelch > 0))
			radio_req_squelch(dir);
		break;
	case QT_TONE:
		ui_st.tone_edit = tones_step(&ui_st.tone_edit, dir);
		radio_req_tone(ui_st.tone_target, ui_st.tone_edit);
		break;
	case QT_CC: {
		int c = ui_st.cc_edit + dir;
		ui_st.cc_edit = (uint8_t)(c < 0 ? 15 : c > 15 ? 0 : c);
		break;
	}
	case QT_BRIGHTNESS: {
		int b = ui_st.brightness + dir * 10;
		ui_st.brightness = (uint8_t)(b < 10 ? 10 : b > 100 ? 100 : b);
		break;
	}
	default:
		break;
	}
	dirty = true;
}

static void quick_close_editor(void)
{
	uint8_t kind = ui_quick_kind(ui_st.quick_focus, view);
	if (kind == QT_CC && ui_st.quick_edit && !ui_svc_dmr_color_code(ui_st.cc_edit))
		ui_toast("Color code: not in this build yet");
	if (kind == QT_BRIGHTNESS)
		ui_svc_setting_set(UI_SET_BRIGHTNESS, ui_st.brightness);
	ui_st.quick_edit = false;
}

static void quick_unavailable(uint8_t kind)
{
	switch (kind) {
	case QT_TONE:
	case QT_BANDWIDTH:
		ui_toast(view->ch.mode == RADIO_MODE_DMR ? "FM only: this is a DMR channel" : "Not in this build");
		break;
	case QT_PRIVACY: ui_toast("Privacy isn't in this build yet"); break;
	case QT_SCAN: ui_toast("Scan isn't in this build yet"); break;
	case QT_MODE: ui_toast("Mode change isn't in this build yet"); break;
	case QT_STEP: ui_toast("Step: set it in qdmr for now"); break;
	default: ui_toast("Not in this build yet"); break;
	}
}

static void quick_activate(int tile)
{
	char label[12], value[12];
	ui_st.quick_focus = (uint8_t)tile;
	uint8_t kind = ui_quick_kind(tile, view);
	if (!ui_quick_tile(tile, view, &ui_st, label, sizeof label, value, sizeof value)) {
		quick_unavailable(kind);
		return;
	}
	switch (kind) {
	case QT_BANDWIDTH: {
		bool wide = view->ch.bw != RADIO_BW_25K;
		radio_req_bandwidth(wide ? RADIO_BW_25K : RADIO_BW_12K5);
		ui_toast(wide ? "Bandwidth 25 kHz" : "Bandwidth 12.5 kHz");
		break;
	}
	case QT_TONE: {
		bool same = view->ch.tx_tone.type == view->ch.rx_tone.type &&
			(view->ch.tx_tone.type == TONE_NONE || view->ch.tx_tone.value == view->ch.rx_tone.value);
		ui_st.tone_target = same ? RADIO_TONE_BOTH : RADIO_TONE_TX;
		ui_st.tone_edit = view->ch.tx_tone;
		ui_st.quick_edit = true;
		ui_toast("Up/Dn tone  # TX/RX");
		break;
	}
	case QT_MODE:
		if (ui_svc_mode(view->ch.mode == RADIO_MODE_DMR ? RADIO_MODE_FM : RADIO_MODE_DMR))
			ui_toast(view->ch.mode == RADIO_MODE_DMR ? "FM for now" : "DMR for now");
		else
			quick_unavailable(kind);
		break;
	case QT_MONITOR:
		if (!ui_svc_monitor(!view->monitor))
			quick_unavailable(kind);
		break;
	case QT_VOX:
		if (!ui_svc_vox(true))
			quick_unavailable(kind);
		break;
	case QT_SCAN:
		scan_toggle();
		break;
#if CONFIG_DMR
	case QT_TIMESLOT: {
		uint8_t ts = view->dmr.ts ? view->dmr.ts : view->ch.timeslot;
		if (ui_svc_dmr_timeslot(ts == 2 ? 1 : 2))
			ui_toast(ts == 2 ? "Timeslot 1 for now" : "Timeslot 2 for now");
		else
			quick_unavailable(kind);
		break;
	}
	case QT_CC:
		ui_st.cc_edit = view->dmr.active ? view->dmr.cc : view->ch.color_code;
		ui_st.quick_edit = true;
		break;
	case QT_HEAR_ALL:
		if (ui_svc_hear_all(!ui_st.hear_all)) {
			ui_st.hear_all = !ui_st.hear_all;
			ui_toast(ui_st.hear_all ? "Hear all: every talkgroup" : "Hear all off");
		} else {
			quick_unavailable(kind);
		}
		break;
	case QT_PRIVACY:
		if (ui_svc_privacy(!ui_st.privacy)) {
			ui_st.privacy = !ui_st.privacy;
			ui_toast(ui_st.privacy ? "Privacy on for now" : "Privacy off");
		} else {
			ui_toast("No key for this channel: add one in qdmr");
		}
		break;
	case QT_TALK_TO:
		ui_push(SCR_PICKER, 0);
		break;
#endif
	case QT_POWER: {
		// A cycle tile over three steps, low / the board's default / high, each press the next one up, wrapping
		// (information-architecture.md 5). Other levels come from the channel
		uint8_t hi = board.power_count ? (uint8_t)(board.power_count - 1u) : 0;
		uint8_t next = view->power < board.power_default ? board.power_default : view->power < hi ? hi : 0;
		for (int n = next - view->power; n != 0; n += n > 0 ? -1 : 1)
			radio_req_power(n > 0 ? 1 : -1);
		static char msg[16];
		strcpy(msg, "Power ");
		strncat(msg, ui_power_name(next), sizeof msg - 7);
		ui_toast(msg);
		break;
	}
	default:	// value tiles: squelch, brightness
		ui_st.quick_edit = true;
		break;
	}
	dirty = true;
}

bool ui_quick_key(const input_event_t *ev)
{
	bool press = ev->gesture == GESTURE_PRESS;
	int d = digit_of(ev);
	uint8_t kind = ui_quick_kind(ui_st.quick_focus, view);
	if (is_nav(ev)) {
		for (int i = steps(ev); i > 0; i--) {
			if (ui_st.quick_edit)
				quick_change(kind, nav_dir(ev));
			else
				ui_st.quick_focus = (uint8_t)((ui_st.quick_focus + UI_QUICK_TILES - nav_dir(ev)) % UI_QUICK_TILES);
		}
		dirty = true;
		return true;
	}
	if (d >= 0 && (press || ev->gesture == GESTURE_LONG)) {
		if (ui_st.quick_edit && kind == QT_SQUELCH) {
			for (int n = d - view->squelch; n != 0; n += n > 0 ? -1 : 1)
				radio_req_squelch(n > 0 ? 1 : -1);
			ui_st.quick_edit = false;
		} else if (ui_st.quick_edit && kind == QT_CC) {
			int c = ui_st.cc_edit * 10 + d;
			ui_st.cc_edit = (uint8_t)(ui_st.cc_edit < 2 && c <= 15 && ui_st.cc_edit != 0 ? c : d);
		} else if (ui_st.quick_edit && kind == QT_BRIGHTNESS) {
			ui_st.brightness = (uint8_t)(d == 0 ? 100 : d * 10);
			quick_close_editor();
		} else if (d == 0) {
			ui_st.quick_edit = false;
			ui_push(SCR_SETTINGS, 0);
		} else {
			ui_st.quick_edit = false;
			quick_activate(d - 1);
		}
		dirty = true;
		return true;
	}
	switch (ev->action) {
	case ACT_CONTEXT:
		if (press && ui_st.quick_edit && kind == QT_TONE) {
			// Tone (both) -> TX -> RX -> both; the editor starts from that direction's tone
			ui_st.tone_target = ui_st.tone_target == RADIO_TONE_BOTH ? RADIO_TONE_TX :
				ui_st.tone_target == RADIO_TONE_TX ? RADIO_TONE_RX : RADIO_TONE_BOTH;
			ui_st.tone_edit = ui_st.tone_target == RADIO_TONE_RX ? view->ch.rx_tone : view->ch.tx_tone;
			ui_toast(ui_st.tone_target == RADIO_TONE_BOTH ? "Tone for RX and TX" :
				ui_st.tone_target == RADIO_TONE_TX ? "Tone for TX only" : "Tone for RX only");
		} else if (press) {
			ui_toast("No actions here");
		}
		return true;
	case ACT_SELECT:
		if (ev->gesture == GESTURE_LONG) {
			if (ui_st.quick_edit)
				quick_close_editor();
			save_channel();
		} else if (press && ui_st.quick_edit) {
			quick_close_editor();
		} else if (press) {
			quick_activate(ui_st.quick_focus);
		}
		return true;
	case ACT_BACK:
		// BACK closes quick settings, an open editor included (its value is already in effect)
		if (press) {
			if (ui_st.quick_edit)
				quick_close_editor();
			ui_pop();
		}
		return true;
	case ACT_QUICK:
		if (press) {
			if (ui_st.quick_edit)
				quick_close_editor();
			else
				ui_pop();
		}
		return true;
	default:
		return false;
	}
}

void ui_quick_draw(void)
{
	ui_draw_quick(view, &ui_st);
}

// --- Diagnostics and emergency ---

void ui_diag_draw(void)
{
#if CONFIG_DIAG
	ui_draw_diag(view, input_raw_keys(), input_encoder_total(), ui_st.diag_first);
#endif
}

bool ui_diag_key(const input_event_t *ev)
{
#if CONFIG_DIAG
	if (ev->action == ACT_QUICK && ev->gesture == GESTURE_PRESS) {
		ui_st.diag_first = ui_diag_next_page(ui_st.diag_first);	// * pages through the board's lines
		return true;
	}
	if (ev->action == ACT_BACK || ev->action == ACT_SELECT)
		return false;
	if (ev->action >= ACT_DIGIT_0 && ev->action <= ACT_DIGIT_9 && !view->vfo)
		return false;	// no Find from here
	return ui_home_key(ev);	// channel, VFO digits and side keys still work underneath (bench checks)
#else
	(void)ev;
	return false;
#endif
}

void ui_emergency_draw(void)
{
	ui_draw_emergency(view, &ui_st, emerg_id, emerg_name);
}

bool ui_emergency_key(const input_event_t *ev)
{
	if (ev->gesture != GESTURE_PRESS)
		return false;
	if (ev->action == ACT_SELECT) {
		ui_pop();
		ui_toast("Emergency acknowledged");
		return true;
	}
	if (ev->action == ACT_CONTEXT) {
		ui_target_t t;
		memset(&t, 0, sizeof t);
		t.id = emerg_id;
		strcpy(t.name, emerg_name);
		ui_pop();
		ui_open_actions(&t);
		return true;
	}
	return false;
}

// --- hotspot mode (the host link drives the radio; a screen of its own replaces the others) ---
#if CONFIG_HOTSPOT
static hotspot_view_t hs_view;

static bool in_hotspot(void)
{
	return hostlink_mode() == HOSTLINK_HOTSPOT || view->hotspot;
}

static void hotspot_key(const input_event_t *ev)
{
	bool press = ev->gesture == GESTURE_PRESS;
	if ((ev->action == ACT_SELECT && press) || (ev->action == ACT_BACK && ev->gesture == GESTURE_LONG)) {
		hostlink_hotspot_exit();
		hs_diag = false;
		ui_toast("Leaving hotspot mode");
	} else if (ev->action == ACT_QUICK && press) {
		hs_diag = !hs_diag;
	} else if (press && !(hs_diag && ev->action == ACT_BACK)) {
		ui_toast("Hotspot: OK exits");
	}
}
#endif

// --- the generic list screen ---

static void list_item(const ui_scr_def_t *d, uint16_t i, ui_item_t *it)
{
	it->label[0] = it->value[0] = 0;
	it->num = 0;
	it->flags = 0;
	d->item(i, it);
}

static void list_move(const ui_scr_def_t *d, int8_t dir)
{
	ui_frame_t *f = &stack[sp];
	uint16_t n = d->count();
	if (n == 0)
		return;
	// Down the list = NAV_DOWN (dir -1); skip text rows, wrap around
	for (uint16_t tries = 0; tries < n; tries++) {
		f->focus = (uint16_t)(dir < 0 ? (f->focus + 1u) % n : (f->focus + n - 1u) % n);
		ui_item_t it;
		list_item(d, f->focus, &it);
		if (!(it.flags & UI_ITEM_INFO))
			break;
	}
	update_focus_target();
	dirty = true;
}

static bool list_key(const ui_scr_def_t *d, const input_event_t *ev)
{
	ui_frame_t *f = &stack[sp];
	bool press = ev->gesture == GESTURE_PRESS;
	int dg = digit_of(ev);
	uint16_t n = d->count != NULL ? d->count() : 0;
	if (f->focus >= n && n > 0)
		f->focus = (uint16_t)(n - 1);
	if (is_nav(ev)) {
		for (int i = steps(ev); i > 0; i--)
			list_move(d, nav_dir(ev));
		return true;
	}
	if (dg >= 0 && (press || ev->gesture == GESTURE_LONG)) {
		if (d->flags & SF_FILTER) {
			filter_type((char)('0' + dg));
			return true;
		}
		if (d->flags & SF_NUMBERED) {
			for (uint16_t i = 0; i < n; i++) {
				ui_item_t it;
				list_item(d, i, &it);
				if (it.num == '0' + dg) {
					f->focus = i;
					update_focus_target();
					if (it.flags & UI_ITEM_DIM)
						d->select(i, false);	// explains why it isn't available
					else
						d->select(i, false);
					return true;
				}
			}
		}
		ui_toast("No item with that number");
		return true;
	}
	switch (ev->action) {
	case ACT_SELECT:
		if (n == 0 || d->select == NULL) {
			if (press || ev->gesture == GESTURE_LONG)
				ui_toast(ev->gesture == GESTURE_LONG ? "Nothing to save here" : "Nothing to open");
			return true;
		}
		if (press || ev->gesture == GESTURE_LONG)
			d->select(f->focus, ev->gesture == GESTURE_LONG);
		return true;
	case ACT_CONTEXT:
		if (!press)
			return false;
		if (d->context != NULL && n > 0)
			d->context(f->focus);
		else
			ui_toast("No actions here");
		return true;
	case ACT_BACK:
		if (!press)
			return false;
		if ((d->flags & SF_FILTER) && filter_keys[0]) {
			filter_type(0);	// BACK deletes the last key typed first
			return true;
		}
		ui_pop();
		return true;
	default:
		return false;
	}
}

static const char *list_title_name(const ui_scr_def_t *d)
{
	return d->title != NULL ? d->title() : NULL;
}

static void list_draw(const ui_scr_def_t *d)
{
	ui_frame_t *f = &stack[sp];
	static char header[24];
	ui_list_t l;
	memset(&l, 0, sizeof l);
	l.prefix = d->prefix;
	l.name = list_title_name(d);
	l.count = d->count();
	l.focus = f->focus;
	l.top = &f->top;
	l.item = d->item;
	l.empty = d->empty;
	l.hint = d->hint;
	l.body = d->body != NULL ? d->body() : NULL;
	if ((d->flags & SF_FILTER) && filter_keys[0]) {
		sbuf_t b;
		sb_init(&b, header, sizeof header);
		sb_str(&b, "Find: ");
		sb_str(&b, filter_keys);
		sb_chr(&b, '_');
		l.header = header;
		if (l.count == 0)
			l.empty = "No match: Back deletes a key";
	}
	if (d->flags & SF_FIND) {
		sbuf_t b;
		sb_init(&b, header, sizeof header);
		sb_str(&b, "Find: ");
		sb_str(&b, ui_find_query());
		sb_chr(&b, '_');
		l.header = header;
	}
	ui_draw_list(view, &ui_st, &l);
}

// --- the app ---

void ui_app_init(void)
{
	view = &view_none;
	memset(&ui_st, 0, sizeof ui_st);
	recent_n = 0;
	recent_seen_ms = 0;
	recent_tx = false;
	ev_head = ev_tail = 0;
	sp = 0;
	stack[0].scr = SCR_HOME;
	ui_st.hints = ui_svc_setting_get(UI_SET_HINTS) != 0;
	ui_st.large_text = ui_svc_setting_get(UI_SET_LARGE_TEXT) != 0;
	int32_t br = ui_svc_setting_get(UI_SET_BRIGHTNESS);
	ui_st.brightness = (uint8_t)(br >= 10 && br <= 100 ? br : 100);
	has_quick_key = ui_has_action(ACT_QUICK);
	has_context_key = ui_has_action(ACT_CONTEXT);
	strcpy(home_hint, has_quick_key ? "OK Menu  * Quick" : "OK Menu  Hold OK Quick");
	ui_st.home_hint = home_hint;
	dirty = true;
}

uint8_t ui_app_brightness(void) { return ui_st.brightness; }

uint16_t ui_app_backlight_s(void)
{
	int32_t s = ui_svc_setting_get(UI_SET_BACKLIGHT_S);
	return (uint16_t)(s < 0 ? 0 : s);
}

static bool emergency_key(const input_event_t *ev)
{
	if (ev->action != ACT_EMERGENCY || ev->gesture != GESTURE_LONG)
		return false;
	static bool on;
	if (!ui_svc_emergency(!on)) {
		ui_toast("Emergency is not set up on this radio");
	} else {
		on = !on;
		ui_toast(on ? "EMERGENCY sent: hold again to cancel" : "Emergency cancelled");
	}
	return true;
}

void ui_app_key(const input_event_t *ev)
{
	dirty = true;
	if (ev->action == ACT_PTT || ev->action == ACT_MONITOR)
		return;		// the input layer already sent these to the radio
	if (emergency_key(ev))
		return;
	// Key lock: only long QUICK (and PTT, MONITOR, emergency) get through
	bool lock_key = has_quick_key ? (ev->action == ACT_QUICK && ev->gesture == GESTURE_LONG) :
		(ev->action == ACT_BACK && ev->gesture == GESTURE_LONG && ui_st.locked);
	if (ui_st.locked) {
		if (lock_key) {
			ui_st.locked = false;
			ui_toast("Keys unlocked");
		} else if (ev->gesture == GESTURE_PRESS || ev->gesture == GESTURE_LONG || ev->action == ACT_ENC_STEP) {
			ui_toast(has_quick_key ? "Keys locked: hold * to unlock" : "Keys locked: hold Back to unlock");
		}
		return;
	}
	if (lock_key && has_quick_key) {
		ui_st.locked = true;
		ui_st.quick_edit = false;
		entry_cancel();
		ui_toast("Keys locked: hold * to unlock");
		return;
	}
#if CONFIG_HOTSPOT
	if (in_hotspot()) {
		hotspot_key(ev);
		return;
	}
#endif
	// Long BACK = Home from anywhere; long CONTEXT = Recent (P3)
	if (ev->action == ACT_BACK && ev->gesture == GESTURE_LONG) {
		ui_go_home();
		return;
	}
	if (ev->action == ACT_CONTEXT && ev->gesture == GESTURE_LONG) {
		if (stack[sp].scr != SCR_RECENT)
			ui_push(SCR_RECENT, 0);
		return;
	}
	if (ev->action == ACT_TALK_TARGET && ev->gesture == GESTURE_PRESS) {
		if (CONFIG_DMR && view->ch.mode == RADIO_MODE_DMR)
			ui_push(SCR_PICKER, 0);
		else
			ui_toast("Talk target: DMR channels only");
		return;
	}
	if (ev->action == ACT_USER_2 && ev->gesture == GESTURE_LONG) {
		scan_toggle();	// long SK2 (DM-1701 key map): scan on / off
		return;
	}
	if (ev->action == ACT_USER_2 && ev->gesture == GESTURE_PRESS) {
		scan_toggle();
		return;
	}
	if (ev->action == ACT_USER_3 && ev->gesture == GESTURE_LONG) {
		ui_toast("Speak status: voice prompts aren't in this build yet");
		return;
	}
	// A banner up: CONTEXT opens what it is about
	if (ui_st.banner.kind != UI_BANNER_NONE && ev->action == ACT_CONTEXT && ev->gesture == GESTURE_PRESS &&
			ui_st.banner.kind != UI_BANNER_RESULT && ui_st.banner.kind != UI_BANNER_INFO) {
		banner_open();
		return;
	}
	// QUICK opens quick settings from any screen; BACK returns to where the user was
	const ui_scr_def_t *d = ui_scr[stack[sp].scr];
	if (ev->action == ACT_QUICK && ev->gesture == GESTURE_PRESS && stack[sp].scr != SCR_QUICK &&
			stack[sp].scr != SCR_DIAG && stack[sp].scr != SCR_HOME) {
		ui_push(SCR_QUICK, 0);
		return;
	}
	if (d->key != NULL && d->key(ev))
		return;
	if (d->flags & SF_LIST) {
		if (list_key(d, ev))
			return;
	} else if (ev->action == ACT_BACK && ev->gesture == GESTURE_PRESS && sp > 0) {
		ui_pop();
		return;
	}
	if (ev->gesture == GESTURE_PRESS && ev->action != ACT_ENC_STEP)
		ui_toast("Not used here");
}

bool ui_app_tick(const radio_view_t *v, uint32_t now_ms)
{
	static uint32_t serial_seen;
	static uint8_t battery_seen;
	view = v;
	now = now_ms;
	ui_st.now_ms = now_ms;

	while (ev_tail != ev_head) {
		ui_event_t ev = events[ev_tail];
		ev_tail = (uint8_t)((ev_tail + 1u) % UI_EVENTS);
		handle_event(&ev);
	}
#if CONFIG_DMR
	recent_update(v);
#endif
	if (ui_st.toast != NULL && (int32_t)(now - toast_until) >= 0) {
		ui_st.toast = NULL;
		dirty = true;
	}
	if (ui_st.banner.kind != UI_BANNER_NONE && (int32_t)(now - ui_st.banner.until_ms) >= 0) {
		ui_st.banner.kind = UI_BANNER_NONE;
		dirty = true;
	}
	if (ui_st.entry && (now - entry_last >= ENTRY_TIMEOUT_MS || !v->vfo))
		entry_cancel();
	uint16_t unread = ui_svc_msg_unread();
	if (unread != ui_st.unread) {
		ui_st.unread = unread;
		dirty = true;
	}
	bool scanning = ui_svc_scan_active();
	if (scanning != ui_st.scanning && (ui_svc_features() & UI_FEAT_SCAN)) {
		ui_st.scanning = scanning;
		dirty = true;
	}
	// The hint names the last caller: "OK Menu  * Quick  # Joe"
	ui_target_t t;
	char name[12];
	sbuf_t b;
	sb_init(&b, home_hint, sizeof home_hint);
	sb_str(&b, has_quick_key ? "OK Menu  * Quick" : "OK Menu  Hold OK Quick");
	if (has_context_key && recent_n && caller_target(&t)) {
		sb_str(&b, "  # ");
		if (t.name[0]) {
			memcpy(name, t.name, sizeof name - 1);
			name[sizeof name - 1] = 0;
			char *sp_ = strchr(name, ' ');
			if (sp_ != NULL)
				*sp_ = 0;	// first word of the name
			sb_str(&b, name);
		} else {
			sb_u32(&b, t.id, 1);
		}
	}
	uint32_t serial = radio_view_serial();
	if (serial != serial_seen) {
		serial_seen = serial;
		dirty = true;
	}
	uint8_t bat = board.power != NULL ? board.power->battery_pct() : 0;
	if (bat != battery_seen) {
		battery_seen = bat;
		dirty = true;
	}
	ui_st.battery_pct = bat;
	bool fast = v->state == RS_TX || stack[sp].scr == SCR_DIAG || ui_st.banner.kind != UI_BANNER_NONE;
#if CONFIG_HOTSPOT
	fast = fast || in_hotspot();
	if (!in_hotspot())
		hs_diag = false;
#endif
	if (now - last_refresh >= (fast ? REFRESH_FAST_MS : REFRESH_MS)) {
		last_refresh = now;
		dirty = true;
	}
	bool d = dirty;
	return d;
}

static bool strip_ok;

static void draw_screen(void);

void ui_app_draw(void)
{
	uint32_t before = ui_strip_draws;
	dirty = false;
	draws++;
	draw_screen();
	strip_ok = ui_strip_draws != before;
}

static void draw_screen(void)
{
#if CONFIG_HOTSPOT
	if (in_hotspot() && !(CONFIG_DIAG && hs_diag)) {
		hotspot_get_view(&hs_view);
		ui_draw_hotspot(view, &hs_view, &ui_st);
		return;
	}
	if (in_hotspot()) {
		ui_diag_draw();
		return;
	}
#endif
	const ui_scr_def_t *d = ui_scr[stack[sp].scr];
	if (d->draw != NULL)
		d->draw();
	else
		list_draw(d);
}

static const char *focus_label(void)
{
	static ui_item_t it;
	const ui_scr_def_t *d = ui_scr[stack[sp].scr];
	if (!(d->flags & SF_LIST) || d->count == NULL || stack[sp].focus >= d->count())
		return "";
	list_item(d, stack[sp].focus, &it);
	return it.label;
}

const char *ui_app_focus_label(void)
{
	return focus_label();
}

void ui_app_status(ui_app_status_t *s)
{
	const ui_scr_def_t *d = ui_scr[stack[sp].scr];
	s->screen = d->name;
#if CONFIG_HOTSPOT
	if (in_hotspot())
		s->screen = "hotspot";
#endif
	s->depth = sp;
	s->focus = stack[sp].scr == SCR_QUICK ? ui_st.quick_focus : stack[sp].focus;
	s->count = (d->flags & SF_LIST) && d->count != NULL ? d->count() : 0;
	s->locked = ui_st.locked;
	s->field_use = (d->flags & SF_FIELD) != 0;
	s->banner = ui_st.banner.kind != UI_BANNER_NONE ? ui_st.banner.title : "";
	s->toast = ui_st.toast != NULL ? ui_st.toast : "";
	s->query = stack[sp].scr == SCR_FIND ? ui_find_query() : (d->flags & SF_FILTER) ? filter_keys : "";
	s->draws = draws;
	s->strip_drawn = strip_ok;
}
