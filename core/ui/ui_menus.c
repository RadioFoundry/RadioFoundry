// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The list screens and the screen table (information-architecture.md 4, 6, 8): the Hub with its six destinations,
// actions on a station, Contacts, Messages, Recent, Channels, Tools, Settings grouped by intent, the talk target
// picker, information pages and the one confirmation (factory reset, P7).
#include <stddef.h>
#include <string.h>
#include "board.h"
#include "ui_nav.h"
#include "ui_svc.h"
#include "fmt.h"
#include "codeplug/codeplug.h"
#include "config.h"
#include "hal_time.h"
#include "radio/tones.h"
#include "radio/mic_gain.h"
#include "radio/ref_trim.h"
#include "radio/radio_am.h"
#include "input/keyassign.h"
#if CONFIG_HOTSPOT || CONFIG_USB_NETWORK
#include "hostlink/hostlink.h"
#endif
#include "rfoundry_about.h"
#if CONFIG_SFR
#include "dmr/sfr.h"
#endif

#ifndef RFOUNDRY_GIT
#define RFOUNDRY_GIT "host"
#endif
#ifndef RFOUNDRY_RELEASE
#define RFOUNDRY_RELEASE ""
#endif

extern const ui_scr_def_t ui_find_screen;
extern const ui_scr_def_t ui_aprs_screen, ui_aprs_heard_screen, ui_aprs_pos_screen;	// ui_aprs.c
#if CONFIG_DUAL_WATCH
extern const ui_scr_def_t ui_dual_screen, ui_dual_pick_screen, ui_dual_zone_screen;	// ui_dual.c
#endif

static void item_num(ui_item_t *it, uint16_t n)
{
	it->num = (char)('0' + n % 10u);
}

void item_num_set(ui_item_t *it, uint16_t n)
{
	item_num(it, n);
}

// ======================= Hub =======================

enum { HUB_LAST_CALLER, HUB_CONTACTS, HUB_MESSAGES, HUB_RECENT, HUB_ZONES, HUB_CHANNELS, HUB_TOOLS, HUB_SETTINGS };

static bool hub_has_last(void)
{
	return !ui_has_action(ACT_CONTEXT);	// few-key boards: "Last caller" stands in for CONTEXT (7.5)
}

static uint16_t hub_count(void) { return hub_has_last() ? 8 : 7; }

static uint8_t hub_entry(uint16_t i)
{
	return (uint8_t)(hub_has_last() ? i : i + 1u);
}

static void hub_item(uint16_t i, ui_item_t *it)
{
	static const char *const names[] = { "Last caller", "Contacts", "Messages", "Recent", "Zones", "Channels",
					     "Tools", "Settings" };
	uint8_t e = hub_entry(i);
	strcpy(it->label, names[e]);
	item_num(it, e);
	sbuf_t b;
	sb_init(&b, it->value, sizeof it->value);
	if (e == HUB_MESSAGES && ui_st.unread) {
		sb_u32(&b, ui_st.unread, 1);
		sb_str(&b, " new");
		it->flags |= UI_ITEM_WARN;
	} else if (e == HUB_ZONES) {
		// The zone (or VFO), left out where it doesn't fit beside the label (an empty last alternate)
		sb_str(&b, TEXT_ALT_OPEN);
		sb_str(&b, ui_view()->vfo ? "VFO" : ui_view()->zone_name);
		sb_str(&b, TEXT_ALT_NEXT TEXT_ALT_CLOSE);
	} else if (e == HUB_TOOLS && ui_st.scanning) {
		sb_str(&b, "Scanning");
	}
}

static void hub_select(uint16_t i, bool hold)
{
	static const uint8_t scr[] = { SCR_RECENT, SCR_CONTACTS, SCR_MESSAGES, SCR_RECENT, SCR_ZONES, SCR_CHANNELS,
				       SCR_TOOLS, SCR_SETTINGS };
	(void)hold;
	uint8_t e = hub_entry(i);
	if (e == HUB_LAST_CALLER) {
		if (ui_recent_count() == 0) {
			ui_toast("No calls heard yet");
			return;
		}
		ui_target_t t;
		ui_recent_target(ui_recent_get(0), &t);
		ui_open_actions(&t);
		return;
	}
	if (!CONFIG_CHANNEL_LIST && e == HUB_CHANNELS) {
		ui_toast("Channels: not in this build");	// the numbers stay the same in every build
		return;
	}
	ui_push(scr[e], 0);
}

static const ui_scr_def_t hub_screen = {
	.name = "hub", .flags = SF_LIST | SF_NUMBERED | SF_FIELD, .hint = "1-7 open  Back",
	.count = hub_count, .item = hub_item, .select = hub_select,
};

// ======================= Actions on a station or talkgroup =======================

enum { A_TALK, A_PRIVATE, A_MESSAGE, A_CHECK, A_ALERT, A_POSITION, A_SAVE_CONTACT, A_SPEAK, A_SAVE_CHANNEL,
       A_MAKE_DEFAULT };

static uint8_t act_list(uint8_t *out)
{
	uint8_t n = 0;
	if (ui_target.group) {
		out[n++] = A_TALK;
		out[n++] = A_MAKE_DEFAULT;
		out[n++] = A_MESSAGE;
	} else {
		static const uint8_t person[] = { A_TALK, A_PRIVATE, A_MESSAGE, A_CHECK, A_ALERT, A_POSITION,
						  A_SAVE_CONTACT, A_SPEAK };
		memcpy(out, person, sizeof person);
		n = sizeof person;
	}
	if (ui_view()->temporary)
		out[n++] = A_SAVE_CHANNEL;
	return n;
}

static uint16_t act_count(void)
{
	uint8_t l[12];
	return act_list(l);
}

static bool act_target(uint16_t i, ui_target_t *t)
{
	uint8_t l[12];
	uint8_t n = act_list(l);
	*t = ui_target;
	if (i < n && l[i] == A_PRIVATE)
		return true;	// a person, privately
	if (!t->group && t->heard_tg != 0) {
		// Talk to: the talkgroup they were heard on
		t->id = t->heard_tg;
		t->group = true;
		t->name[0] = 0;
	}
	return true;
}

static uint32_t feats(void) { return ui_svc_features(); }

static void act_item(uint16_t i, ui_item_t *it)
{
	uint8_t l[12];
	uint8_t n = act_list(l);
	if (i >= n)
		return;
	item_num(it, (uint16_t)(i + 1u));
	sbuf_t b;
	sb_init(&b, it->label, sizeof it->label);
	char name[32];
	ui_target_t t;
	switch (l[i]) {
	case A_TALK:
		act_target(i, &t);
		if (t.group)
			t.name[0] = 0;	// "Talk to TG 91": the name is in the title
		ui_sb_target(name, sizeof name, &t);
		sb_str(&b, "Talk to ");
		sb_str(&b, name);
		if (!(feats() & UI_FEAT_TALK_TARGET))
			it->flags |= UI_ITEM_DIM;
		break;
	case A_PRIVATE:
		sb_str(&b, "Private call");
		if (!(feats() & UI_FEAT_TALK_TARGET))
			it->flags |= UI_ITEM_DIM;
		break;
	case A_MESSAGE:
		sb_str(&b, "Message");
		if (!(feats() & UI_FEAT_SMS))
			it->flags |= UI_ITEM_DIM;
		break;
	case A_CHECK:
		sb_str(&b, "Radio check");
		if (!(feats() & UI_FEAT_SIGNALING))
			it->flags |= UI_ITEM_DIM;
		break;
	case A_ALERT:
		sb_str(&b, "Call alert");
		if (!(feats() & UI_FEAT_SIGNALING))
			it->flags |= UI_ITEM_DIM;
		break;
	case A_POSITION:
		sb_str(&b, "Position");
		it->flags |= UI_ITEM_DIM;
		break;
	case A_SAVE_CONTACT:
		sb_str(&b, ui_target.saved ? "In contacts" : "Save as contact");
		it->flags |= UI_ITEM_DIM;
		break;
	case A_SPEAK:
		sb_str(&b, "Speak status");
		it->flags |= UI_ITEM_DIM;
		break;
	case A_SAVE_CHANNEL:
		sb_str(&b, "Save to channel");
		if (!(feats() & UI_FEAT_SAVE_CHANNEL))
			it->flags |= UI_ITEM_DIM;
		break;
	case A_MAKE_DEFAULT:
		sb_str(&b, "Make channel default");
		if (!(feats() & UI_FEAT_TALK_TARGET))
			it->flags |= UI_ITEM_DIM;
		break;
	default:
		break;
	}
}

static void act_select(uint16_t i, bool hold)
{
	static char msg[40];
	uint8_t l[12];
	uint8_t n = act_list(l);
	if (i >= n)
		return;
	ui_target_t t;
	char name[24];
	ui_sb_target(name, sizeof name, &ui_target);
	sbuf_t b;
	sb_init(&b, msg, sizeof msg);
	switch (l[i]) {
	case A_TALK:
		act_target(i, &t);
		ui_talk(&t, hold);
		break;
	case A_PRIVATE:
		t = ui_target;
		t.group = false;
		ui_talk(&t, false);
		break;
	case A_MAKE_DEFAULT:
		ui_talk(&ui_target, true);
		break;
	case A_MESSAGE:
		if (feats() & UI_FEAT_SMS)
			ui_push(SCR_COMPOSE, 0);
		else
			ui_toast("Messages: not in this build");
		break;
	case A_CHECK:
		if (ui_svc_radio_check(ui_target.id)) {
			sb_str(&b, "Checking ");
			sb_str(&b, name);
			sb_str(&b, "...");
			ui_go_home();
			ui_toast(msg);
		} else {
			ui_toast("Radio check: not in this build");
		}
		break;
	case A_ALERT:
		if (ui_svc_call_alert(ui_target.id)) {
			sb_str(&b, "Call alert to ");
			sb_str(&b, name);
			ui_go_home();
			ui_toast(msg);
		} else {
			ui_toast("Call alert: not in this build");
		}
		break;
	case A_POSITION:
		ui_toast("Position requests: not in this build");
		break;
	case A_SAVE_CONTACT:
		ui_toast(ui_target.saved ? "Already in your contacts" : "Add contacts in qdmr");
		break;
	case A_SPEAK:
		ui_toast("Voice prompts: not in this build");
		break;
	case A_SAVE_CHANNEL:
		if (ui_svc_save_channel()) {
			ui_go_home();
			ui_toast("Saved to channel");
		} else {
			ui_toast("Can't save here: use qdmr");
		}
		break;
	default:
		break;
	}
}

// The station or talkgroup by name ("Joe Smith", "Main Ops"), else by number; the rows give the numbers
static const char *act_title(void)
{
	static char t[40];
	if (ui_target.group && ui_target.name[0] != '\0') {
		strcpy(t, ui_target.name);
		return t;
	}
	ui_sb_target(t, sizeof t, &ui_target);
	return t;
}

static const ui_scr_def_t actions_screen = {
	.name = "actions", .flags = SF_LIST | SF_NUMBERED | SF_FIELD, .hint = "1-9 pick  PTT talk  Back",
	.count = act_count, .item = act_item, .select = act_select, .target = act_target, .title = act_title,
};

// ======================= Message composer (canned first) =======================

static const char *const canned[] = { "OK", "On my way", "Call me", "Can't talk now" };
#define CANNED_N	(sizeof canned / sizeof canned[0])

static uint16_t compose_count(void) { return CANNED_N + 1; }

static void compose_item(uint16_t i, ui_item_t *it)
{
	if (i < CANNED_N) {
		sbuf_t b;
		sb_init(&b, it->label, sizeof it->label);
		sb_chr(&b, '"');
		sb_str(&b, canned[i]);
		sb_chr(&b, '"');
	} else {
		strcpy(it->label, "Write...");
		it->flags |= UI_ITEM_DIM;
	}
}

static void send_text(const char *text)
{
	static char msg[40];
	char name[24];
	ui_sb_target(name, sizeof name, &ui_target);
	if (!ui_svc_msg_send(ui_target.id, ui_target.group, text)) {
		ui_toast("Couldn't send: try again");
		return;
	}
	sbuf_t b;
	sb_init(&b, msg, sizeof msg);
	sb_str(&b, "Sending to ");
	sb_str(&b, name);
	ui_go_home();
	ui_toast(msg);
}

static void compose_select(uint16_t i, bool hold)
{
	(void)hold;
	if (i < CANNED_N)
		send_text(canned[i]);
	else
		ui_toast("Typing: not in this build");
}

static const char *compose_title(void)
{
	static char t[32];
	ui_sb_target(t, sizeof t, &ui_target);
	return t;
}

static const ui_scr_def_t compose_screen = {
	.name = "compose", .flags = SF_LIST | SF_FIELD, .prefix = "To", .hint = "OK send  Back",
	.count = compose_count, .item = compose_item, .select = compose_select, .title = compose_title,
};

// ======================= Contacts (type to find) =======================

#if CONFIG_DMR
static uint16_t contacts_raw_count(void) { return CODEPLUG_CONTACTS_MAX; }

static bool contacts_raw_label(uint16_t raw, char *buf, uint8_t len)
{
	contact_t c;
	if (!codeplug_contact((uint16_t)(raw + 1u), &c))
		return false;
	sbuf_t b;
	sb_init(&b, buf, len);
	sb_str(&b, c.name);
	sb_chr(&b, ' ');
	sb_u32(&b, c.id, 1);	// digits find by ID too
	return true;
}

static bool contacts_target(uint16_t i, ui_target_t *t)
{
	contact_t c;
	if (i >= ui_filter_count() || !codeplug_contact((uint16_t)(ui_filter_raw(i) + 1u), &c))
		return false;
	memset(t, 0, sizeof *t);
	t->id = c.id;
	t->group = c.type != CONTACT_PRIVATE;
	t->saved = true;
	strcpy(t->name, c.name);
	return true;
}

static void contacts_item(uint16_t i, ui_item_t *it)
{
	ui_target_t t;
	if (!contacts_target(i, &t))
		return;
	sbuf_t b;
	sb_init(&b, it->label, sizeof it->label);
	sbuf_t vb;
	sb_init(&vb, it->value, sizeof it->value);
	sb_str(&b, t.name[0] ? t.name : "(no name)");
	if (t.id == 0xFFFFFFu) {
		sb_str(&vb, "All call");
	} else if (t.group) {
		// "TG 3100", or the number alone where the name needs the room (text alternates)
		sb_str(&vb, TEXT_ALT_OPEN "TG ");
		sb_u32(&vb, t.id, 1);
		sb_str(&vb, TEXT_ALT_NEXT);
		sb_u32(&vb, t.id, 1);
		sb_str(&vb, TEXT_ALT_CLOSE);
	} else {
		sb_u32(&vb, t.id, 1);
	}
}

static void contacts_select(uint16_t i, bool hold)
{
	ui_target_t t;
	if (contacts_target(i, &t))
		ui_talk(&t, hold);
}

static void contacts_context(uint16_t i)
{
	ui_target_t t;
	if (contacts_target(i, &t))
		ui_open_actions(&t);
}

static const ui_scr_def_t contacts_screen = {
	.name = "contacts", .flags = SF_LIST | SF_FILTER | SF_FIELD, .prefix = "Contacts",
	.hint = "0-9 find  OK talk  # more", .empty = "No contacts: add them in qdmr",
	.count = ui_filter_count, .item = contacts_item, .select = contacts_select, .context = contacts_context,
	.target = contacts_target, .raw_count = contacts_raw_count, .raw_label = contacts_raw_label,
};
#else
static uint16_t none_count(void) { return 0; }
static void none_item(uint16_t i, ui_item_t *it) { (void)i; (void)it; }
static const ui_scr_def_t contacts_screen = {
	.name = "contacts", .flags = SF_LIST | SF_FIELD, .prefix = "Contacts", .hint = "Back",
	.empty = "Contacts: not in this build", .count = none_count, .item = none_item,
};
#endif

// ======================= Messages =======================

static uint16_t msgs_count(void) { return ui_svc_msg_count(); }

static void msgs_item(uint16_t i, ui_item_t *it)
{
	ui_msg_t m;
	if (!ui_svc_msg_get(i, &m))
		return;
	sbuf_t b;
	sb_init(&b, it->label, sizeof it->label);
	if (m.sent)
		sb_str(&b, GLYPH_ARROW);
	if (m.name[0])
		sb_str(&b, m.name);
	else
		sb_u32(&b, m.src, 1);
	sb_str(&b, ": ");
	sb_str(&b, m.text);
	ui_sb_age(it->value, sizeof it->value, m.age_ms);
	it->flags |= UI_ITEM_PREVIEW;	// the start of the message: cut at its end
	if (m.unread)
		it->flags |= UI_ITEM_WARN;
}

static void msgs_select(uint16_t i, bool hold)
{
	(void)hold;
	ui_push(SCR_MESSAGE, i);
}

static void msg_target_of(uint16_t i, ui_target_t *t)
{
	ui_msg_t m;
	memset(t, 0, sizeof *t);
	if (ui_svc_msg_get(i, &m)) {
		t->id = m.src;
		strcpy(t->name, m.name);
		t->saved = m.name[0] != 0;
	}
}

static void msgs_context(uint16_t i)
{
	ui_target_t t;
	msg_target_of(i, &t);
	ui_open_actions(&t);
}

static const ui_scr_def_t messages_screen = {
	.name = "messages", .flags = SF_LIST | SF_FIELD, .prefix = "Messages", .hint = "OK open  # reply  Back",
	.empty = "No messages yet",
	.count = msgs_count, .item = msgs_item, .select = msgs_select, .context = msgs_context,
};

// One message: the text, then canned replies first, call, delete

enum { MV_REPLY_OK, MV_REPLY_WAY, MV_CALL, MV_DELETE, MV_COUNT };

static void msg_enter(void)
{
	ui_svc_msg_read(ui_frame()->arg);
}

static uint16_t msg_count(void) { return MV_COUNT; }

static void msg_item(uint16_t i, ui_item_t *it)
{
	static const char *const names[] = { "Reply \"OK\"", "Reply \"On my way\"", "Call", "Delete" };
	strcpy(it->label, names[i]);
	if (i <= MV_REPLY_WAY && !(feats() & UI_FEAT_SMS))
		it->flags |= UI_ITEM_DIM;
}

static void msg_select(uint16_t i, bool hold)
{
	(void)hold;
	msg_target_of(ui_frame()->arg, &ui_target);
	switch (i) {
	case MV_REPLY_OK:
	case MV_REPLY_WAY:
		send_text(i == MV_REPLY_OK ? canned[0] : canned[1]);
		break;
	case MV_CALL:
		ui_talk(&ui_target, false);
		break;
	case MV_DELETE:
		if (ui_svc_msg_delete(ui_frame()->arg)) {
			ui_pop();
			ui_toast("Message deleted");
		} else {
			ui_toast("Couldn't delete: try again");
		}
		break;
	default:
		break;
	}
}

static bool msg_target(uint16_t i, ui_target_t *t)
{
	(void)i;
	msg_target_of(ui_frame()->arg, t);
	return t->id != 0;
}

static const char *msg_title(void)
{
	static char t[24];
	sbuf_t b;
	sb_init(&b, t, sizeof t);
	sb_u32(&b, ui_frame()->arg + 1u, 1);
	sb_str(&b, " of ");
	sb_u32(&b, ui_svc_msg_count(), 1);
	return t;
}

static const char *msg_body(void)
{
	static char t[UI_MSG_TEXT_MAX + 48];
	ui_msg_t m;
	if (!ui_svc_msg_get(ui_frame()->arg, &m))
		return "Message deleted";
	// "From Joe Smith, 2m ago" (the ID where there is no name; # gives the station's actions), then the text
	sbuf_t b;
	sb_init(&b, t, sizeof t);
	sb_str(&b, m.sent ? "To " : "From ");
	if (m.name[0])
		sb_str(&b, m.name);
	else
		sb_u32(&b, m.src, 1);
	sb_str(&b, ", ");
	sb_age_long(&b, m.age_ms);
	sb_chr(&b, '\n');
	sb_str(&b, m.text);
	return t;
}

static const ui_scr_def_t message_screen = {
	.name = "message", .flags = SF_LIST | SF_FIELD, .prefix = "Message", .hint = "OK choose  # more  Back",
	.count = msg_count, .item = msg_item, .select = msg_select, .context = msgs_context, .target = msg_target,
	.title = msg_title, .body = msg_body, .enter = msg_enter,
};

// ======================= Recent =======================

static uint16_t recent_count(void) { return ui_recent_count(); }

static bool recent_target(uint16_t i, ui_target_t *t)
{
	const ui_recent_t *r = ui_recent_get((uint8_t)i);
	if (r == NULL)
		return false;
	ui_recent_target(r, t);
	if (!t->group && t->heard_tg) {
		// PTT on a station heard on a talkgroup: that talkgroup (the same as Talk to in its actions)
		t->id = t->heard_tg;
		t->group = true;
		t->name[0] = 0;
	}
	return true;
}

static void recent_item(uint16_t i, ui_item_t *it)
{
	const ui_recent_t *r = ui_recent_get((uint8_t)i);
	if (r == NULL)
		return;
	sbuf_t b;
	sb_init(&b, it->label, sizeof it->label);
	sbuf_t vb;
	sb_init(&vb, it->value, sizeof it->value);
	if (r->out) {
		sb_str(&b, GLYPH_ARROW);
		sb_str(&b, r->group ? "TG " : "");
		sb_u32(&b, r->dst, 1);
		if (r->name[0]) {
			sb_chr(&b, ' ');
			sb_str(&b, r->name);
		}
	} else {
		if (r->name[0])
			sb_str(&b, r->name);
		else
			sb_u32(&b, r->src, 1);
		if (r->group) {
			sb_str(&vb, "TG ");
			sb_u32(&vb, r->dst, 1);
		} else {
			sb_str(&vb, r->to_us ? "To you" : "Private");
		}
		sb_chr(&vb, ' ');
		if (r->gps_range) {	// the caller's position: how far
			sb_u32(&vb, (r->gps_dist_m + 500u) / 1000u, 1);
			sb_str(&vb, "km ");
		}
	}
	sb_age(&vb, ui_st.now_ms - r->ms);
	if (r->to_us)
		it->flags |= UI_ITEM_WARN;
}

static void recent_select(uint16_t i, bool hold)
{
	ui_target_t t;
	if (recent_target(i, &t))
		ui_talk(&t, hold);
}

static void recent_context(uint16_t i)
{
	const ui_recent_t *r = ui_recent_get((uint8_t)i);
	ui_target_t t;
	if (r != NULL) {
		ui_recent_target(r, &t);
		ui_open_actions(&t);
	}
}

static const ui_scr_def_t recent_screen = {
	.name = "recent", .flags = SF_LIST | SF_FIELD, .prefix = "Recent", .hint = "PTT talk  # more  Back",
	.empty = "No calls heard yet",
	.count = recent_count, .item = recent_item, .select = recent_select, .context = recent_context,
	.target = recent_target,
};

// ======================= Channels: VFO, the zones; a zone's channels =======================

#define CH_VFO	((CONFIG_VFO_FM || CONFIG_VFO_DMR) ? 1u : 0u)	// raw row 0 is the VFO when it is built

static bool chans_is_vfo(uint16_t raw) { return (CONFIG_VFO_FM || CONFIG_VFO_DMR) && raw == 0; }
static uint16_t chans_raw_count(void) { return (uint16_t)(CH_VFO + codeplug_zone_count()); }

static bool chans_raw_label(uint16_t raw, char *buf, uint8_t len)
{
	static zone_t z;
	if (chans_is_vfo(raw)) {
		strncpy(buf, "VFO", len);
		return true;
	}
	if (!codeplug_zone((uint16_t)(raw - CH_VFO), &z))
		return false;
	strncpy(buf, z.name, len - 1);
	buf[len - 1] = 0;
	return true;
}

static void chans_enter(void)
{
	// Focus the current zone (task-flows.md 2)
	const radio_view_t *v = ui_view();
	uint16_t want = v->vfo ? 0 : (uint16_t)(CH_VFO + v->zone_index);
	for (uint16_t i = 0; i < ui_filter_count(); i++)
		if (ui_filter_raw(i) == want)
			ui_frame()->focus = i;
}

static void chans_item(uint16_t i, ui_item_t *it)
{
	static zone_t z;
	const radio_view_t *v = ui_view();
	uint16_t raw = ui_filter_raw(i);
	sbuf_t vb;
	sb_init(&vb, it->value, sizeof it->value);
	if (chans_is_vfo(raw)) {
		strcpy(it->label, "VFO");
		if (v->vfo)
			sb_str(&vb, "On");
		return;
	}
	uint16_t zi = (uint16_t)(raw - CH_VFO);
	if (!codeplug_zone(zi, &z))
		return;
	strcpy(it->label, z.name);
	// The zone's name whole; beside it "Here" for the zone in use and the channel count, as much as fits:
	// "Here, 16 channels", "Here, 16 ch", "Here" (or "16 channels", "16 ch", nothing)
	bool here = !v->vfo && zi == v->zone_index;
	sb_str(&vb, TEXT_ALT_OPEN);
	for (int form = 0; form < 3; form++) {
		if (form)
			sb_str(&vb, TEXT_ALT_NEXT);
		if (here)
			sb_str(&vb, form < 2 ? "Here, " : "Here");
		if (form < 2) {
			sb_u32(&vb, z.count, 1);
			sb_str(&vb, form == 0 ? (z.count == 1 ? " channel" : " channels") : " ch");
		}
	}
	sb_str(&vb, TEXT_ALT_CLOSE);
}

static void chans_context(uint16_t i);

static void chans_select(uint16_t i, bool hold)
{
	static char msg[CODEPLUG_NAME_LEN + 8];
	static zone_t z;
	if (hold && !ui_has_action(ACT_CONTEXT)) {
		chans_context(i);	// few-key boards: long OK opens what CONTEXT would (information-architecture 7.5)
		return;
	}
	const radio_view_t *v = ui_view();
	uint16_t raw = ui_filter_raw(i);
	if (chans_is_vfo(raw)) {
		bool was = v->vfo;	// the view may change at once (the host simulator)
		radio_req_vfo(!was);
		ui_go_home();
		ui_toast(was ? "Back to channels" : "VFO: type a frequency");
		return;
	}
	uint16_t zi = (uint16_t)(raw - CH_VFO);
	if (v->vfo)
		radio_req_vfo(false);
	radio_req_zone(zi);
	ui_go_home();
	if (codeplug_zone(zi, &z)) {
		strcpy(msg, "Zone: ");
		strncat(msg, z.name, CODEPLUG_NAME_LEN);
		ui_toast(msg);
	}
}

static void chans_context(uint16_t i)
{
	uint16_t raw = ui_filter_raw(i);
	if (chans_is_vfo(raw)) {
		ui_toast("The VFO has no channel list");
		return;
	}
	ui_push(SCR_ZONE, (uint16_t)(raw - CH_VFO));
}

static const ui_scr_def_t channels_screen = {
	.name = "zones", .flags = SF_LIST | SF_FILTER | SF_FIELD, .prefix = "Zones",
	.hint = TEXT_ALT_OPEN "OK go  # channels  0-9 find" TEXT_ALT_NEXT "OK go  # channels" TEXT_ALT_CLOSE,
	.empty = "No zones",
	.count = ui_filter_count, .item = chans_item, .select = chans_select, .context = chans_context,
	.enter = chans_enter, .raw_count = chans_raw_count, .raw_label = chans_raw_label,
};

// A channel row's value: its RX frequency and what it talks to, the talkgroup on DMR ("TG3113", "ID3101234" for a
// private contact) or "FM". The whole name comes first; where both don't fit beside it, the frequency and the bare
// number, then the talkgroup alone (text alternates)
static void sb_channel_row(sbuf_t *b, const channel_t *ch)
{
	char what[16];
	uint8_t prefix = 0;	// "TG" / "ID": left out in the middle form
	sbuf_t wb;
	sb_init(&wb, what, sizeof what);
#if CONFIG_DMR
	if (ch->mode == RADIO_MODE_DMR) {
		static contact_t c;
		uint16_t talk = codeplug_channel_talk_contact(ch);
		bool known = talk != 0 && codeplug_contact(talk, &c);
		sb_str(&wb, known && c.type == CONTACT_PRIVATE ? "ID" : "TG");
		prefix = 2;
		sb_u32(&wb, known ? c.id : 9u, 1);
	} else
#endif
	{
		sb_str(&wb, ch->mode == RADIO_MODE_DMR ? "DMR" : radio_am_channel(ch) ? "AM" : "FM");
	}
	sb_str(b, TEXT_ALT_OPEN);
	sb_freq(b, ch->rx_hz);
	sb_chr(b, ' ');
	sb_str(b, what);
	if (prefix) {
		sb_str(b, TEXT_ALT_NEXT);
		sb_freq(b, ch->rx_hz);
		sb_chr(b, ' ');
		sb_str(b, what + prefix);
	}
	sb_str(b, TEXT_ALT_NEXT);
	sb_str(b, what);
	sb_str(b, TEXT_ALT_CLOSE);
}

static zone_t zone_cache;
static uint16_t zone_cached = 0xFFFF;

// With the zone editor (ui.md 2.2) the list's raw rows are "Add channel", the channels, "Zone options"
#define ZE_ADD		(CONFIG_ZONE_EDIT ? 1u : 0u)
#if CONFIG_ZONE_EDIT
#if !CONFIG_CHANNEL_EDIT
#error "CONFIG_ZONE_EDIT needs CONFIG_CHANNEL_EDIT"
#endif
static uint16_t zone_focus_num;		// the channel the zone's list focuses when it shows again (ui_menus_resume)
static uint16_t ze_zone;		// the zone the editor's screens work on (the zone list's index)
static zone_op_t ze_op;			// the change handed to the storage job (ui_svc_zone_edit)
static char ze_msg[CODEPLUG_NAME_LEN + 24];	// the editor's toasts

static zone_op_t *ze_op_new(uint8_t kind, uint16_t pos)
{
	memset(&ze_op, 0, sizeof ze_op);
	ze_op.kind = kind;
	ze_op.zone = ze_zone;
	ze_op.pos = pos;
	return &ze_op;
}

static bool ze_post(void)
{
	if (ui_svc_zone_edit(&ze_op))
		return true;
	ui_toast("Couldn't save now: try again");
	return false;
}

// "<text><name>" in ze_msg, as a toast
static void ze_toast(const char *text, const char *name)
{
	sbuf_t b;
	sb_init(&b, ze_msg, sizeof ze_msg);
	sb_str(&b, text);
	sb_str(&b, name);
	ui_toast(ze_msg);
}
#endif

static const zone_t *zone_of_frame(void)
{
	uint16_t zi = ui_frame()->arg;
	if (zi != zone_cached) {
		if (!codeplug_zone(zi, &zone_cache))
			zone_cache.count = 0;
		zone_cached = zi;
	}
	return &zone_cache;
}

static uint16_t zone_raw_count(void)
{
	zone_cached = 0xFFFF;	// a fresh read when the list opens or the filter changes
	return (uint16_t)(zone_of_frame()->count + 2u * ZE_ADD);
}

#if CONFIG_ZONE_EDIT
// The editor's rows: their label, NULL for a channel's row
static const char *zone_edit_row(uint16_t raw)
{
	if (raw == 0)
		return "Add channel";
	return raw > zone_of_frame()->count ? "Zone options" : NULL;
}
#endif

static bool zone_raw_label(uint16_t raw, char *buf, uint8_t len)
{
	static channel_t ch;
	const zone_t *z = zone_of_frame();
	sbuf_t b;
	sb_init(&b, buf, len);
#if CONFIG_ZONE_EDIT
	if (zone_edit_row(raw) != NULL) {
		sb_str(&b, zone_edit_row(raw));
		return true;
	}
#endif
	uint16_t pos = (uint16_t)(raw - ZE_ADD);
	if (pos >= z->count || !codeplug_channel(z->channels[pos], &ch))
		return false;
	sb_u32(&b, pos + 1u, 1);
	sb_chr(&b, ' ');
	sb_str(&b, ch.name);
	return true;
}

// Focus the row of channel number num (0: the channel in use, when this is the zone in use)
static void zone_focus(uint16_t num)
{
	const radio_view_t *v = ui_view();
	const zone_t *z = zone_of_frame();
	for (uint16_t i = 0; i < ui_filter_count(); i++) {
		uint16_t pos = (uint16_t)(ui_filter_raw(i) - ZE_ADD);
		if (pos >= z->count)
			continue;
		if (num != 0 ? z->channels[pos] == num : !v->vfo && v->zone_index == ui_frame()->arg && pos == v->zone_pos)
			ui_frame()->focus = i;
	}
}

static void zone_enter(void)
{
#if CONFIG_ZONE_EDIT
	zone_focus_num = 0;
#endif
	zone_focus(0);
}

#if CONFIG_ZONE_EDIT
// The list shows again (a menu above it closed) or the codeplug changed under it: the rows are read again
// (ui_filter_rebuild), and the focus follows the channel just moved or added
void ui_menus_resume(void)
{
	if (ui_frame()->scr == SCR_ZONE && zone_focus_num != 0)
		zone_focus(zone_focus_num);
}
#endif

static void zone_item(uint16_t i, ui_item_t *it)
{
	static channel_t ch;
	const zone_t *z = zone_of_frame();
	uint16_t raw = ui_filter_raw(i);
#if CONFIG_ZONE_EDIT
	if (zone_edit_row(raw) != NULL) {
		strcpy(it->label, zone_edit_row(raw));
		return;
	}
#endif
	raw = (uint16_t)(raw - ZE_ADD);		// the position in the zone
	if (raw >= z->count || !codeplug_channel(z->channels[raw], &ch))
		return;
	sbuf_t b;
	sb_init(&b, it->label, sizeof it->label);
	sb_u32(&b, raw + 1u, 2);
	sb_chr(&b, ' ');
	sb_str(&b, ch.name);
	sb_init(&b, it->value, sizeof it->value);
	sb_channel_row(&b, &ch);
	const radio_view_t *v = ui_view();
	if (!v->vfo && v->zone_index == ui_frame()->arg && v->zone_pos == raw)
		it->flags |= UI_ITEM_TEMP;	// the channel we are on
}

#if CONFIG_ZONE_EDIT
static void zone_context(uint16_t i)
{
	uint16_t raw = ui_filter_raw(i);
	if (zone_edit_row(raw) != NULL)
		return;
	ze_zone = ui_frame()->arg;
	ui_push(SCR_ZCHANNEL, (uint16_t)(raw - ZE_ADD));
}
#endif

static void zone_select(uint16_t i, bool hold)
{
	const radio_view_t *v = ui_view();
	uint16_t zi = ui_frame()->arg;
	uint16_t raw = ui_filter_raw(i);
#if CONFIG_ZONE_EDIT
	if (zone_edit_row(raw) != NULL) {
		ze_zone = zi;
		ui_push(raw == 0 ? SCR_ZPICK : SCR_ZONEEDIT, zi);
		return;
	}
	if (hold && !ui_has_action(ACT_CONTEXT)) {
		zone_context(i);	// few-key boards: long OK opens what CONTEXT would (information-architecture 7.5)
		return;
	}
#endif
	(void)hold;
	if (v->vfo)
		radio_req_vfo(false);
	if (v->zone_index != zi)
		radio_req_zone(zi);
	radio_req_zone_pos((uint16_t)(raw - ZE_ADD));
	ui_go_home();
}

static const char *zone_title(void)
{
	return zone_of_frame()->name;
}

#if CONFIG_ZONE_SHARE
// ======================= Zone sharing (docs/spec/zone-share.md; docs/user/07-channels-zones-vfo.md) =======================
// CONTEXT in a zone: send the zone, or the channel focused, to a contact or talkgroup (with the zone editor: from
// its Channel actions and Zone options). An offer from another radio comes as a banner (CONTEXT opens it) and waits
// under Tools while it lasts: Import, Preview, Reject
// The rows that share, with the share icon in front
#define SHARE_ZONE	GLYPH_SHARE " Share zone to..."
#define SHARE_CHANNEL	GLYPH_SHARE " Share channel to..."

static uint16_t share_zone, share_pos;	// the zone and the channel position CONTEXT was pressed on
static bool share_one;			// send only that channel
static char share_name[CODEPLUG_NAME_LEN + 1];

// Share To for the zone, or (one) only its channel at pos; from a menu one level above the zone's list, which
// share_to_select leaves with it. Zone UI_ZONE_NONE: pos is a channel's number (the channel editor, which stays)
static void share_send_to(uint16_t zone, uint16_t pos, bool one, const char *name)
{
	if (!(feats() & UI_FEAT_ZONE_SHARE)) {
		ui_toast("Zone sharing: not in this build");
		return;
	}
#if CONFIG_ZONE_EDIT
	if (ui_svc_zone_pending()) {
		ui_toast("Saving the zone: try again");	// the send reads the stored zone, its positions as saved
		return;
	}
#endif
	share_zone = zone;
	share_pos = pos;
	share_one = one;
	if (name != share_name)
		strcpy(share_name, name);	// the Share menu passes its own
	ui_push(SCR_SHARE_TO, 0);
}

#if !CONFIG_ZONE_EDIT
static void zone_context(uint16_t i)
{
	const zone_t *z = zone_of_frame();
	if (z->count == 0)
		return;
	share_zone = ui_frame()->arg;
	share_pos = ui_filter_raw(i);
	strcpy(share_name, z->name);
	ui_push(SCR_SHARE, 0);
}

enum { SH_ZONE, SH_CHANNEL, SH_COUNT };

static uint16_t share_count(void) { return SH_COUNT; }

static void share_item(uint16_t i, ui_item_t *it)
{
	strcpy(it->label, i == SH_ZONE ? SHARE_ZONE : SHARE_CHANNEL);
	item_num(it, (uint16_t)(i + 1u));
	if (!(feats() & UI_FEAT_ZONE_SHARE))
		it->flags |= UI_ITEM_DIM;
}

static void share_select(uint16_t i, bool hold)
{
	(void)hold;
	share_send_to(share_zone, share_pos, i == SH_CHANNEL, share_name);
}

static const char *share_title(void) { return share_name; }

static const ui_scr_def_t share_screen = {
	.name = "zone_share", .flags = SF_LIST | SF_NUMBERED, .prefix = NULL, .hint = "OK pick  Back",
	.count = share_count, .item = share_item, .select = share_select, .title = share_title,
};
#endif

// Share To: the codeplug's contacts, private or group (type to find, as Contacts)
static void share_to_select(uint16_t i, bool hold)
{
	(void)hold;
	ui_target_t t;
	uint8_t total = 0;
	if (!contacts_target(i, &t))
		return;
	uint8_t n = ui_svc_zone_send(share_zone, share_pos, share_one, t.id, t.group, &total);
	static char msg[48];	// the toast keeps the pointer
	sbuf_t b;
	sb_init(&b, msg, sizeof msg);
	if (n == 0) {
		ui_toast("Can't send now: try again");
		return;
	}
	if (!share_one && n < total) {
		// The record holds what fits (zone-share.md 4): say how much goes
		sb_str(&b, "Sending ");
		sb_u32(&b, n, 1);
		sb_str(&b, " of ");
		sb_u32(&b, total, 1);
		sb_str(&b, " channels");
	} else {
		sb_str(&b, share_one ? "Sending channel to " : "Sending zone to ");
		sb_str(&b, t.name[0] ? t.name : "this contact");
	}
	ui_pop();
	if (share_zone != UI_ZONE_NONE)
		ui_pop();	// and the menu it came from; the channel editor stays
	ui_toast(msg);
}

static const ui_scr_def_t share_to_screen = {
	.name = "zone_share_to", .flags = SF_LIST | SF_FILTER, .prefix = "Share To",
	.hint = "0-9 find  OK send  Back", .empty = "No contacts: add them in qdmr",
	.count = ui_filter_count, .item = contacts_item, .select = share_to_select,
	.raw_count = contacts_raw_count, .raw_label = contacts_raw_label,
};

// --- An offer from another radio ---
static ui_zone_offer_t offer;	// read on each pass: it may go (imported, rejected, expired)

static bool offer_get(void)
{
	if (!ui_svc_zone_offer(&offer)) {
		offer.serial = 0;
		return false;
	}
	return true;
}

enum { OF_IMPORT, OF_PREVIEW, OF_REJECT, OF_COUNT };

static uint16_t offer_count(void) { return offer_get() ? OF_COUNT : 0; }

static void offer_item(uint16_t i, ui_item_t *it)
{
	static const char *const names[] = { "Import", "Preview", "Reject" };
	strcpy(it->label, names[i]);
	item_num(it, (uint16_t)(i + 1u));
	if (i == OF_IMPORT && offer.in_band == 0)
		it->flags |= UI_ITEM_DIM;
}

static void offer_enter(void)
{
	if (offer_get())
		ui_svc_zone_offer_open(offer.serial);	// open: it doesn't expire while the user reads it
}

static void offer_import(uint16_t zone)
{
	if (!ui_svc_zone_offer_import(offer.serial, zone)) {
		ui_toast("Busy saving: try again");
		return;
	}
	ui_go_home();
	ui_toast("Importing...");
}

static void offer_select(uint16_t i, bool hold)
{
	(void)hold;
	if (!offer_get())
		return;
	switch (i) {
	case OF_IMPORT:
		if (offer.in_band == 0)
			ui_toast("No channel in this radio's bands");
		else if (offer.channel)
			ui_push(SCR_OFFER_ZONE, 0);	// which zone it joins
		else
			offer_import(0);
		break;
	case OF_PREVIEW:
		ui_push(SCR_OFFER_PREVIEW, 0);
		break;
	default:
		ui_svc_zone_offer_reject(offer.serial);
		ui_pop();
		ui_toast("Offer rejected");
		break;
	}
}

// "Zone Club, 24 channels, from Joe 3101234" and how many can't be received here
static const char *offer_body(void)
{
	static char body[96];
	sbuf_t b;
	sb_init(&b, body, sizeof body);
	if (!offer_get())
		return NULL;
	sb_str(&b, offer.channel ? "Channel " : "Zone ");
	sb_str(&b, offer.name);
	if (!offer.channel) {
		sb_str(&b, ", ");
		sb_u32(&b, offer.channels, 1);
		sb_str(&b, offer.channels == 1 ? " channel" : " channels");
	}
	sb_str(&b, ", from ");
	if (offer.from[0]) {
		sb_str(&b, offer.from);
		sb_chr(&b, ' ');
	}
	sb_u32(&b, offer.src, 1);
	if (offer.in_band < offer.channels) {
		// Left out by the import (the result banner says the rest come in receive only)
		sb_str(&b, ". ");
		sb_u32(&b, (uint32_t)(offer.channels - offer.in_band), 1);
		sb_str(&b, " not in band");
	}
	return body;
}

// The longest title that fits: a whole zone or one channel
static const char *offer_title(void)
{
	if (offer_get() && offer.channel)
		return TEXT_ALT_OPEN "Channel Share Received " GLYPH_DOWNLOAD TEXT_ALT_NEXT "Ch Share Received " GLYPH_DOWNLOAD
		       TEXT_ALT_NEXT "Ch Received " GLYPH_DOWNLOAD TEXT_ALT_NEXT "Ch Received" TEXT_ALT_CLOSE;
	return TEXT_ALT_OPEN "Zone Share Received " GLYPH_DOWNLOAD TEXT_ALT_NEXT "Zone Received " GLYPH_DOWNLOAD TEXT_ALT_NEXT
	       "Zone Received" TEXT_ALT_CLOSE;
}

static const ui_scr_def_t offer_screen = {
	.name = "zone_offer", .flags = SF_LIST | SF_NUMBERED, .hint = "OK pick  Back",
	.empty = "No share waiting",
	.count = offer_count, .item = offer_item, .select = offer_select, .body = offer_body, .enter = offer_enter,
	.title = offer_title,
};

static uint16_t preview_count(void) { return offer_get() ? offer.channels : 0; }

static void preview_item(uint16_t i, ui_item_t *it)
{
	ui_zone_offer_ch_t ch;
	if (!ui_svc_zone_offer_channel(offer.serial, i, &ch))
		return;
	sbuf_t b;
	sb_init(&b, it->label, sizeof it->label);
	sb_str(&b, ch.name[0] ? ch.name : "(no name)");
	sb_init(&b, it->value, sizeof it->value);
	if (!ch.in_band) {
		sb_str(&b, TEXT_ALT_OPEN "Not in band" TEXT_ALT_NEXT "Out" TEXT_ALT_CLOSE);
		it->flags |= UI_ITEM_DIM;
		return;
	}
	sb_str(&b, TEXT_ALT_OPEN);
	sb_freq(&b, ch.rx_hz);
	sb_str(&b, ch.dmr ? " DMR" : " FM");
	sb_str(&b, TEXT_ALT_NEXT);
	sb_freq(&b, ch.rx_hz);
	sb_str(&b, TEXT_ALT_CLOSE);
}

static void preview_select(uint16_t i, bool hold)
{
	(void)i;
	(void)hold;
	ui_pop();	// back to Import / Reject
}

static const char *preview_title(void) { return offer.name; }

static const ui_scr_def_t preview_screen = {
	.name = "zone_offer_preview", .flags = SF_LIST, .prefix = NULL, .hint = "OK or Back returns",
	.empty = "No zone offer waiting",
	.count = preview_count, .item = preview_item, .select = preview_select, .title = preview_title,
};

// A channel offer joins a zone the user picks
static uint16_t offer_zone_count(void) { return codeplug_zone_count(); }

static void offer_zone_item(uint16_t i, ui_item_t *it)
{
	static zone_t z;
	if (!codeplug_zone(i, &z))
		return;
	strcpy(it->label, z.name);
	sbuf_t b;
	sb_init(&b, it->value, sizeof it->value);
	sb_u32(&b, z.count, 1);
	sb_str(&b, TEXT_ALT_OPEN " channels" TEXT_ALT_NEXT " ch" TEXT_ALT_CLOSE);
	if (z.count >= CODEPLUG_ZONE_MAX)
		it->flags |= UI_ITEM_DIM;
}

static void offer_zone_select(uint16_t i, bool hold)
{
	(void)hold;
	if (offer_get())
		offer_import(i);
}

static const ui_scr_def_t offer_zone_screen = {
	.name = "zone_offer_zone", .flags = SF_LIST, .prefix = "Add to Zone", .hint = "OK add  Back",
	.empty = "No zones",
	.count = offer_zone_count, .item = offer_zone_item, .select = offer_zone_select,
};

// Tools: the offer waiting, while it lasts (the banner may have gone unseen)
static bool offer_waiting(void)
{
	ui_zone_offer_t o;
	return ui_svc_zone_offer(&o);
}
#endif

#if CONFIG_ZONE_EDIT
#define ZONE_HINT	TEXT_ALT_OPEN "OK go  # edit  0-9 find" TEXT_ALT_NEXT "OK go  # edit" TEXT_ALT_CLOSE
#define ZONE_CONTEXT	zone_context
#elif CONFIG_ZONE_SHARE
#define ZONE_HINT	TEXT_ALT_OPEN "OK go  # share  0-9 find" TEXT_ALT_NEXT "OK go  # share" TEXT_ALT_CLOSE
#define ZONE_CONTEXT	zone_context
#else
#define ZONE_HINT	"OK go  0-9 find  Back"
#define ZONE_CONTEXT	NULL
#endif

static const ui_scr_def_t zone_screen = {
	.name = "zone", .flags = SF_LIST | SF_FILTER | SF_FIELD, .prefix = NULL,
	.hint = ZONE_HINT, .empty = "Empty zone",
	.count = ui_filter_count, .item = zone_item, .select = zone_select, .context = ZONE_CONTEXT,
	.title = zone_title, .enter = zone_enter, .raw_count = zone_raw_count, .raw_label = zone_raw_label,
};

#if CONFIG_ZONE_WRITE
// Zone indexes moved (zone_edit.h): the stack's zone screens follow them. Zones keeps its focus on the same zone, a
// zone's list and the editor's screens on their zone; a list whose zone was deleted closes, with what is above it.
// Returns the new top of the stack
uint8_t ui_menus_zones_moved(ui_frame_t *stack, uint8_t sp, uint32_t from, uint32_t to)
{
	uint16_t out;
#if CONFIG_ZONE_EDIT
	bool ze_ok = zone_edit_map(from, to, ze_zone, &out) == ZONE_MAP_KEPT;
	if (ze_ok)
		ze_zone = out;
#endif
	for (uint8_t i = 1; i <= sp; i++) {
		ui_frame_t *f = &stack[i];
		if (f->scr == SCR_ZONES && !chans_is_vfo(f->focus)) {
			if (zone_edit_map(from, to, (uint16_t)(f->focus - CH_VFO), &out) != ZONE_MAP_LOST)
				f->focus = (uint16_t)(out + CH_VFO);	// a deleted one: the next zone, clamped by the list
		} else if (f->scr == SCR_ZONE) {
			if (zone_edit_map(from, to, f->arg, &out) != ZONE_MAP_KEPT)
				return (uint8_t)(i - 1u);
			f->arg = out;
		}
#if CONFIG_ZONE_EDIT
		else if (f->scr == SCR_ZCHANNEL || f->scr == SCR_ZPICK || f->scr == SCR_ZONEEDIT || f->scr == SCR_ZONEDEL) {
			if (!ze_ok)
				return (uint8_t)(i - 1u);
		}
#endif
	}
	return sp;
}
#endif

#if CONFIG_CHANNEL_LIST
// ======================= Channels: every codeplug channel by number (hub 5) =======================
// The raw row is the channel number - 1 (with CONFIG_CHANNEL_EDIT, raw 0 is "New from current" and the raw
// row is the channel number); empty slots have no label, so the filter leaves them out
#define CHLIST_NEW	(CONFIG_CHANNEL_EDIT ? 1u : 0u)
#define CHLIST_NUM(raw)	((uint16_t)((raw) + 1u - CHLIST_NEW))
#if CONFIG_CHANNEL_EDIT
static bool ced_from_current;		// the next push of the channel opens a copy of what the radio uses now
#endif
#if CONFIG_ZONE_EDIT
static uint16_t ced_copy_of;		// the next push of the channel opens a copy of this one (Copy here, ui.md 2.2)
static bool ced_into;			// saving the copy puts it in zone ze_zone at ced_into_pos
static uint16_t ced_into_pos;
#endif

// The channel the radio is on, 0 in the VFO or when its zone can't be read
static uint16_t chlist_current(void)
{
	static zone_t z;
	const radio_view_t *v = ui_view();
	if (v->vfo || !codeplug_zone(v->zone_index, &z) || v->zone_pos >= z.count)
		return 0;
	return z.channels[v->zone_pos];
}

static uint16_t chlist_raw_count(void)
{
	return (uint16_t)(codeplug_channel_count() + CHLIST_NEW);
}

static bool chlist_raw_label(uint16_t raw, char *buf, uint8_t len)
{
	static channel_t ch;
	sbuf_t b;
	sb_init(&b, buf, len);
	if (CHLIST_NEW && raw == 0) {
		sb_str(&b, "New from current");
		return true;
	}
	if (!codeplug_channel(CHLIST_NUM(raw), &ch))
		return false;
	sb_u32(&b, CHLIST_NUM(raw), 1);
	sb_chr(&b, ' ');
	sb_str(&b, ch.name);
	return true;
}

static void chlist_enter(void)
{
	uint16_t cur = chlist_current();
	for (uint16_t i = 0; cur != 0 && i < ui_filter_count(); i++)
		if (CHLIST_NUM(ui_filter_raw(i)) == cur)
			ui_frame()->focus = i;
}

static void chlist_item(uint16_t i, ui_item_t *it)
{
	static channel_t ch;
	if (CHLIST_NEW && ui_filter_raw(i) == 0) {
		strcpy(it->label, "New from current");
		return;
	}
	uint16_t num = CHLIST_NUM(ui_filter_raw(i));
	if (!codeplug_channel(num, &ch))
		return;
	sbuf_t b;
	sb_init(&b, it->label, sizeof it->label);
	sb_u32(&b, num, 1);
	sb_chr(&b, ' ');
	sb_str(&b, ch.name);
	sb_init(&b, it->value, sizeof it->value);
	sb_channel_row(&b, &ch);
	if (num == chlist_current())
		it->flags |= UI_ITEM_TEMP;	// the channel we are on
}

// Go to channel num: in the zone in use if it holds it, else the first zone that does. false: no zone holds it
static bool chlist_tune(uint16_t num)
{
	static zone_t z;
	const radio_view_t *v = ui_view();
	uint16_t zones = codeplug_zone_count();
	for (uint16_t k = 0; k <= zones; k++) {
		// k == 0 tries the zone in use first, then every zone in order
		uint16_t zi = k == 0 ? v->zone_index : (uint16_t)(k - 1u);
		if ((k == 0 && v->vfo) || (k > 0 && zi == v->zone_index && !v->vfo) || !codeplug_zone(zi, &z))
			continue;
		for (uint16_t pos = 0; pos < z.count; pos++) {
			if (z.channels[pos] != num)
				continue;
			if (v->vfo)
				radio_req_vfo(false);
			if (v->zone_index != zi)
				radio_req_zone(zi);
			radio_req_zone_pos(pos);
			ui_go_home();
			return true;
		}
	}
	ui_toast("Not in any zone: add it to one to use it");
	return false;
}

static void chlist_select(uint16_t i, bool hold)
{
	(void)hold;
#if CONFIG_CHANNEL_EDIT
	if (ui_filter_raw(i) == 0) {
		uint16_t num = codeplug_free_channel();
		if (num == 0) {
			ui_toast("No free channel");
			return;
		}
		ced_from_current = true;
		ui_push(SCR_CHANNEL, num);
		return;
	}
#endif
	ui_push(SCR_CHANNEL, CHLIST_NUM(ui_filter_raw(i)));
}

static void chlist_context(uint16_t i)
{
	if (CHLIST_NEW && ui_filter_raw(i) == 0)
		return;
	chlist_tune(CHLIST_NUM(ui_filter_raw(i)));
}

static const ui_scr_def_t chlist_screen = {
	.name = "channels", .flags = SF_LIST | SF_FILTER | SF_FIELD, .prefix = "Channels",
	.hint = TEXT_ALT_OPEN "OK details  # go  0-9 find" TEXT_ALT_NEXT "OK details  # go" TEXT_ALT_CLOSE,
	.empty = "No channels",
	.count = ui_filter_count, .item = chlist_item, .select = chlist_select, .context = chlist_context,
	.enter = chlist_enter, .raw_count = chlist_raw_count, .raw_label = chlist_raw_label,
};

// ======================= One channel's settings, edited in place (ui.md 2.1) =======================
// Without CONFIG_CHANNEL_EDIT the rows are read only and OK goes to the channel

enum { CE_NAME, CE_NUMBER, CE_MODE, CE_RX, CE_TX, CE_POWER, CE_RX_ONLY, CE_BW, CE_RX_TONE, CE_TX_TONE, CE_VOX,
       CE_CC, CE_TS, CE_CONTACT, CE_LIST, CE_ID, CE_DMO, CE_BEEP, CE_ALIAS1, CE_ALIAS2, CE_TOT, CE_SCAN, CE_ECO,
       CE_ZONES, CE_GO, CE_SHARE, CE_SAVE, CE_SAVE_NEW, CE_COUNT, CE_NONE = 0xFF };

static channel_t ced_ch;		// what the screen shows (the working copy when editing)
static uint16_t ced_num;		// its number, 0 = no such channel
static uint8_t ced_rows[CE_COUNT];
static uint8_t ced_n;

#if CONFIG_CHANNEL_EDIT
#define CE_TOT_MAX	495
#define CE_TOT_STEP	15
#define CE_ID_MAX	((int32_t)CODEPLUG_DMR_ID_MAX)
#define CE_DIGITS	UI_ENTRY_MAX	// a frequency, or an ID (8 digits too)

static channel_t ced_orig;		// what is stored
static bool ced_new;			// not stored yet (New from current settings)
static uint8_t ced_edit = CE_NONE;	// the row whose editor is open
static int32_t ced_val;			// spinner: power level (power_count = the radio's), color code, time-out
static bool ced_typed;
static subtone_t ced_tone;		// spinner: a tone
static char ced_digits[CE_DIGITS + 1];	// typed frequency or ID
static uint8_t ced_len;

static bool tone_same(const subtone_t *a, const subtone_t *b)
{
	return a->type == b->type && (a->type == TONE_NONE || a->value == b->value);
}

// Where each row's field is in channel_t, for the changed mark: offset, size (0 = never changes), and the bit for the
// rows that share alias_tx
#define CE_FIELD(f)	{ offsetof(channel_t, f), sizeof(((channel_t *)0)->f), 0 }
static const struct { uint8_t off, len, bit; } ced_fields[CE_ZONES] = {
	[CE_NAME] = CE_FIELD(name), [CE_MODE] = CE_FIELD(mode), [CE_RX] = CE_FIELD(rx_hz), [CE_TX] = CE_FIELD(tx_hz),
	[CE_POWER] = CE_FIELD(power), [CE_RX_ONLY] = CE_FIELD(rx_only), [CE_BW] = CE_FIELD(bw),
	[CE_RX_TONE] = CE_FIELD(rx_tone), [CE_TX_TONE] = CE_FIELD(tx_tone), [CE_VOX] = CE_FIELD(vox),
	[CE_CC] = CE_FIELD(color_code), [CE_TS] = CE_FIELD(timeslot), [CE_CONTACT] = CE_FIELD(contact),
	[CE_LIST] = CE_FIELD(listen_list), [CE_ID] = CE_FIELD(dmr_id), [CE_DMO] = CE_FIELD(dmo),
	[CE_BEEP] = CE_FIELD(no_beep), [CE_ALIAS1] = { offsetof(channel_t, alias_tx), 1, CODEPLUG_ALIAS_TS1_TEXT },
	[CE_ALIAS2] = { offsetof(channel_t, alias_tx), 1, CODEPLUG_ALIAS_TS2_TEXT }, [CE_TOT] = CE_FIELD(tot_s),
	[CE_SCAN] = CE_FIELD(scan_skip), [CE_ECO] = CE_FIELD(no_eco),
};

// Whether row r differs from what is stored (the dot), or for CE_COUNT, any row
static bool ced_changed(uint8_t r)
{
	if (ced_new)
		return r == CE_COUNT;
	if (r == CE_COUNT) {
		for (uint8_t i = 0; i < CE_ZONES; i++)
			if (ced_changed(i))
				return true;
		return false;
	}
	if (r >= CE_ZONES)
		return false;
	const uint8_t *a = (const uint8_t *)&ced_ch + ced_fields[r].off, *b = (const uint8_t *)&ced_orig + ced_fields[r].off;
	if (r == CE_NAME)
		return strcmp((const char *)a, (const char *)b) != 0;	// the bytes past the end may differ
	if (r == CE_RX_TONE || r == CE_TX_TONE)
		return !tone_same((const subtone_t *)a, (const subtone_t *)b);
	if (ced_fields[r].bit != 0)
		return ((*a ^ *b) & ced_fields[r].bit) != 0;
	return memcmp(a, b, ced_fields[r].len) != 0;
}

// A copy of what the radio uses now (temporary changes included), named after it: "Name 2", or the frequency
static void ced_from_view(void)
{
	const radio_view_t *v = ui_view();
	ced_ch = v->ch;
	if (v->temp & RADIO_TEMP_POWER)
		ced_ch.power = v->power;
	ced_ch.roam = false;
	char name[CODEPLUG_NAME_LEN + 1];
	if (v->vfo) {
		sbuf_t b;
		sb_init(&b, name, sizeof name);
		sb_freq(&b, ced_ch.rx_hz);
	} else {
		zone_copy_name(name, ced_ch.name);	// "Name 2", cut to fit
	}
	strcpy(ced_ch.name, name);
}
#endif

static void ced_build(void)
{
	bool dmr = CONFIG_DMR && ced_ch.mode == RADIO_MODE_DMR;
	bool edit = CONFIG_CHANNEL_EDIT;
	bool fresh = false, changed = false;
#if CONFIG_CHANNEL_EDIT
	fresh = ced_new;
	changed = ced_changed(CE_COUNT);
#endif
	ced_n = 0;
	for (uint8_t r = 0; r < CE_COUNT; r++) {
		bool fm_only = r == CE_BW || r == CE_RX_TONE || r == CE_TX_TONE || r == CE_VOX;
		bool dmr_only = r >= CE_CC && r <= CE_ALIAS2;
		if ((fm_only && dmr) || (dmr_only && !dmr) || (r == CE_MODE && !CONFIG_DMR))
			continue;
		if ((r == CE_ZONES || r == CE_GO || r == CE_SHARE || r == CE_SAVE_NEW) && fresh)
			continue;
		if (r == CE_SHARE && !CONFIG_ZONE_SHARE)
			continue;
		if ((r == CE_SAVE && !(changed || fresh)) || ((r == CE_SAVE || r == CE_SAVE_NEW) && !edit))
			continue;
		ced_rows[ced_n++] = r;
	}
}

static void chinfo_enter(void)
{
	ced_num = ui_frame()->arg;
#if CONFIG_CHANNEL_EDIT
	ced_edit = CE_NONE;
	ced_new = ced_from_current;
	ced_from_current = false;
#if CONFIG_ZONE_EDIT
	// Copy here: the channel in use with its temporary changes, as New from current; another one as stored
	uint16_t src = ced_copy_of;
	const radio_view_t *v = ui_view();
	ced_into = src != 0;
	ced_copy_of = 0;
	if (ced_into && (v->vfo || v->channel_number != src)) {
		ced_new = true;
		if (!codeplug_channel(src, &ced_ch))
			ced_num = 0;
		ced_ch.roam = false;
		char name[CODEPLUG_NAME_LEN + 1];
		zone_copy_name(name, ced_ch.name);
		strcpy(ced_ch.name, name);
		ced_orig = ced_ch;
		return;
	}
	ced_new = ced_new || ced_into;
#endif
	if (ced_new) {
		ced_from_view();
		ced_orig = ced_ch;
		return;
	}
#endif
	if (!codeplug_channel(ced_num, &ced_ch))
		ced_num = 0;
#if CONFIG_CHANNEL_EDIT
	ced_orig = ced_ch;
#endif
}

static uint16_t chinfo_count(void)
{
	if (ced_num == 0)
		return 0;
	ced_build();
	return ced_n;
}

static const char *chinfo_title(void)
{
	return ced_ch.name[0] != 0 ? ced_ch.name : "New Channel";
}

// The zones holding the channel, as much as fits: "Fav, Local, Ga" / "Fav +2" / "3 zones" (text alternates)
static void sb_chinfo_zones(sbuf_t *b, uint16_t num)
{
	static zone_t z;
	char all[24], first[CODEPLUG_NAME_LEN + 1] = "";
	sbuf_t ab;
	sb_init(&ab, all, sizeof all);
	uint16_t found = 0;
	for (uint16_t zi = 0; zi < codeplug_zone_count(); zi++) {
		if (!codeplug_zone(zi, &z))
			continue;
		for (uint16_t pos = 0; pos < z.count; pos++) {
			if (z.channels[pos] != num)
				continue;
			if (found++)
				sb_str(&ab, ", ");
			else
				strcpy(first, z.name);
			sb_str(&ab, z.name);
			break;
		}
	}
	if (found == 0) {
		sb_str(b, "None");
		return;
	}
	sb_str(b, TEXT_ALT_OPEN);
	sb_str(b, all);
	if (found > 1) {
		sb_str(b, TEXT_ALT_NEXT);
		sb_str(b, first);
		sb_str(b, " +");
		sb_u32(b, found - 1u, 1);
	}
	sb_str(b, TEXT_ALT_NEXT);
	sb_u32(b, found, 1);
	sb_str(b, found == 1 ? " zone" : " zones");
	sb_str(b, TEXT_ALT_CLOSE);
}

static void sb_onoff(sbuf_t *b, bool on)
{
	sb_str(b, on ? "On" : "Off");
}

static void sb_ced_power(sbuf_t *b, int32_t level)
{
	sb_str(b, level < 0 || level >= board.power_count ? "Radio's" : ui_power_name((uint8_t)level));
}

static void sb_ced_tot(sbuf_t *b, int32_t s)
{
	if (s == 0) {
		sb_str(b, "Radio's");
		return;
	}
	sb_i32(b, s);
	sb_str(b, " s");
}

// The open spinner's value; the stored one in a build without editing (never reached there: nothing is edited)
static int32_t ced_val_or(int32_t stored)
{
#if CONFIG_CHANNEL_EDIT
	(void)stored;
	return ced_val;
#else
	return stored;
#endif
}

static void chinfo_item(uint16_t i, ui_item_t *it)
{
	static const char *const labels[CE_COUNT] = { "Name", "Number", "Mode", "RX", "TX", "Power", "RX only",
		"Bandwidth", "RX tone", "TX tone", "VOX", "Color code", "Timeslot", "TX contact", "RX group list", "Own ID",
		"Direct mode", "Talk permit beeps", "Talker alias TS1", "Talker alias TS2", "TX time-out", "Scan",
		"Battery saver", "Zones", "Go to it", GLYPH_SHARE " Share channel to...", "Save", "Save as new channel" };
	const channel_t *ch = &ced_ch;
	if (i >= ced_n)
		return;
	uint8_t r = ced_rows[i];
	bool editing = false;
#if CONFIG_CHANNEL_EDIT
	editing = ced_edit == r;
	if (editing)
		it->flags |= UI_ITEM_EDIT;
	if (ced_changed(r))
		it->flags |= UI_ITEM_TEMP;
#endif
	strcpy(it->label, labels[r]);
#if CONFIG_ZONE_SHARE
	if (r == CE_SHARE && !(feats() & UI_FEAT_ZONE_SHARE))
		it->flags |= UI_ITEM_DIM;
#endif
	sbuf_t b;
	sb_init(&b, it->value, sizeof it->value);
#if CONFIG_CHANNEL_EDIT
	if (editing && (r == CE_RX || r == CE_TX || r == CE_ID)) {
		// What has been typed so far: "446.07_", "3101_"
		for (uint8_t k = 0; k < ced_len; k++) {
			if (r != CE_ID && k == UI_ENTRY_MHZ)
				sb_chr(&b, '.');
			sb_chr(&b, ced_digits[k]);
		}
		sb_chr(&b, '_');
		return;
	}
#endif
	switch (r) {
	case CE_NAME:
		sb_str(&b, ch->name);
		break;
	case CE_NUMBER:
		sb_u32(&b, ced_num, 1);
		break;
	case CE_MODE:
		sb_str(&b, ch->mode == RADIO_MODE_DMR ? "DMR" : "FM");
		break;
	case CE_RX:
		sb_freq(&b, ch->rx_hz);
		break;
	case CE_TX:
		sb_freq(&b, ch->tx_hz);
		if (ch->tx_hz != ch->rx_hz) {
			// The repeater offset: "+5 MHz", "-600 kHz"
			int32_t off_khz = (int32_t)(ch->tx_hz / 1000u) - (int32_t)(ch->rx_hz / 1000u);
			bool mhz = off_khz % 1000 == 0;
			sb_str(&b, off_khz > 0 ? " +" : " ");
			sb_i32(&b, mhz ? off_khz / 1000 : off_khz);
			sb_str(&b, mhz ? " MHz" : " kHz");
		}
		break;
	case CE_POWER:
#if CONFIG_CHANNEL_EDIT
		if (editing) {
			sb_ced_power(&b, ced_val);
			break;
		}
#endif
		sb_ced_power(&b, ch->power == CODEPLUG_POWER_DEFAULT ? -1 : ch->power);
		break;
	case CE_RX_ONLY:
		sb_str(&b, ch->rx_only ? "Yes" : "No");
		break;
	case CE_BW:
		sb_str(&b, ch->bw == RADIO_BW_25K ? "25 kHz" : "12.5 kHz");
		break;
	case CE_RX_TONE:
	case CE_TX_TONE: {
		const subtone_t *t = r == CE_RX_TONE ? &ch->rx_tone : &ch->tx_tone;
#if CONFIG_CHANNEL_EDIT
		if (editing)
			t = &ced_tone;
#endif
		sb_tone(&b, t);
		break;
	}
	case CE_VOX:
		sb_onoff(&b, ch->vox);
		break;
	case CE_CC:
		sb_u32(&b, editing ? (uint32_t)ced_val_or(ch->color_code) : ch->color_code, 1);
		break;
	case CE_TS:
		sb_u32(&b, ch->timeslot, 1);
		break;
#if CONFIG_DMR
	case CE_CONTACT: {
		static contact_t c;
		if (ch->contact != 0 && codeplug_contact(ch->contact, &c))
			sb_str(&b, c.name);
		else
			sb_str(&b, "None");
		break;
	}
	case CE_LIST: {
		static listen_list_t l;
		if (ch->listen_list != 0 && codeplug_listen_list(ch->listen_list, &l))
			sb_str(&b, l.name);
		else
			sb_str(&b, "None");
		break;
	}
#endif
	case CE_ID:
		if (ch->dmr_id != 0)
			sb_u32(&b, ch->dmr_id, 1);
		else
			sb_str(&b, "Radio's");
		break;
	case CE_DMO:
		sb_onoff(&b, ch->dmo);
		break;
	case CE_BEEP:
		sb_onoff(&b, !ch->no_beep);
		break;
	case CE_ALIAS1:
		sb_onoff(&b, ch->alias_tx & CODEPLUG_ALIAS_TS1_TEXT);
		break;
	case CE_ALIAS2:
		sb_onoff(&b, ch->alias_tx & CODEPLUG_ALIAS_TS2_TEXT);
		break;
	case CE_TOT:
		sb_ced_tot(&b, editing ? ced_val_or(ch->tot_s) : ch->tot_s);
		break;
	case CE_SCAN:
		sb_str(&b, ch->scan_skip ? "Skipped" : "Scanned");
		break;
	case CE_ECO:
		sb_onoff(&b, !ch->no_eco);
		break;
	case CE_ZONES:
		sb_chinfo_zones(&b, ced_num);
		break;
	default:
		break;
	}
}

#if CONFIG_CHANNEL_EDIT
static void ui_text_open(char *target, uint8_t max);

// The first problem that keeps the copy from being saved, as a toast; NULL when it can be saved
static const char *ced_problem(void)
{
	const channel_t *c = &ced_ch;
	if (c->name[0] == 0)
		return "Give it a name first";
	if (!board_in_band(c->rx_hz, false))
		return "RX is outside the radio's bands";
	if (!c->rx_only && !board_in_band(c->tx_hz, true))
		return "TX can't transmit there: set RX only";
	if (CONFIG_DMR && c->mode == RADIO_MODE_DMR && c->contact == 0)
		return "Pick a TX contact first";
	return NULL;
}

// Write the copy: as a channel, or for Copy here as a channel put in its zone in the same job (ZONE_OP_ADD_NEW)
static bool ced_write(uint16_t num, bool as_new)
{
#if CONFIG_ZONE_EDIT
	if (ced_into && ced_new && !as_new) {
		ze_op_new(ZONE_OP_ADD_NEW, ced_into_pos)->channel = num;
		ze_op.ch = ced_ch;
		if (!ui_svc_zone_edit(&ze_op))
			return false;
		ced_into = false;
		zone_focus_num = num;	// the zone's list shows it when the editor closes
		return true;
	}
#endif
	(void)as_new;
	return ui_svc_write_channel(num, &ced_ch);
}

static void ced_save(bool as_new)
{
	static char msg[32];
	const char *why = ced_problem();
	if (why != NULL) {
		ui_toast(why);
		return;
	}
	if (!as_new && !ced_new && !ced_changed(CE_COUNT)) {
		ui_toast("No changes to save");
		return;
	}
	uint16_t num = as_new ? codeplug_free_channel() : ced_num;
	if (num == 0) {
		ui_toast("No free channel");
		return;
	}
	if (!ced_write(num, as_new)) {
		ui_toast("Couldn't save now: try again");
		return;
	}
	ced_num = num;
	ui_frame()->arg = num;
	ced_orig = ced_ch;
	sbuf_t b;
	sb_init(&b, msg, sizeof msg);
	sb_str(&b, as_new || ced_new ? "Saved as channel " : "Saved channel ");
	sb_u32(&b, num, 1);
	ced_new = false;
	ui_toast(msg);
}

static void ced_spin(int dir)
{
	switch (ced_edit) {
	case CE_POWER: {
		int32_t n = board.power_count + 1;	// the levels, then the radio's
		ced_val = (ced_val + dir + n) % n;
		break;
	}
	case CE_CC:
		ced_val = (ced_val + dir + 16) % 16;
		break;
	case CE_TOT:
		ced_val += dir * CE_TOT_STEP;
		ced_val = ced_val < 0 ? 0 : ced_val > CE_TOT_MAX ? CE_TOT_MAX : ced_val;
		break;
	case CE_RX_TONE:
	case CE_TX_TONE:
		ced_tone = tones_step(&ced_tone, dir);
		break;
	default:
		break;
	}
	ced_typed = false;
}

// A digit into the open editor: frequency and ID digits are appended (refused when no band or ID can start so),
// numbers typed as in Settings (starting over when too big). An ID's value builds up in ced_val as it is typed
static void ced_digit(char d)
{
	int32_t v = ced_val * 10 + (d - '0');
	switch (ced_edit) {
	case CE_RX:
	case CE_TX:
		ui_freq_entry_digit(ced_digits, &ced_len, d);
		break;
	case CE_ID:
		if (ced_len >= CE_DIGITS)
			break;
		if (v > CE_ID_MAX) {
			ui_toast("IDs go up to 16776415");
			break;
		}
		ced_val = v;
		ced_digits[ced_len++] = d;
		ced_digits[ced_len] = 0;
		break;
	case CE_CC:
	case CE_TOT: {
		int32_t hi = ced_edit == CE_CC ? 15 : CE_TOT_MAX;
		v = ced_typed ? v : d - '0';
		ced_val = v > hi ? d - '0' : v;
		ced_typed = true;
		break;
	}
	default:
		break;
	}
}

// OK in the open editor: the value goes into the copy (or a toast says why not, and the editor stays open)
static void ced_commit(void)
{
	channel_t *c = &ced_ch;
	switch (ced_edit) {
	case CE_RX:
	case CE_TX: {
		if (ced_len == 0)
			break;		// nothing typed: unchanged
		uint32_t hz = ui_freq_entry_hz(ced_digits, ced_len);
		if (hz == 0)
			return;
		if (ced_edit == CE_RX) {
			// TX moves with RX, keeping a repeater's offset; where that lands outside every band (or the channel
			// had no TX frequency) TX becomes RX
			uint32_t tx = c->tx_hz - c->rx_hz + hz;
			c->tx_hz = board_in_band(tx, false) ? tx : hz;
			c->rx_hz = hz;
		} else {
			c->tx_hz = hz;
		}
		break;
	}
	case CE_ID:
		if (ced_len > 0)
			c->dmr_id = (uint32_t)ced_val;	// 0 = the radio's
		break;
	case CE_POWER:
		c->power = ced_val >= board.power_count ? CODEPLUG_POWER_DEFAULT : (uint8_t)ced_val;
		break;
	case CE_CC:
		c->color_code = (uint8_t)ced_val;
		break;
	case CE_TOT:
		if (ced_val % CE_TOT_STEP != 0) {
			ui_toast("Not changed: 0 to 495, steps of 15");
			break;
		}
		c->tot_s = (uint16_t)ced_val;
		break;
	case CE_RX_TONE:
		c->rx_tone = ced_tone;
		break;
	case CE_TX_TONE:
		c->tx_tone = ced_tone;
		break;
	default:
		break;
	}
	ced_edit = CE_NONE;
}

static bool chinfo_key(const input_event_t *ev)
{
	bool press = ev->gesture == GESTURE_PRESS || ev->gesture == GESTURE_LONG;
	if (ced_edit == CE_NONE) {
		if (ev->action == ACT_BACK && ev->gesture == GESTURE_PRESS && ced_num != 0 &&
				(ced_new || ced_changed(CE_COUNT))) {
			ui_push(SCR_DISCARD, 0);
			return true;
		}
		return false;
	}
	int dir = ev->action == ACT_NAV_UP ? 1 : ev->action == ACT_NAV_DOWN ? -1 :
		ev->action == ACT_ENC_STEP ? (ev->value > 0 ? 1 : -1) : 0;
	if (dir != 0 && ev->gesture != GESTURE_UP) {
		if (ced_edit != CE_RX && ced_edit != CE_TX && ced_edit != CE_ID)
			ced_spin(dir);	// typed fields ignore the knob, so the focus can't move under them either
		ui_dirty();
		return true;
	}
	if (ev->action >= ACT_DIGIT_0 && ev->action <= ACT_DIGIT_9 && press) {
		ced_digit((char)('0' + ev->action - ACT_DIGIT_0));
		ui_dirty();
		return true;
	}
	if (ev->action == ACT_BACK && ev->gesture == GESTURE_PRESS) {
		if ((ced_edit == CE_RX || ced_edit == CE_TX || ced_edit == CE_ID) && ced_len > 0) {
			ced_digits[--ced_len] = 0;	// takes the last digit back
			ced_val /= 10;
		} else {
			ced_edit = CE_NONE;		// leaves the editor, the row unchanged
		}
		ui_dirty();
		return true;
	}
	if (ev->action == ACT_SELECT && press) {
		ced_commit();
		ui_dirty();
		return true;
	}
	return false;
}
#endif

static void chinfo_select(uint16_t i, bool hold)
{
#if CONFIG_ZONE_SHARE
	if (i < ced_n && ced_rows[i] == CE_SHARE && !hold) {
#if CONFIG_CHANNEL_EDIT
		if (ced_changed(CE_COUNT)) {
			ui_toast("Save first: hold OK");	// the send reads the stored channel
			return;
		}
#endif
		share_send_to(UI_ZONE_NONE, ced_num, true, ced_ch.name);
		return;
	}
#endif
#if CONFIG_CHANNEL_EDIT
	if (i >= ced_n)
		return;
	if (hold) {
		ced_save(false);	// long OK saves, as it makes a quick setting permanent
		return;
	}
	channel_t *c = &ced_ch;
	uint8_t r = ced_rows[i];
	switch (r) {
	case CE_NAME: ui_text_open(c->name, CODEPLUG_NAME_LEN); break;
	case CE_MODE: c->mode = c->mode == RADIO_MODE_DMR ? RADIO_MODE_FM : RADIO_MODE_DMR; break;
	case CE_RX:
	case CE_TX:
	case CE_ID:
		ced_edit = r;
		ced_len = 0;
		ced_val = 0;
		ced_digits[0] = 0;
		break;
	case CE_POWER:
		ced_edit = r;
		ced_val = c->power < board.power_count ? c->power : board.power_count;
		break;
	case CE_RX_ONLY: c->rx_only = !c->rx_only; break;
	case CE_BW: c->bw = c->bw == RADIO_BW_25K ? RADIO_BW_12K5 : RADIO_BW_25K; break;
	case CE_RX_TONE:
	case CE_TX_TONE:
		if (!CONFIG_SUBTONE) {
			// The spinner would only offer none, and OK would wipe the codeplug's tone
			ui_toast("Tones: not in this build");
			break;
		}
		ced_edit = r;
		ced_tone = r == CE_RX_TONE ? c->rx_tone : c->tx_tone;
		break;
	case CE_VOX: c->vox = !c->vox; break;
	case CE_CC:
	case CE_TOT:
		ced_edit = r;
		ced_val = r == CE_CC ? c->color_code : c->tot_s;
		ced_typed = false;
		break;
	case CE_TS: c->timeslot = c->timeslot == 2 ? 1 : 2; break;
#if CONFIG_DMR
	case CE_CONTACT: ui_push(SCR_CHPICK, 0); break;
	case CE_LIST: ui_push(SCR_CHPICK, 1); break;
#endif
	case CE_DMO: c->dmo = !c->dmo; break;
	case CE_BEEP: c->no_beep = !c->no_beep; break;
	case CE_ALIAS1: c->alias_tx ^= CODEPLUG_ALIAS_TS1_TEXT; break;
	case CE_ALIAS2: c->alias_tx ^= CODEPLUG_ALIAS_TS2_TEXT; break;
	case CE_SCAN: c->scan_skip = !c->scan_skip; break;
	case CE_ECO: c->no_eco = !c->no_eco; break;
	case CE_NUMBER: ui_toast("Save as new channel to use another number"); break;
#if CONFIG_ZONE_EDIT
	case CE_ZONES: ui_toast("Zones: add it from a zone's list"); break;
#else
	case CE_ZONES: ui_toast("Zones: change them with qdmr for now"); break;
#endif
	case CE_GO:
		if (ced_changed(CE_COUNT))
			ui_toast("Save first: hold OK");
		else
			chlist_tune(ced_num);
		break;
	case CE_SAVE: ced_save(false); break;
	case CE_SAVE_NEW: ced_save(true); break;
	default: break;
	}
	ui_dirty();
#else
	(void)i;
	(void)hold;
	chlist_tune(ui_frame()->arg);
#endif
}

static const ui_scr_def_t chinfo_screen = {
	.name = "channel", .flags = SF_LIST | SF_FIELD, .empty = "Empty channel",
#if CONFIG_CHANNEL_EDIT
	.hint = TEXT_ALT_OPEN "OK change  hold OK save  Back" TEXT_ALT_NEXT "OK change  hold OK save" TEXT_ALT_CLOSE,
	.key = chinfo_key,
#else
	.hint = "OK go to it  Back",
#endif
	.count = chinfo_count, .item = chinfo_item, .select = chinfo_select, .title = chinfo_title,
	.enter = chinfo_enter,
};

#if CONFIG_CHANNEL_EDIT
// ======================= Leaving the channel editor with changes (defaults to keep editing; P7) ===============

static uint16_t discard_count(void) { return 2; }

static void discard_item(uint16_t i, ui_item_t *it)
{
	strcpy(it->label, i == 0 ? "No, keep editing" : "Yes, discard");
	if (i == 1)
		it->flags |= UI_ITEM_WARN;
}

static void discard_select(uint16_t i, bool hold)
{
	(void)hold;
	ui_pop();		// this question
	if (i == 1)
		ui_pop();	// and the channel
}

static const char *discard_body(void) { return "Discard the changes to this channel?"; }

static const ui_scr_def_t discard_screen = {
	.name = "discard", .flags = SF_LIST, .prefix = "Unsaved Changes", .hint = TEXT_ALT_OPEN "OK choose  Back keep editing" TEXT_ALT_NEXT "OK choose  Back" TEXT_ALT_CLOSE,
	.count = discard_count, .item = discard_item, .select = discard_select, .body = discard_body,
};

// ======================= Text entry: keypad letters (ui.md 2.1) =======================
// Phone keypad: in ABC / abc, 2-9 cycle their letters, the digit, then the letters in the other case (ABC: A B C 2 a b
// c); 1 cycles punctuation and 0 a space, 0 and a few signs. The same key within 1 s takes the next one, another key
// or the pause moves on. In 123 a digit types itself, as a held digit does
// in any mode. # cycles ABC -> 123 -> abc. BACK deletes the last character and leaves once the field is empty, without
// a change (the phone convention; the one exception to P4, principles 4); * or Left delete too. OK keeps. Without a
// keypad Up / Down step the last character (down past the space deletes it), OK adds one and long OK keeps

#define TEXT_MULTI_MS	1000u

static char *text_target;
static void (*text_done)(void);		// called once the text is kept (NULL: none)
static uint8_t text_max;
static char text_buf[CODEPLUG_NAME_LEN + 1];
static uint8_t text_len;
static int8_t text_key = -1;		// the key that typed the last character, -1 = none to cycle
static uint8_t text_idx;
static uint32_t text_ms;
enum { TEXT_UPPER, TEXT_DIGITS, TEXT_LOWER, TEXT_MODES };	// the order # cycles through
static uint8_t text_mode;

static void ui_text_open(char *target, uint8_t max)
{
	text_target = target;
	text_done = NULL;
	text_max = max < CODEPLUG_NAME_LEN ? max : CODEPLUG_NAME_LEN;
	ui_push(SCR_TEXT, 0);
}

static void text_enter(void)
{
	strncpy(text_buf, text_target, text_max);
	text_buf[text_max] = 0;
	text_len = (uint8_t)strlen(text_buf);
	text_key = -1;
	text_mode = TEXT_UPPER;
}

static uint16_t text_count(void) { return 1; }

static void text_item(uint16_t i, ui_item_t *it)
{
	(void)i;
	sbuf_t b;
	sb_init(&b, it->label, sizeof it->label);
	sb_str(&b, text_buf);
	sb_chr(&b, '_');	// the cursor; the mode is in the title
}

static char text_case(char c)
{
	return text_mode == TEXT_LOWER && c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

// The keys with letters cycle letters, digit, then the letters again in the other case; the others their set as is
static uint8_t text_cycle_len(const char *set)
{
	uint8_t n = (uint8_t)strlen(set);
	return set[0] >= 'A' && set[0] <= 'Z' ? (uint8_t)(2u * n - 1u) : n;
}

static char text_char(const char *set, uint8_t idx)
{
	uint8_t n = (uint8_t)strlen(set);
	if (!(set[0] >= 'A' && set[0] <= 'Z'))
		return set[idx];
	if (idx < n)
		return idx == n - 1u ? set[idx] : text_case(set[idx]);	// the letters in the mode's case, then the digit
	char c = set[idx - n];
	return text_mode == TEXT_LOWER ? c : (char)(c - 'A' + 'a');	// then the other case
}

static void text_append(char c)
{
	if (text_len >= text_max) {
		ui_toast("That's the most it can hold");
		return;
	}
	text_buf[text_len++] = c;
	text_buf[text_len] = 0;
}

static bool text_key_fn(const input_event_t *ev)
{
	// A key's characters within the timing window: its letters, its digit, its letters in the other case (text_char)
	static const char *const keys[10] = { " 0+#*", ".-/,'&()@_!?1", "ABC2", "DEF3", "GHI4", "JKL5", "MNO6", "PQRS7",
		"TUV8", "WXYZ9" };
	bool press = ev->gesture == GESTURE_PRESS || ev->gesture == GESTURE_LONG;
	uint32_t now = hal_millis();
	if (ev->action >= ACT_DIGIT_0 && ev->action <= ACT_DIGIT_9 && press) {
		int8_t k = (int8_t)(ev->action - ACT_DIGIT_0);
		const char *set = keys[k];
		if (text_mode == TEXT_DIGITS || ev->gesture == GESTURE_LONG) {
			text_append((char)('0' + k));	// 123, or a held digit: the digit itself
			text_key = -1;
		} else if (k == text_key && text_len > 0 && now - text_ms < TEXT_MULTI_MS) {
			text_idx = (uint8_t)((text_idx + 1u) % text_cycle_len(set));
			text_buf[text_len - 1] = text_char(set, text_idx);
			text_key = k;
		} else {
			text_idx = 0;
			text_append(text_char(set, 0));
			text_key = k;
		}
		text_ms = now;
		ui_dirty();
		return true;
	}
	int dir = ev->action == ACT_NAV_UP ? 1 : ev->action == ACT_NAV_DOWN ? -1 :
		ev->action == ACT_ENC_STEP ? (ev->value > 0 ? 1 : -1) : 0;
	if (dir != 0 && ev->gesture != GESTURE_UP) {
		// Step the last character through the printable ones (the way in without a keypad); down past the space
		// deletes it
		if (text_len == 0)
			text_append('A');
		else if (dir < 0 && text_buf[text_len - 1] == ' ')
			text_buf[--text_len] = 0;
		else
			text_buf[text_len - 1] = (char)(' ' + (text_buf[text_len - 1] - ' ' + dir + 95) % 95);
		text_key = -1;
		ui_dirty();
		return true;
	}
	if (ev->action == ACT_CONTEXT && ev->gesture == GESTURE_PRESS) {
		text_mode = (uint8_t)((text_mode + 1u) % TEXT_MODES);
		text_key = -1;	// the next key starts a new character
		ui_dirty();
		return true;
	}
	if (ev->action == ACT_BACK && ev->gesture == GESTURE_PRESS && text_len == 0) {
		ui_pop();	// an empty field: BACK leaves, the name as it was
		return true;
	}
	if (((ev->action == ACT_BACK || ev->action == ACT_NAV_LEFT) && ev->gesture == GESTURE_PRESS) ||
			(ev->action == ACT_QUICK && press)) {	// a slow * deletes too (it doesn't lock here)
		if (text_len > 0)
			text_buf[--text_len] = 0;
		text_key = -1;
		ui_dirty();
		return true;
	}
	if (ev->action == ACT_SELECT && press) {
		if (!(board.caps & CAP_KEYPAD) && ev->gesture == GESTURE_PRESS) {
			text_append(text_len > 0 ? text_buf[text_len - 1] : 'A');	// the next character, started as this one
		} else {
			strcpy(text_target, text_buf);
			ui_pop();
			if (text_done != NULL)
				text_done();
		}
		ui_dirty();
		return true;
	}
	return false;
}

// The title carries the mode, so it shows however long the name is: "Name · ABC"
static const char *text_title(void)
{
	static const char *const modes[TEXT_MODES] = { "ABC", "123", "abc" };
	return modes[text_mode];
}

static const ui_scr_def_t text_screen = {
	.name = "text", .flags = SF_LIST, .prefix = "Name", .title = text_title,
	.hint = TEXT_ALT_OPEN "Back delete  # ABC/123/abc  OK keep" TEXT_ALT_NEXT "Back del  # mode  OK" TEXT_ALT_CLOSE,
	.count = text_count, .item = text_item, .key = text_key_fn, .enter = text_enter,
};

#if CONFIG_ZONE_EDIT
// ======================= The zone editor (ui.md 2.2) =======================
// Channel actions (arg: the position), Add channel, Zone options and Delete zone work on zone ze_zone. ze_z is the
// zone as these screens show it, read when one opens and changed as the change is posted, so it is right before the
// job has run

static zone_t ze_z;

static void ze_read(void)
{
	if (!codeplug_zone(ze_zone, &ze_z))
		memset(&ze_z, 0, sizeof ze_z);
}

static const char *ze_title(void) { return ze_z.name; }

// --- Channel actions ---
enum { ZC_UP, ZC_DOWN, ZC_COPY, ZC_EDIT,
#if CONFIG_ZONE_SHARE
	ZC_SEND,	// zone sharing (zone-share.md 7)
#endif
	ZC_REMOVE, ZC_COUNT };
static char zc_name[CODEPLUG_NAME_LEN + 1];

static void zchan_enter(void)
{
	static channel_t ch;
	ze_read();
	uint16_t pos = ui_frame()->arg;
	zc_name[0] = 0;
	if (pos < ze_z.count && codeplug_channel(ze_z.channels[pos], &ch))
		strcpy(zc_name, ch.name);
}

static uint16_t zchan_count(void) { return ZC_COUNT; }

static void zchan_item(uint16_t i, ui_item_t *it)
{
	static const char *const names[ZC_COUNT] = {
		[ZC_UP] = "Move up", [ZC_DOWN] = "Move down", [ZC_COPY] = "Copy here", [ZC_EDIT] = "Edit channel",
#if CONFIG_ZONE_SHARE
		[ZC_SEND] = SHARE_CHANNEL,
#endif
		[ZC_REMOVE] = "Remove from zone" };
	strcpy(it->label, names[i]);
	item_num(it, (uint16_t)(i + 1u));
	if (i == ZC_REMOVE)
		it->flags |= UI_ITEM_WARN;
#if CONFIG_ZONE_SHARE
	if (i == ZC_SEND && !(feats() & UI_FEAT_ZONE_SHARE))
		it->flags |= UI_ITEM_DIM;
#endif
}

static void zchan_select(uint16_t i, bool hold)
{
	(void)hold;
	uint16_t pos = ui_frame()->arg;
	if (pos >= ze_z.count)
		return;
	uint16_t num = ze_z.channels[pos];
	if (i == ZC_UP || i == ZC_DOWN) {
		int dir = i == ZC_UP ? -1 : 1;
		if (zone_list_move(&ze_z, pos, dir) != ZONE_EDIT_OK) {
			ui_toast(dir < 0 ? "Already first" : "Already last");
			return;
		}
		ze_op_new(ZONE_OP_MOVE, pos)->dir = (int8_t)dir;
		if (!ze_post()) {
			zone_list_move(&ze_z, (uint16_t)(pos + dir), -dir);
			return;
		}
		pos = (uint16_t)(pos + dir);
		ui_frame()->arg = pos;		// the menu stays on the channel: the digit again moves it on
		zone_focus_num = num;
		sbuf_t b;
		sb_init(&b, ze_msg, sizeof ze_msg);
		sb_str(&b, "Position ");
		sb_u32(&b, pos + 1u, 1);
		sb_str(&b, " of ");
		sb_u32(&b, ze_z.count, 1);
		ui_toast(ze_msg);
	} else if (i == ZC_COPY) {
		uint16_t free = codeplug_free_channel();
		if (free == 0) {
			ui_toast("No free channel");
		} else if (ze_z.count >= codeplug_zone_capacity()) {
			ui_toast("Zone full");
		} else {
			ced_copy_of = num;
			ced_into_pos = (uint16_t)(pos + 1u);
			ui_pop();	// the editor takes the menu's place: the stack stays within its six frames
			ui_push(SCR_CHANNEL, free);
		}
	} else if (i == ZC_EDIT) {
		ui_pop();
		ui_push(SCR_CHANNEL, num);
#if CONFIG_ZONE_SHARE
	} else if (i == ZC_SEND) {
		share_send_to(ze_zone, pos, true, ze_z.name);
#endif
	} else if (i == ZC_REMOVE) {
		ze_op_new(ZONE_OP_REMOVE, pos);
		if (ze_post()) {
			ui_pop();
			ze_toast("Removed from ", ze_z.name);
		}
	}
}

static const char *zchan_title(void) { return zc_name; }

static const ui_scr_def_t zchan_screen = {
	.name = "zchannel", .flags = SF_LIST | SF_NUMBERED, .hint = CONFIG_ZONE_SHARE ? "1-6 pick  Back" : "1-5 pick  Back",
	.count = zchan_count, .item = zchan_item, .select = zchan_select, .title = zchan_title, .enter = zchan_enter,
};

// --- Add channel: the stored channels the zone doesn't hold (raw r: channel r + 1) ---
static uint16_t zpick_raw_count(void)
{
	ze_read();
	return codeplug_channel_count();
}

static channel_t zpick_ch;	// the channel zpick_raw_label read last

static bool zpick_raw_label(uint16_t raw, char *buf, uint8_t len)
{
	uint16_t num = (uint16_t)(raw + 1u);
	if (zone_list_find(&ze_z, num) >= 0 || !codeplug_channel(num, &zpick_ch))
		return false;
	sbuf_t b;
	sb_init(&b, buf, len);
	sb_u32(&b, num, 1);
	sb_chr(&b, ' ');
	sb_str(&b, zpick_ch.name);
	return true;
}

static void zpick_item(uint16_t i, ui_item_t *it)
{
	if (!zpick_raw_label(ui_filter_raw(i), it->label, sizeof it->label))
		return;
	sbuf_t b;
	sb_init(&b, it->value, sizeof it->value);
	sb_channel_row(&b, &zpick_ch);
}

static void zpick_select(uint16_t i, bool hold)
{
	(void)hold;
	uint16_t num = (uint16_t)(ui_filter_raw(i) + 1u);
	const radio_view_t *v = ui_view();
	if (ze_z.count >= codeplug_zone_capacity()) {
		ui_toast("Zone full");
		return;
	}
	// After the channel in use when this is the zone in use, else at the end
	bool here = !v->vfo && v->zone_index == ze_zone && v->zone_pos < ze_z.count;
	ze_op_new(ZONE_OP_INSERT, here ? (uint16_t)(v->zone_pos + 1u) : ze_z.count)->channel = num;
	if (!ze_post())
		return;
	zone_focus_num = num;
	ui_pop();
	ui_filter_reset();	// the whole list again (the 2 typed to find Add channel), on the channel added
	ui_menus_resume();
	ze_toast("Added to ", ze_z.name);
}

static const ui_scr_def_t zpick_screen = {
	.name = "zpick", .flags = SF_LIST | SF_FILTER, .prefix = "Add Channel",
	.hint = TEXT_ALT_OPEN "OK add  0-9 find  Back" TEXT_ALT_NEXT "OK add  Back" TEXT_ALT_CLOSE,
	.empty = "Every channel is in it",
	.count = ui_filter_count, .item = zpick_item, .select = zpick_select,
	.raw_count = zpick_raw_count, .raw_label = zpick_raw_label,
};

// --- Zone options ---
enum { ZO_RENAME, ZO_COPY,
#if CONFIG_ZONE_SHARE
	ZO_SEND,	// zone sharing (zone-share.md 7)
#endif
	ZO_DELETE, ZO_COUNT };
static char ze_name[CODEPLUG_NAME_LEN + 1];

static uint16_t zopt_count(void) { return ZO_COUNT; }

static void zopt_item(uint16_t i, ui_item_t *it)
{
	static const char *const names[ZO_COUNT] = {
		[ZO_RENAME] = "Rename", [ZO_COPY] = "Copy as new zone",
#if CONFIG_ZONE_SHARE
		[ZO_SEND] = SHARE_ZONE,
#endif
		[ZO_DELETE] = "Delete zone" };
	strcpy(it->label, names[i]);
	item_num(it, (uint16_t)(i + 1u));
	if (i == ZO_DELETE)
		it->flags |= UI_ITEM_WARN;
#if CONFIG_ZONE_SHARE
	if (i == ZO_SEND && !(feats() & UI_FEAT_ZONE_SHARE))
		it->flags |= UI_ITEM_DIM;
#endif
}

static void ze_renamed(void)
{
	if (ze_name[0] == 0) {
		ui_toast("Give it a name first");
		return;
	}
	strcpy(ze_op_new(ZONE_OP_RENAME, 0)->name, ze_name);
	if (ze_post()) {
		strcpy(ze_z.name, ze_name);
		ui_toast("Renamed");
	}
}

static void zopt_select(uint16_t i, bool hold)
{
	(void)hold;
	if (i == ZO_RENAME) {
		strcpy(ze_name, ze_z.name);
		ui_text_open(ze_name, CODEPLUG_NAME_LEN);
		text_done = ze_renamed;
	} else if (i == ZO_COPY) {
		if (codeplug_zone_count() >= CODEPLUG_ZONES_MAX) {
			ui_toast("No room for another zone");
			return;
		}
		ze_op_new(ZONE_OP_COPY, 0);
		if (!ze_post())
			return;
		// Back to Zones: the copy can take a slot ahead of this zone and move its index
		ui_pop();
		ui_pop();
		char name[CODEPLUG_NAME_LEN + 1];
		zone_copy_name(name, ze_z.name);
		ze_toast("Copied as ", name);
#if CONFIG_ZONE_SHARE
	} else if (i == ZO_SEND) {
		if (ze_z.count == 0)
			ui_toast("Empty zone: nothing to send");
		else
			share_send_to(ze_zone, 0, false, ze_z.name);
#endif
	} else if (!codeplug_zones_stored()) {
		ui_toast("It holds every channel: it stays");
	} else {
		ui_push(SCR_ZONEDEL, 0);
	}
}

static const ui_scr_def_t zopt_screen = {
	.name = "zoneedit", .flags = SF_LIST | SF_NUMBERED, .prefix = "Zone",
	.hint = CONFIG_ZONE_SHARE ? "1-4 pick  Back" : "1-3 pick  Back",
	.count = zopt_count, .item = zopt_item, .select = zopt_select, .title = ze_title, .enter = ze_read,
};

// --- Delete zone (destructive: asks, defaults to No; P7) ---
static uint16_t zdel_count(void) { return 2; }

static void zdel_item(uint16_t i, ui_item_t *it)
{
	strcpy(it->label, i == 0 ? "No, keep it" : "Yes, delete");
	if (i == 1)
		it->flags |= UI_ITEM_WARN;
}

static void zdel_select(uint16_t i, bool hold)
{
	(void)hold;
	ui_pop();		// this question
	if (i == 0)
		return;
	ze_op_new(ZONE_OP_DELETE, 0);
	if (!ze_post())
		return;
	ui_pop();		// Zone options
	ui_pop();		// and the zone's list: back to Zones
	ze_toast("Deleted ", ze_z.name);
}

// The codeplug changed and nothing of ours is queued (our last change written, or refused, or a CPS write): the
// editor's screens show the zone as stored. Channel actions stay on their channel, found by number; it closes when
// the channel is no longer in the zone
void ui_menus_codeplug(void)
{
	ui_frame_t *f = ui_frame();
	if (f->scr != SCR_ZCHANNEL && f->scr != SCR_ZPICK && f->scr != SCR_ZONEEDIT && f->scr != SCR_ZONEDEL)
		return;
	uint16_t num = f->scr == SCR_ZCHANNEL && f->arg < ze_z.count ? ze_z.channels[f->arg] : 0;
	ze_read();
	if (f->scr == SCR_ZCHANNEL) {
		int at = zone_list_find(&ze_z, num);
		if (at < 0) {
			ui_pop();
			return;
		}
		f->arg = (uint16_t)at;
		zone_focus_num = num;
	}
}

static const char *zdel_body(void)
{
	sbuf_t b;
	sb_init(&b, ze_msg, sizeof ze_msg);
	sb_str(&b, "Delete ");
	sb_str(&b, ze_z.name);
	sb_str(&b, "? Channels stay.");
	return ze_msg;
}

static const ui_scr_def_t zdel_screen = {
	.name = "zonedel", .flags = SF_LIST, .prefix = "Delete Zone", .hint = "OK choose  Back cancel",
	.count = zdel_count, .item = zdel_item, .select = zdel_select, .body = zdel_body,
};
#endif


#if CONFIG_DMR
// ======================= A channel's TX contact or RX group list (arg 0 / 1) =======================
// Raw row 0 is None, raw r the contact or list with index r

static uint16_t chpick_raw_count(void)
{
	return (uint16_t)((ui_frame()->arg == 0 ? CODEPLUG_CONTACTS_MAX : CODEPLUG_LISTS_MAX) + 1u);
}

static contact_t chpick_c;	// the contact chpick_raw_label read last, for the row's value

static bool chpick_raw_label(uint16_t raw, char *buf, uint8_t len)
{
	static listen_list_t l;
	const char *name = "None";
	if (raw != 0) {
		if (ui_frame()->arg == 0 ? !codeplug_contact(raw, &chpick_c) : !codeplug_listen_list(raw, &l))
			return false;
		name = ui_frame()->arg == 0 ? chpick_c.name : l.name;
	}
	sbuf_t b;
	sb_init(&b, buf, len);
	sb_str(&b, name);
	return true;
}

static uint16_t chpick_current(void)
{
	return ui_frame()->arg == 0 ? ced_ch.contact : ced_ch.listen_list;
}

static void chpick_enter(void)
{
	for (uint16_t i = 0; i < ui_filter_count(); i++)
		if (ui_filter_raw(i) == chpick_current())
			ui_frame()->focus = i;
}

static void chpick_item(uint16_t i, ui_item_t *it)
{
	uint16_t raw = ui_filter_raw(i);
	if (chpick_raw_label(raw, it->label, sizeof it->label) && raw != 0 && ui_frame()->arg == 0) {
		sbuf_t b;
		sb_init(&b, it->value, sizeof it->value);
		sb_str(&b, chpick_c.type == CONTACT_PRIVATE ? "ID " : "TG ");
		sb_u32(&b, chpick_c.id, 1);
	}
	if (raw == chpick_current())
		it->flags |= UI_ITEM_TEMP;
}

static void chpick_select(uint16_t i, bool hold)
{
	(void)hold;
	uint16_t raw = ui_filter_raw(i);
	if (ui_frame()->arg == 0)
		ced_ch.contact = raw;
	else
		ced_ch.listen_list = raw;
	ui_pop();
}

static const char *chpick_title(void) { return ui_frame()->arg == 0 ? "TX Contact" : "RX Group List"; }

static const ui_scr_def_t chpick_screen = {
	.name = "chpick", .flags = SF_LIST | SF_FILTER, .hint = "OK pick  0-9 find  Back", .empty = "None stored",
	.count = ui_filter_count, .item = chpick_item, .select = chpick_select, .title = chpick_title,
	.enter = chpick_enter, .raw_count = chpick_raw_count, .raw_label = chpick_raw_label,
};
#endif
#endif
#endif

// ======================= Info pages =======================

enum { INFO_POSITION, INFO_HOTSPOT, INFO_BATTERY, INFO_FIRMWARE, INFO_CODEPLUG, INFO_MY_RADIO, INFO_USB_NET,
       INFO_SFR };

static const char *const info_titles[] = { "Position", "Hotspot", "Battery", "About", "Codeplug", "My Radio",
					   "USB Network", "Single-Freq Repeater" };

// The USB personality can be switched here (the host link's network adapter, CONFIG_USB_NETWORK)
#define USB_NET_UI	(CONFIG_USB_NETWORK && CONFIG_HOSTLINK)

static const char *info_title(void)
{
	uint16_t a = ui_frame()->arg;
	return a == INFO_BATTERY && board_volts() == BOARD_VOLTS_SUPPLY ? "Supply" : info_titles[a];
}

// mV as volts with one decimal: 13.8 V
static void sb_volts(sbuf_t *b, uint16_t mv)
{
	uint32_t dv = (mv + 50u) / 100u;
	sb_u32(b, dv / 10u, 1);
	sb_chr(b, '.');
	sb_u32(b, dv % 10u, 1);
	sb_str(b, " V");
}

static void sb_deg(sbuf_t *b, int32_t e7, char pos, char neg)
{
	uint32_t a = (uint32_t)(e7 < 0 ? -e7 : e7);
	sb_u32(b, a / 10000000u, 1);
	sb_chr(b, '.');
	sb_u32(b, (a % 10000000u) / 1000u, 4);
	sb_chr(b, e7 < 0 ? neg : pos);
}

static const char *info_body(void)
{
	static char t[200];
	sbuf_t b;
	sb_init(&b, t, sizeof t);
	switch (ui_frame()->arg) {
	case INFO_POSITION: {
		ui_position_t p;
		if (!ui_svc_position(&p)) {
			sb_str(&b, "No GPS, or it is off (Settings > Location)");
		} else if (!p.fix) {
			sb_str(&b, "No fix yet: ");
			sb_u32(&b, p.sats, 1);
			sb_str(&b, " satellites seen. Go outside.");
		} else {
			sb_str(&b, "Fix, ");
			sb_u32(&b, p.sats, 1);
			sb_str(&b, " satellites\n");
			sb_deg(&b, p.lat_e7, 'N', 'S');
			sb_chr(&b, ' ');
			sb_deg(&b, p.lon_e7, 'E', 'W');
			sb_str(&b, "\nAltitude ");
			sb_i32(&b, p.alt_m);
			sb_str(&b, " m");
		}
		break;
	}
	case INFO_HOTSPOT:
#if CONFIG_HOTSPOT
		sb_str(&b, "Start MMDVMHost or Pi-Star on a computer on USB: hotspot mode starts by itself.");
#else
		sb_str(&b, "Hotspot: not in this build");
#endif
		break;
	case INFO_BATTERY:
		if (board_volts() == BOARD_VOLTS_SUPPLY) {	// a reading only: no charge level or warning
			sb_volts(&b, ui_view()->supply_mv);
			break;
		}
		sb_u32(&b, ui_st.battery_pct, 1);
		sb_str(&b, ui_st.battery_pct <= (ui_st.low_batt_pct ? ui_st.low_batt_pct : 15) ? "%: charge soon" : "%");
		break;
	case INFO_CODEPLUG: {
		static const char *const src[] = { "None: built-in channels", "Built-in channels", "Written by the CPS" };
		codeplug_source_t s = codeplug_source();
		sb_str(&b, (unsigned)s < 3 ? src[s] : "?");
		sb_str(&b, "\nZones: ");
		sb_u32(&b, codeplug_zone_count(), 1);
		sb_str(&b, "\nChannels: up to ");
		sb_u32(&b, codeplug_channel_count(), 1);
		break;
	}
	case INFO_MY_RADIO:
#if CONFIG_DMR
		sb_str(&b, "DMR ID: ");
		if (codeplug_dmr_id())
			sb_u32(&b, codeplug_dmr_id(), 1);
		else
			sb_str(&b, "not set (set it in qdmr)");
		sb_chr(&b, '\n');
#endif
		sb_str(&b, "Radio: ");
		sb_str(&b, board.name);
		break;
	case INFO_USB_NET:
#if USB_NET_UI
		if (hostlink_network_mode())
			sb_str(&b, "USB is a network adapter: IP over DMR. qdmr and the hotspot are off until you switch "
				"back.");
		else
			sb_str(&b, "USB is a serial port: qdmr, hotspot. As a network adapter it carries IP over DMR.");
#else
		sb_str(&b, "USB network: not in this build");
#endif
		break;
	case INFO_SFR: {
#if CONFIG_SFR
		// The channel's SFR role and what it is doing now (docs/spec/sfr.md 4)
		const radio_dmr_view_t *d = &ui_view()->dmr;
		char in = d->sfr_inbound ? '2' : '1', out = d->sfr_inbound ? '1' : '2';

		if (ui_view()->ch.mode != RADIO_MODE_DMR || d->sfr_role == SFR_ROLE_OFF) {
			sb_str(&b, "This channel has no SFR role. Set Repeater or User in the Codeplug Editor, on a channel "
				"with one frequency.");
		} else if (d->sfr_role == SFR_ROLE_RPT_STANDARD || d->sfr_role == SFR_ROLE_RPT_MOTOROLA) {
			sb_str(&b, d->sfr_role == SFR_ROLE_RPT_MOTOROLA ? "Repeater (ERDM style)\nIn TS" : "Repeater\nIn TS");
			sb_chr(&b, in);
			sb_str(&b, ", out TS");
			sb_chr(&b, out);
			sb_str(&b, !d->sfr_rpt ? "\nOff: transmit not allowed now" : d->sfr_cw ? "\nSending its CW ID" : d->sfr_busy ? "\nRepeating a call" :
				d->sfr_keyed ? "\nHang time" : "\nListening");
		} else {
			sb_str(&b, d->sfr_role == SFR_ROLE_USER_ERDM ? "User of an ERDM repeater\nTalk on TS" :
				"User of a repeater\nTalk on TS");
			sb_chr(&b, in);
			sb_str(&b, ", hear TS");
			sb_chr(&b, in);
			sb_str(&b, " and TS");
			sb_chr(&b, out);
		}
#else
		sb_str(&b, "Single-freq repeater: not in this build");
#endif
		break;
	}
	default:
		break;
	}
	return t;
}

static uint16_t info_count(void)
{
#if CONFIG_HOTSPOT
	if (ui_frame()->arg == INFO_HOTSPOT && hostlink_mode() == HOSTLINK_HOTSPOT)
		return 1;
#endif
#if USB_NET_UI
	if (ui_frame()->arg == INFO_USB_NET)
		return 1;
#endif
	return 0;
}

static void info_item(uint16_t i, ui_item_t *it)
{
#if USB_NET_UI
	if (ui_frame()->arg == INFO_USB_NET) {
		strcpy(it->label, hostlink_network_mode() ? "Switch USB to serial" : "Switch USB to network");
		return;
	}
#endif
	strcpy(it->label, "Stop hotspot");
}

static void info_select(uint16_t i, bool hold)
{
	(void)hold;
#if USB_NET_UI
	if (ui_frame()->arg == INFO_USB_NET) {
		bool net = !hostlink_network_mode();
		if (!hostlink_request_usb_network(net)) {
			ui_toast("No USB network on this radio");
		} else {
			ui_go_home();
			ui_toast(net ? "USB is a network adapter now" : "USB is a serial port again");
		}
		return;
	}
#endif
#if CONFIG_HOTSPOT
	if (ui_frame()->arg == INFO_HOTSPOT) {
		hostlink_hotspot_exit();
		ui_toast("Hotspot stopped");
	}
#endif
}

static const ui_scr_def_t info_screen = {
	.name = "info", .flags = SF_LIST | SF_FIELD, .hint = "Back", .count = info_count, .item = info_item,
	.select = info_select, .title = info_title, .body = info_body,
};

#if CONFIG_DMR
// ======================= DMR ID (Settings > My radio) =======================
// One row: the radio's own DMR ID, the codeplug's (a CPS read shows it). Digits type a new one, OK writes it into the
// codeplug. Typed as the channel editor's Own ID (ui.md 2.1): a digit past CODEPLUG_DMR_ID_MAX is refused, BACK takes
// the last digit back and with none typed leaves the ID unchanged

static uint32_t dmr_id_value;
static uint8_t dmr_id_len;		// digits typed
static bool dmr_id_editing;
static uint32_t dmr_id_saved, dmr_id_serial;	// the ID just saved, shown until the codeplug's serial moves on

static uint16_t dmr_id_count(void) { return 1; }

static void dmr_id_item(uint16_t i, ui_item_t *it)
{
	(void)i;
	strcpy(it->label, "DMR ID");
	uint32_t v = dmr_id_editing ? dmr_id_value : dmr_id_saved != 0 && codeplug_serial() == dmr_id_serial ?
		     dmr_id_saved : (uint32_t)ui_svc_setting_get(UI_SET_DMR_ID);
	sbuf_t b;
	sb_init(&b, it->value, sizeof it->value);
	if (dmr_id_editing) {
		if (dmr_id_len > 0)
			sb_u32(&b, v, 1);
		sb_chr(&b, '_');		// what is typed so far, as the channel editor: "3101_"
		it->flags |= UI_ITEM_EDIT;
	} else if (v != 0) {
		sb_u32(&b, v, 1);
	} else {
		sb_str(&b, "Not set");	// DMR transmit is refused until there is one
	}
}

static const char *dmr_id_body(void)
{
	static char t[48];
	sbuf_t b;
	sb_init(&b, t, sizeof t);
	sb_str(&b, "Saved in the codeplug\nRadio: ");
	sb_str(&b, board.name);		// as the info page it replaces
	return t;
}

static void dmr_id_begin(void)
{
	dmr_id_editing = true;
	dmr_id_len = 0;
	dmr_id_value = 0;
}

static void dmr_id_save(void)
{
	if (dmr_id_len == 0) {
		dmr_id_editing = false;	// nothing typed: as it was
		return;
	}
	if (dmr_id_value == 0) {
		ui_toast("IDs start at 1");	// 0 would leave the codeplug without one
		return;
	}
	dmr_id_editing = false;
	uint32_t serial = codeplug_serial();
	if (!ui_svc_setting_set(UI_SET_DMR_ID, (int32_t)dmr_id_value)) {
		ui_toast("Couldn't save: try again");
		return;
	}
	dmr_id_saved = dmr_id_value;
	dmr_id_serial = serial;
	ui_toast("DMR ID saved");
}

static void dmr_id_select(uint16_t i, bool hold)
{
	(void)i;
	(void)hold;
	if (dmr_id_editing)
		dmr_id_save();
	else
		dmr_id_begin();
}

static bool dmr_id_key(const input_event_t *ev)
{
	bool press = ev->gesture == GESTURE_PRESS || ev->gesture == GESTURE_LONG;
	if (ev->action >= ACT_DIGIT_0 && ev->action <= ACT_DIGIT_9 && press) {
		if (!dmr_id_editing)
			dmr_id_begin();
		uint32_t v = dmr_id_value * 10u + (uint32_t)(ev->action - ACT_DIGIT_0);
		if (v > CODEPLUG_DMR_ID_MAX) {
			ui_toast("IDs go up to 16776415");	// the IDs above are the network's
			return true;
		}
		dmr_id_value = v;
		dmr_id_len++;
		ui_dirty();
		return true;
	}
	if (!dmr_id_editing)
		return false;
	if (ev->action == ACT_BACK && ev->gesture == GESTURE_PRESS) {
		if (dmr_id_len > 0) {
			dmr_id_len--;
			dmr_id_value /= 10u;	// takes the last digit back
		} else {
			dmr_id_editing = false;
		}
		ui_dirty();
		return true;
	}
	return false;
}

static void dmr_id_enter(void)
{
	dmr_id_editing = false;
	dmr_id_saved = 0;
}

static const ui_scr_def_t dmr_id_screen = {
	.name = "dmr-id", .flags = SF_LIST | SF_FIELD, .prefix = "My Radio", .hint = "0-9 type  OK save  Back",
	.count = dmr_id_count, .item = dmr_id_item, .select = dmr_id_select, .key = dmr_id_key, .enter = dmr_id_enter,
	.body = dmr_id_body,
};
#endif

// ======================= About (Settings > Advanced) =======================
// A text page, every word shown (nothing shortened), no status strip while idle: the release and the commit, RadioFoundry's license, the
// authors, the third-party code and fonts in this image and the codec. From AUTHORS and platform/<platform>/THIRD_PARTY,
// through the header make generates (tools/gen_about.py)

static const char *const about_authors[] = RFOUNDRY_ABOUT_AUTHORS;
static const char *const about_third[][2] = RFOUNDRY_ABOUT_THIRD_PARTY;
static uint16_t about_first;	// first line shown
static uint16_t about_max;	// the largest first line (from the last draw)

static const char *about_text(void)
{
	static char t[400];
	sbuf_t b;
	sb_init(&b, t, sizeof t);
	sb_str(&b, "RadioFoundry ");
	// a release: its name, then the commit it was built from
	if (RFOUNDRY_RELEASE[0] != '\0') {
		sb_str(&b, RFOUNDRY_RELEASE);
		sb_str(&b, "\nBuild ");
	}
	sb_str(&b, RFOUNDRY_GIT);
	sb_str(&b, "\nRadio: ");
	sb_str(&b, board.name);
	sb_str(&b, "\nMIT License, (c) 2026 The RadioFoundry Authors\n");
	// one author per line, in AUTHORS order: a comma-joined list wraps mid-name on every display
	sb_str(&b, RFOUNDRY_ABOUT_AUTHOR_COUNT > 1 ? "Authors:" : "Author:");
	for (unsigned i = 0; i < RFOUNDRY_ABOUT_AUTHOR_COUNT; i++) {
		sb_chr(&b, RFOUNDRY_ABOUT_AUTHOR_COUNT > 1 ? '\n' : ' ');
		sb_str(&b, about_authors[i]);
	}
	for (unsigned i = 0; i < RFOUNDRY_ABOUT_THIRD_PARTY_COUNT; i++) {
		sb_chr(&b, '\n');
		sb_str(&b, about_third[i][0]);
		sb_str(&b, ": ");
		sb_str(&b, about_third[i][1]);
	}
	sb_str(&b, "\nSpleen font: BSD-2-Clause");	// core/ui/fonts.c
#if CONFIG_FONT_AA
	sb_str(&b, "\nJetBrains Mono font: OFL-1.1");
#endif
	sb_str(&b, "\nAMBE+2 codec: DVSI (stock)");	// from the radio's own firmware, not part of RadioFoundry
	return t;
}

static void about_enter(void)
{
	about_first = 0;
}

static void about_draw(void)
{
	about_max = (uint16_t)ui_draw_text_page(ui_view(), &ui_st, "About", about_text(), &about_first);
}

// Up / Down and the knob scroll a line (the knob's forward = down, as in lists); BACK leaves
static bool about_key(const input_event_t *ev)
{
	int dir = ev->action == ACT_NAV_UP ? -1 : ev->action == ACT_NAV_DOWN ? 1 :
		ev->action == ACT_ENC_STEP ? (ev->value > 0 ? 1 : -1) : 0;
	if (dir == 0 || (ev->gesture != GESTURE_PRESS && ev->gesture != GESTURE_REPEAT && ev->gesture != GESTURE_LONG &&
			ev->action != ACT_ENC_STEP))
		return false;
	int n = ev->action == ACT_ENC_STEP ? (ev->value < 0 ? -ev->value : ev->value) : 1;
	int f = about_first + dir * n;
	about_first = (uint16_t)(f < 0 ? 0 : f > about_max ? about_max : f);
	ui_dirty();
	return true;
}

static const ui_scr_def_t about_screen = {
	.name = "about", .flags = SF_FIELD | SF_NO_STRIP, .key = about_key, .draw = about_draw, .enter = about_enter,
};

// ======================= Roaming (Tools > Roaming, docs/spec/dmr-roaming.md 6.2) =======================

enum { RM_ZONE, RM_NOW, RM_LOCK, RM_SURVEY, RM_BEACON, RM_LEN, RM_PROBE, RM_COUNT };

#if CONFIG_DMR_ROAMING
// The survey and beacon rows: radio-wide settings, cycled with OK (the values must match the repeaters)
static const int16_t rm_survey_v[] = { 1, 2, 0 };
static const char *const rm_survey_n[] = { "Synced", "Per site", "Off" };
static const int16_t rm_beacon_v[] = { 60, 120, 300, 600, 0, 10, 15, 20, 30 };
static const char *const rm_beacon_n[] = { "60 s", "2 min", "5 min", "10 min", "None", "10 s", "15 s", "20 s", "30 s" };
static const int16_t rm_len_v[] = { 4, 5, 6, 8, 10, 1, 2, 3 };
static const char *const rm_len_n[] = { "4 s", "5 s", "6 s", "8 s", "10 s", "1 s", "2 s", "3 s" };

typedef struct {
	uint8_t id;		// ui_setting_t
	const int16_t *v;
	const char *const *n;
	uint8_t count;
} rm_list_t;

static const rm_list_t rm_lists[] = {
	{ UI_SET_ROAM_SURVEY, rm_survey_v, rm_survey_n, 3 },
	{ UI_SET_ROAM_BEACON_S, rm_beacon_v, rm_beacon_n, 9 },
	{ UI_SET_ROAM_BEACON_LEN, rm_len_v, rm_len_n, 8 },
};

static uint8_t rm_find(const rm_list_t *l, int32_t v)
{
	for (uint8_t k = 0; k < l->count; k++)
		if (l->v[k] == v)
			return k;
	return 0xFF;
}
#endif

#if CONFIG_DMR_ROAMING
static bool roam_active(const radio_view_t *v) { return v->roam.state != RROAM_STATE_OFF; }

// Why roaming isn't working on this channel, NULL when it is
static const char *roam_why_not(const radio_view_t *v)
{
	if (v->vfo)
		return "Roaming works on channels, not the VFO.";
	if (!v->roam.zone_on)
		return "Off in this zone. On, the radio moves between the zone's repeaters.";
	if (roam_active(v))
		return NULL;
	if (v->ch.mode != RADIO_MODE_DMR || v->ch.dmo || v->ch.rx_only || v->ch.tx_hz == v->ch.rx_hz)
		return "This channel is not a repeater site.";
	return "Needs two sites in the zone with the same talkgroup.";
}
#endif

static const char *roam_body(void)
{
	static char t[120];
	sbuf_t b;
	sb_init(&b, t, sizeof t);
#if CONFIG_DMR_ROAMING
	const radio_view_t *v = ui_view();
	const char *why = roam_why_not(v);
	if (why != NULL) {
		sb_str(&b, why);
		return t;
	}
	if (v->roam.state == RROAM_STATE_SEARCH) {
		sb_str(&b, "Searching sites...");
		return t;
	}
	sb_str(&b, "Site ");
	sb_u32(&b, v->roam.site + 1u, 1);
	sb_str(&b, " of ");
	sb_u32(&b, v->roam.sites, 1);
	sb_str(&b, ": ");
	sb_str(&b, v->ch.name);
	if (v->roam.rssi_dbm != 0) {
		sb_str(&b, "\nSignal ");
		sb_i32(&b, v->roam.rssi_dbm);
		sb_str(&b, v->roam.weak ? " dBm, weak" : " dBm");
	}
	if (v->roam.state == RROAM_STATE_LOCKED)
		sb_str(&b, "\nLocked to this site");
	else if (ui_svc_setting_get(UI_SET_ROAM_SURVEY) == 1 && ui_svc_setting_get(UI_SET_ROAM_BEACON_S) != 0)
		sb_str(&b, v->roam.win_conf >= 2 ? "\nBeacons: surveying" : "\nBeacons: learning");
#else
	sb_str(&b, "Roaming: not in this build");
#endif
	return t;
}

static uint16_t roam_count(void) { return RM_COUNT; }

static void roam_item(uint16_t i, ui_item_t *it)
{
	static const char *const names[] = { "Roam in this zone", "Roam now", "Lock to this site", "Site survey",
					     "Beacon interval", "Beacon length", "Wake probe" };
	strcpy(it->label, names[i]);
	item_num(it, (uint16_t)(i + 1u));
#if CONFIG_DMR_ROAMING
	const radio_view_t *v = ui_view();
	if (i >= RM_SURVEY && i <= RM_LEN) {
		const rm_list_t *l = &rm_lists[i - RM_SURVEY];
		int32_t val = ui_svc_setting_get(l->id);
		uint8_t k = rm_find(l, val);
		sbuf_t b;
		sb_init(&b, it->value, sizeof it->value);
		if (k != 0xFF) {
			sb_str(&b, l->n[k]);
		} else {
			sb_i32(&b, val);	// a value set over the debug link
			sb_str(&b, " s");
		}
		bool no_beacons = ui_svc_setting_get(UI_SET_ROAM_BEACON_S) == 0;
		if ((i == RM_LEN && no_beacons) || (i != RM_SURVEY && ui_svc_setting_get(UI_SET_ROAM_SURVEY) == 0))
			it->flags |= UI_ITEM_DIM;	// nothing uses it now
		return;
	}
	if (i == RM_PROBE) {
		strcpy(it->value, ui_svc_setting_get(UI_SET_ROAM_PROBE) ? "On" : "Off");
		return;
	}
	if (i == RM_ZONE)
		strcpy(it->value, v->roam.zone_on ? "On" : "Off");
	else if (i == RM_LOCK)
		strcpy(it->value, v->roam.state == RROAM_STATE_LOCKED ? "On" : "Off");
	if (i == RM_ZONE ? v->vfo : !roam_active(v))
		it->flags |= UI_ITEM_DIM;
#else
	it->flags |= UI_ITEM_DIM;
#endif
}

static void roam_select(uint16_t i, bool hold)
{
	(void)hold;
#if CONFIG_DMR_ROAMING
	const radio_view_t *v = ui_view();
	static char msg[20 + CODEPLUG_NAME_LEN];	// the toast keeps the pointer
	sbuf_t b;
	sb_init(&b, msg, sizeof msg);
	const char *why = roam_why_not(v);
	if (i >= RM_SURVEY && i <= RM_LEN) {
		// Radio-wide: they apply in every roaming zone, roaming on here or not
		const rm_list_t *l = &rm_lists[i - RM_SURVEY];
		uint8_t k = rm_find(l, ui_svc_setting_get(l->id));
		int32_t nv = l->v[k == 0xFF ? 0 : (k + 1u) % l->count];
		if (ui_svc_setting_set(l->id, nv))
			ui_svc_apply(l->id, nv);
		if (i == RM_BEACON || i == RM_LEN)
			ui_toast("Must match the repeaters");
		return;
	}
	if (i == RM_PROBE) {
		bool on = !ui_svc_setting_get(UI_SET_ROAM_PROBE);
		if (ui_svc_setting_set(UI_SET_ROAM_PROBE, on))
			ui_svc_apply(UI_SET_ROAM_PROBE, on);
		ui_toast(on ? "Wake probe on: transmits on silent sites" : "Wake probe off: listening only");
		return;
	}
	if (i == RM_ZONE) {
		if (v->vfo) {
			ui_toast("Roaming works on channels");
			return;
		}
		bool on = !v->roam.zone_on;
		ui_svc_roam(RROAM_ZONE_TOGGLE);
		if (on) {
			sb_str(&b, "Roaming on for ");
			sb_str(&b, v->zone_name);
			ui_toast(msg);
		} else {
			ui_toast("Roaming off");
		}
		return;
	}
	if (why != NULL) {
		ui_toast(!v->roam.zone_on ? "Turn roaming on first" : "Can't roam on this channel");
		return;
	}
	if (i == RM_NOW) {
		if (v->roam.state == RROAM_STATE_SEARCH) {
			ui_toast("Already searching");
			return;
		}
		ui_svc_roam(RROAM_NOW);
		ui_go_home();
		ui_toast("Searching sites");
	} else if (i == RM_LOCK) {
		bool lock = v->roam.state != RROAM_STATE_LOCKED;
		ui_svc_roam(lock ? RROAM_LOCK : RROAM_UNLOCK);
		if (lock) {
			sb_str(&b, "Locked to ");
			sb_str(&b, v->ch.name);
			ui_toast(msg);
		} else {
			ui_toast("Roaming again");
		}
	}
#else
	(void)i;
	ui_toast("Roaming: not in this build");
#endif
}

static const ui_scr_def_t roam_screen = {
	.name = "roaming", .flags = SF_LIST | SF_NUMBERED | SF_FIELD, .prefix = "Roaming", .hint = "1-7 pick  Back",
	.count = roam_count, .item = roam_item, .select = roam_select, .body = roam_body,
};

// ======================= Crash log (Settings > Advanced, docs/spec/crash-log.md) =======================
// arg 0: from the menu (the record, the report's settings); arg 1: the prompt after the restart ("After a crash:
// Ask"). Only with the crash log; the report's rows only with the crash report

#if CONFIG_CRASH_SCREEN
enum { CR_SEND, CR_TO, CR_MODE, CR_CLEAR, CR_LATER };

static bool crash_prompt(void) { return ui_frame()->arg == 1; }

static uint8_t crash_rows(uint8_t *out)
{
	uint8_t n = 0;
	if (CONFIG_CRASH_REPORT)
		out[n++] = CR_SEND;
	if (crash_prompt()) {
		out[n++] = CR_LATER;
		return n;
	}
	if (CONFIG_CRASH_REPORT) {
		out[n++] = CR_TO;
		out[n++] = CR_MODE;
	}
	out[n++] = CR_CLEAR;
	return n;
}

static uint16_t crash_count(void)
{
	uint8_t r[5];
	return crash_rows(r);
}

// The target contact's name (or "TG 9" / its ID without one); "" when none is set or it is gone from the codeplug
static void crash_target(char *out, size_t len)
{
	sbuf_t b;
	sb_init(&b, out, len);
#if CONFIG_CRASH_REPORT
	contact_t c;
	int32_t num = ui_svc_setting_get(UI_SET_CRASH_CONTACT);
	if (num <= 0 || !codeplug_contact((uint16_t)num, &c))
		return;
	if (c.name[0]) {
		sb_str(&b, c.name);
		return;
	}
	if (c.type != CONTACT_PRIVATE)
		sb_str(&b, "TG ");
	sb_u32(&b, c.id, 1);
#endif
}

static const char *const crash_modes[] = { "Off", "Ask", "Auto" };	// UI_CRASH_SEND_*

static const char *crash_body(void)
{
	static char t[160];
	sbuf_t b;
	sb_init(&b, t, sizeof t);
	ui_crash_t c;
	if (!ui_svc_crash_get(&c)) {
		sb_str(&b, "No crash recorded.");
		return t;
	}
	if (crash_prompt()) {
		// The question only: it has to fit above the two rows on the smallest screens
		char to[CODEPLUG_NAME_LEN + 4];
		crash_target(to, sizeof to);
		sb_str(&b, "Restarted after a crash. Send the report to ");
		sb_str(&b, to);
		sb_str(&b, "?");
		return t;
	}
	sb_str(&b, c.summary);
	if (c.sent)
		sb_str(&b, "\nReport sent");
	if (c.count > 1) {
		sb_str(&b, "\n");
		sb_u32(&b, c.count, 1);
		sb_str(&b, " crashes in a row");
	}
	sb_str(&b, "\nRan for ");
	sb_mmss(&b, c.uptime_ms / 1000u);
	if (c.build != 0) {
		sb_str(&b, ", build ");
		for (int k = 28; k >= 0; k -= 4)
			sb_chr(&b, "0123456789abcdef"[(c.build >> k) & 0xFu]);
	}
	return t;
}

static void crash_item(uint16_t i, ui_item_t *it)
{
	uint8_t r[5];
	uint8_t n = crash_rows(r);
	if (i >= n)
		return;
	ui_crash_t c;
	bool have = ui_svc_crash_get(&c);
	item_num(it, (uint16_t)(i + 1u));
	switch (r[i]) {
	case CR_SEND:
		strcpy(it->label, "Send report");
		if (!have)
			it->flags |= UI_ITEM_DIM;
		break;
	case CR_TO:
		strcpy(it->label, "Send to");
		crash_target(it->value, sizeof it->value);
		if (it->value[0] == 0)
			strcpy(it->value, "Not set");
		break;
	case CR_MODE: {
		int32_t m = ui_svc_setting_get(UI_SET_CRASH_MODE);
		strcpy(it->label, "After a crash");
		strcpy(it->value, crash_modes[m >= 0 && m <= UI_CRASH_SEND_AUTO ? m : 0]);
		break;
	}
	case CR_CLEAR:
		strcpy(it->label, "Clear");
		if (!have)
			it->flags |= UI_ITEM_DIM;
		break;
	default:
		strcpy(it->label, "Not now");
		break;
	}
}

static bool crash_watch;	// a report of ours is going: say how it ended

static void crash_select(uint16_t i, bool hold)
{
	(void)hold;
	uint8_t r[5];
	uint8_t n = crash_rows(r);
	if (i >= n)
		return;
	ui_crash_t c;
	bool have = ui_svc_crash_get(&c);
	char to[CODEPLUG_NAME_LEN + 4];
	switch (r[i]) {
	case CR_SEND:
		crash_target(to, sizeof to);
		if (!have) {
			ui_toast("No crash recorded");
		} else if (to[0] == 0) {
			ui_toast("Pick Send to first");
		} else if (ui_svc_crash_send()) {
			crash_watch = true;
			ui_toast("Sending crash report");
			if (crash_prompt())
				ui_pop();
		} else {
			ui_toast("Can't send now: try again");
		}
		break;
	case CR_TO:
		if (CONFIG_DMR)
			ui_push(SCR_CRASH_TO, 0);
		break;
	case CR_MODE: {
		int32_t m = ui_svc_setting_get(UI_SET_CRASH_MODE);
		int32_t nm = m >= UI_CRASH_SEND_OFF && m < UI_CRASH_SEND_AUTO ? m + 1 : UI_CRASH_SEND_OFF;
		if (!ui_svc_setting_set(UI_SET_CRASH_MODE, nm))
			ui_toast("Couldn't save: try again");
		else if (nm != UI_CRASH_SEND_OFF && ui_svc_setting_get(UI_SET_CRASH_CONTACT) <= 0)
			ui_toast("Pick Send to as well");
		break;
	}
	case CR_CLEAR:
		if (have) {
			ui_svc_crash_clear();
			ui_toast("Crash record cleared");
		}
		break;
	default:
		ui_pop();
		break;
	}
}

static void crash_enter(void)
{
	ui_svc_crash_seen();	// shown: no prompt for it again, and the next crash counts from 1
}

static const ui_scr_def_t crash_screen = {
	.name = "crash", .flags = SF_LIST | SF_NUMBERED, .prefix = "Crash Log", .hint = "OK pick  Back",
	.count = crash_count, .item = crash_item, .select = crash_select, .body = crash_body, .enter = crash_enter,
};

#if CONFIG_CRASH_REPORT && CONFIG_DMR
// Send to: the codeplug's contacts, private or group (type to find, as Contacts); OK keeps the one picked
static void crash_to_select(uint16_t i, bool hold)
{
	(void)hold;
	ui_target_t t;
	if (!contacts_target(i, &t))
		return;
	if (!ui_svc_setting_set(UI_SET_CRASH_CONTACT, (int32_t)ui_filter_raw(i) + 1)) {
		ui_toast("Couldn't save: try again");
		return;
	}
	static char msg[24 + CODEPLUG_NAME_LEN];	// the toast keeps the pointer
	sbuf_t b;
	sb_init(&b, msg, sizeof msg);
	sb_str(&b, "Crash reports to ");
	sb_str(&b, t.name[0] ? t.name : "this contact");
	ui_pop();
	ui_toast(msg);
}

static const ui_scr_def_t crash_to_screen = {
	.name = "crash_to", .flags = SF_LIST | SF_FILTER, .prefix = "Send To",
	.hint = "0-9 find  OK pick  Back", .empty = "No contacts: add them in qdmr",
	.count = ui_filter_count, .item = contacts_item, .select = crash_to_select,
	.raw_count = contacts_raw_count, .raw_label = contacts_raw_label,
};
#endif

// After the restart (ui_app_tick): once the Home screen has been up a moment, a record nobody has seen is asked about
// (Ask) or sent (Auto); Off leaves it to the menu. Then the report's end, once, as a toast
#define CRASH_BOOT_MS	1500u

static uint32_t crash_since;
static bool crash_checked;

void ui_crash_reset(void)
{
	crash_since = 0;
	crash_checked = false;
	crash_watch = false;
}

void ui_crash_tick(uint32_t now_ms)
{
	if (!crash_checked) {
		if (crash_since == 0)
			crash_since = now_ms != 0 ? now_ms : 1;
		if (now_ms - crash_since < CRASH_BOOT_MS || ui_depth() != 0)
			return;
		crash_checked = true;
		ui_crash_t c;
		char to[CODEPLUG_NAME_LEN + 4];
		int32_t mode = CONFIG_CRASH_REPORT ? ui_svc_setting_get(UI_SET_CRASH_MODE) : UI_CRASH_SEND_OFF;
		if (!ui_svc_crash_get(&c) || c.seen || c.sent || mode == UI_CRASH_SEND_OFF)
			return;
		crash_target(to, sizeof to);
		if (to[0] == 0)
			return;		// nowhere to send it: the menu shows it
		if (mode == UI_CRASH_SEND_ASK) {
			ui_push(SCR_CRASH, 1);
		} else if (ui_svc_crash_send()) {
			ui_svc_crash_seen();
			crash_watch = true;
			ui_toast("Sending crash report");
		}
		return;
	}
	if (!crash_watch)
		return;
	uint8_t st = ui_svc_crash_tx_state();
	if (st == UI_CRASH_TX_OK || st == UI_CRASH_TX_FAILED) {
		crash_watch = false;
		ui_toast(st == UI_CRASH_TX_OK ? "Crash report sent" : "Crash report not sent");
	}
}
#endif

// ======================= Tools =======================

// Every tool keeps its number on every radio and build, so a key path (OK 6 7: Diagnostics; tests/hil/test_ui.py and the
// user guide) is the same everywhere. The one a radio may not have (Battery) is last, so no radio has a gap; a build
// without diagnostics skips 7
enum { T_SCAN, T_POSITION, T_HOTSPOT, T_USB_NET, T_SFR, T_ROAM, T_DIAG, T_APRS, T_BATTERY, T_ZONE_OFFER, T_COUNT };

// A tool this build or radio doesn't have is left out of the list (Battery, Supply on a radio that measures its DC
// input instead, is left out where the radio measures neither)
static bool tool_shown(uint16_t i)
{
#if CONFIG_ZONE_SHARE
	if (i == T_ZONE_OFFER)
		return offer_waiting();	// 0 on the keypad: only while another radio's zone waits
#endif
	return (i != T_DIAG || CONFIG_DIAG) && (i != T_BATTERY || board_volts() != BOARD_VOLTS_NONE) &&
	       i != T_ZONE_OFFER;
}

// The hint names the last number ("1-9 open"): the highest tool this radio has
static char tools_hint[] = "1-9 open  Back";

static uint16_t tools_count(void)
{
	uint16_t n = 0, last = 0;
	for (uint16_t i = 0; i < T_COUNT; i++) {
		if (tool_shown(i)) {
			n++;
			last = (uint16_t)(i + 1u);
		}
	}
	tools_hint[2] = (char)('0' + (last < 9 ? last : 9));	// "1-9": the zone offer's 0 isn't named
	return n;
}

// The tool on list row `row`
static uint16_t tools_row(uint16_t row)
{
	for (uint16_t i = 0; i < T_COUNT; i++) {
		if (tool_shown(i) && row-- == 0)
			return i;
	}
	return T_COUNT;
}

static void tools_item(uint16_t row, ui_item_t *it)
{
	static const char *const names[] = { "Scan", "Position", "Hotspot", "USB network", "Single-freq repeater",
					     "Roaming", "Diagnostics", "APRS", "Battery", "Zones/Chs Received" };
	uint16_t i = tools_row(row);
	if (i >= T_COUNT)
		return;
	strcpy(it->label, i == T_BATTERY && board_volts() == BOARD_VOLTS_SUPPLY ? "Supply" : names[i]);
	item_num(it, (uint16_t)(i + 1u));	// the tool's own number, not its row (see the enum)
	sbuf_t b;
	sb_init(&b, it->value, sizeof it->value);
	switch (i) {
	case T_SCAN:
#if CONFIG_DUAL_WATCH
		if (ui_st.dual.on) {
			sb_str(&b, "Dual watch");
			break;
		}
#endif
		sb_str(&b, ui_st.scanning ? "On" : "Off");
		if (!(feats() & UI_FEAT_SCAN))
			it->flags |= UI_ITEM_DIM;
		break;
	case T_POSITION:
		if (!(feats() & UI_FEAT_GPS))
			it->flags |= UI_ITEM_DIM;
		break;
	case T_HOTSPOT:
#if CONFIG_HOTSPOT
		sb_str(&b, hostlink_mode() == HOSTLINK_HOTSPOT ? "On" : "Off");
#else
		it->flags |= UI_ITEM_DIM;
#endif
		break;
	case T_USB_NET:
#if USB_NET_UI
		sb_str(&b, hostlink_network_mode() ? "Network" : "Serial");
#else
		it->flags |= UI_ITEM_DIM;
#endif
		break;
	case T_SFR:
		if (!CONFIG_SFR)
			it->flags |= UI_ITEM_DIM;
		break;
	case T_BATTERY:
		if (board_volts() == BOARD_VOLTS_SUPPLY) {
			sb_volts(&b, ui_view()->supply_mv);
			break;
		}
		sb_u32(&b, ui_st.battery_pct, 1);
		sb_chr(&b, '%');
		break;
	case T_ROAM:
#if CONFIG_DMR_ROAMING
		sb_str(&b, ui_view()->roam.zone_on ? "On" : "Off");
#else
		it->flags |= UI_ITEM_DIM;
#endif
		break;
	case T_APRS:
		if (!CONFIG_APRS)
			it->flags |= UI_ITEM_DIM;
		break;
#if CONFIG_ZONE_SHARE
	case T_ZONE_OFFER: {
		ui_zone_offer_t o;
		if (ui_svc_zone_offer(&o)) {
			sb_str(&b, TEXT_ALT_OPEN);	// the name where it fits beside the whole label, else none
			sb_str(&b, o.name);
			sb_str(&b, TEXT_ALT_NEXT TEXT_ALT_CLOSE);
		}
		break;
	}
#endif
	default:
		break;
	}
}

static void tools_select(uint16_t row, bool hold)
{
	(void)hold;
	switch (tools_row(row)) {
	case T_SCAN:
		if (!(feats() & UI_FEAT_SCAN)) {
			ui_toast("Scan: not in this build");
		} else {
			ui_go_home();
			ui_scan_toggle();	// says what scans: the zone, or the band in the VFO (dual watch ends)
		}
		break;
	case T_POSITION: ui_push(SCR_INFO, INFO_POSITION); break;
	case T_HOTSPOT: ui_push(SCR_INFO, INFO_HOTSPOT); break;
	case T_USB_NET: ui_push(SCR_INFO, INFO_USB_NET); break;
	case T_SFR: ui_push(SCR_INFO, INFO_SFR); break;
	case T_BATTERY: ui_push(SCR_INFO, INFO_BATTERY); break;
	case T_DIAG: ui_push(SCR_DIAG, 0); break;
	case T_ROAM: ui_push(SCR_ROAM, 0); break;
	case T_APRS: ui_push(SCR_APRS, 0); break;
	case T_ZONE_OFFER: ui_push(SCR_OFFER, 0); break;
	default: break;
	}
}

static const ui_scr_def_t tools_screen = {
	.name = "tools", .flags = SF_LIST | SF_NUMBERED | SF_FIELD, .prefix = "Tools",
	.hint = tools_hint,
	.count = tools_count, .item = tools_item, .select = tools_select,
};

// ======================= Settings by intent (information-architecture.md 6) =======================
// CONFIG_UI_SETTINGS=n leaves the whole menu out (the settings keep their stored values; qdmr sets them)
#if CONFIG_UI_SETTINGS

typedef enum { ST_TOGGLE, ST_LIST, ST_NUMBER, ST_ACTION } set_type_t;
enum { SA_DIAG, SA_FIRMWARE, SA_CODEPLUG, SA_RESET, SA_MY_RADIO, SA_CRASH };

typedef struct {
	const char *name;
	const char *syn;		// another name Find knows it by
	uint8_t type;			// set_type_t
	uint8_t id;			// ui_setting_t, or SA_* for an action
	int16_t min, max, step;		// ST_NUMBER
	const int16_t *values;		// ST_LIST: the values ...
	const char *const *names;	// ... and their names (NULL: number + unit)
	uint8_t n;
	const char *unit;
} set_item_t;

typedef struct {
	const char *name;
	const char *title;	// the group's page title, in Title Case (the name is its row in Settings)
	const set_item_t *items;
	uint8_t count;
} set_group_t;

static const int16_t v_backlight[] = { 5, 10, 30, 60, 120, 0 };
// Dim after / off after (four times as long): both times shown, so that the dim step isn't taken for the time-out
static const char *const n_backlight[] = { "5s/20s", "10s/40s", "30s/2m", "1m/4m", "2m/8m", "Never" };
#if CONFIG_UI_COLOR
static const int16_t v_theme[] = { 0, 1, 2 };
static const char *const n_theme[] = { "Dark", "Light", "High contrast" };
static const int16_t v_accent[] = { UI_ACCENT_DEFAULT, UI_ACCENT_RED, UI_ACCENT_ORANGE, UI_ACCENT_YELLOW, UI_ACCENT_TEAL,
				     UI_ACCENT_BLUE, UI_ACCENT_PURPLE, UI_ACCENT_PINK };
static const char *const n_accent[] = { "Default", "Red", "Orange", "Yellow", "Teal", "Light blue", "Purple", "Pink" };
#endif
static const int16_t v_tot[] = { 0, 30, 60, 120, 180, 300, 600 };
static const char *const n_tot[] = { "Off", "30 s", "1 min", "2 min", "3 min", "5 min", "10 min" };
static const int16_t v_lock[] = { 0, 30, 60, 300 };
static const char *const n_lock[] = { "Off", "30 s", "1 min", "5 min" };
static const int16_t v_posfmt[] = { 0, 1, 2 };
static const char *const n_posfmt[] = { "Degrees", "Deg min sec", "Grid" };
static const int16_t v_possend[] = { 0, 60, 300, 900 };
static const char *const n_possend[] = { "Off", "1 min", "5 min", "15 min" };
#if CONFIG_KEY_ASSIGN
// Side keys (core/input/keyassign.h, KEYFN_* values): monitor needs the key held, so it is a press only;
// emergency is hold to activate, so a long press only
static const int16_t v_keyfn_press[] = { KEYFN_DEFAULT, KEYFN_NONE, KEYFN_MONITOR, KEYFN_TALK_TARGET, KEYFN_POWER,
					 KEYFN_ZONE_UP, KEYFN_ZONE_DOWN, KEYFN_SCAN, KEYFN_KEY_LOCK,
					 KEYFN_HOME_ZONE,
#if CONFIG_VFO_FM || CONFIG_VFO_DMR
					 KEYFN_COPY_VFO,
#endif
#if CONFIG_DMR
					 KEYFN_CALL_BACK,
#endif
#if CONFIG_DUAL_WATCH
					 KEYFN_AB, KEYFN_DUAL_WATCH,
#endif
};
static const char *const n_keyfn_press[] = { "Default", "Nothing", "Monitor", "Talk target", "Power level",
					     "Next zone", "Previous zone", "Scan", "Key lock", "Home zone",
#if CONFIG_VFO_FM || CONFIG_VFO_DMR
					     "Copy to VFO",
#endif
#if CONFIG_DMR
					     "Call back",
#endif
#if CONFIG_DUAL_WATCH
					     "A/B", "Dual watch",
#endif
};
static const int16_t v_keyfn_hold[] = { KEYFN_DEFAULT, KEYFN_NONE, KEYFN_TALK_TARGET, KEYFN_POWER, KEYFN_ZONE_UP,
					KEYFN_ZONE_DOWN, KEYFN_SCAN,
#if CONFIG_DMR_EMERGENCY
					KEYFN_EMERGENCY,
#endif
					KEYFN_KEY_LOCK, KEYFN_HOME_ZONE,
#if CONFIG_VFO_FM || CONFIG_VFO_DMR
					KEYFN_COPY_VFO,
#endif
#if CONFIG_DMR
					KEYFN_CALL_BACK,
#endif
#if CONFIG_DUAL_WATCH
					KEYFN_AB, KEYFN_DUAL_WATCH,
#endif
};
static const char *const n_keyfn_hold[] = { "Default", "Nothing", "Talk target", "Power level", "Next zone",
					    "Previous zone", "Scan",
#if CONFIG_DMR_EMERGENCY
					    "Emergency",
#endif
					    "Key lock", "Home zone",
#if CONFIG_VFO_FM || CONFIG_VFO_DMR
					    "Copy to VFO",
#endif
#if CONFIG_DMR
					    "Call back",
#endif
#if CONFIG_DUAL_WATCH
					    "A/B", "Dual watch",
#endif
};
#endif
static const int16_t v_updown[] = { 0, 1, 2 };
static const char *const n_updown[] = { "Channel", "Zone", "Talk target" };
#if CONFIG_VFO
static const int16_t v_step[] = { 0, 250, 500, 625, 833, 1000, 1250, 2000, 2500, 5000, 10000 };	// 10 Hz units
static const char *const n_step[] = { "Auto", "2.5 kHz", "5 kHz", "6.25 kHz", "8.33 kHz", "10 kHz", "12.5 kHz",
				      "20 kHz", "25 kHz", "50 kHz", "100 kHz" };
#endif
#if CONFIG_VFO && CONFIG_BCAST_FM
static const int16_t v_bcast_step[] = { 200, 100 };	// kHz
static const char *const n_bcast_step[] = { "200 kHz", "100 kHz" };
#endif
#if CONFIG_DUAL_WATCH
static const int16_t v_dual_wait[] = { 500, 1000, 1500, 2000, 3000, 5000 };	// ms
static const char *const n_dual_wait[] = { "0.5 s", "1 s", "1.5 s", "2 s", "3 s", "5 s" };
#endif
static const int16_t v_autooff[] = { 0, 30, 60, 120 };
static const char *const n_autooff[] = { "Off", "30 min", "1 h", "2 h" };
#if CONFIG_DMR_SMS
static const int16_t v_msgfmt[] = { 2, 1,	// dmr_sms_format_t: ETSI IP text, ETSI short data, Motorola TMS
#if CONFIG_DMR_TMS
				    0,
#endif
};
static const char *const n_msgfmt[] = { "ETSI", "ETSI short", "Motorola" };
#endif
#if CONFIG_ZONE_SHARE
static const int16_t v_zsoffers[] = { 0, 1 };	// ZS_OFFERS_OFF, ZS_OFFERS_ASK
static const char *const n_zsoffers[] = { "Off", "Ask" };
#endif
#if CONFIG_DMR_CONTROL_STATION
static const int16_t v_netlink[] = { 0, 1 };
static const char *const n_netlink[] = { "By DMR ID", "192.168.10.x" };
#endif
#if CONFIG_SFR
static const int16_t v_sfrrx[] = { 0, 1,	// dmr_sfr_rx_t
#if CONFIG_SFR_DUALPATH_RX
				   2,
#endif
};
static const char *const n_sfrrx[] = { "Repeater", "First copy", "Dual-path RX" };
#endif
#if CONFIG_SFR_CWID
static const int16_t v_sfrcwid[] = { 0, 1, 2, 3, 4 };	// radio_cwid_interval_ms codes
static const char *const n_sfrcwid[] = { "Off", "5 min", "10 min", "15 min", "30 min" };
#endif
static const int16_t v_ptype[] = { 0, 1, 2 };	// dmrPrivacyType_t
static const char *const n_ptype[] = { "Off", "Motorola", "Anytone" };

#define TOGGLE(nm, syn_, sid) { nm, syn_, ST_TOGGLE, sid, 0, 1, 1, NULL, NULL, 0, NULL }
#define NUMBER(nm, syn_, sid, lo, hi, st, u) { nm, syn_, ST_NUMBER, sid, lo, hi, st, NULL, NULL, 0, u }
#define LIST(nm, syn_, sid, v, n) { nm, syn_, ST_LIST, sid, 0, 0, 0, v, n, sizeof v / sizeof v[0], NULL }
#define ACTION(nm, syn_, a) { nm, syn_, ST_ACTION, a, 0, 0, 0, NULL, NULL, 0, NULL }

static const set_item_t s_sound[] = {
	NUMBER("Volume", "speaker loudness", UI_SET_VOLUME, 0, RADIO_VOLUME_MAX, 1, NULL),
	TOGGLE("Key tones", "beep", UI_SET_KEY_TONES),
	NUMBER("Tone volume", NULL, UI_SET_TONE_VOLUME, 0, 10, 1, NULL),
	TOGGLE("Talk permit tone", NULL, UI_SET_TALK_PERMIT),
	TOGGLE("Voice prompts", "speech", UI_SET_VOICE_PROMPTS),
	TOGGLE("Message tone", NULL, UI_SET_MESSAGE_TONE),
#if CONFIG_VIBRATE
	TOGGLE("Vibrate", "vibration motor silent", UI_SET_VIBRATE),
#endif
};
static const set_item_t s_display[] = {
	NUMBER("Brightness", "backlight", UI_SET_BRIGHTNESS, 10, 100, 10, "%"),
	LIST("Backlight Dim/Off", "timeout", UI_SET_BACKLIGHT_S, v_backlight, n_backlight),
#if CONFIG_UI_COLOR	// a radio whose only screen is monochrome has no colors to set: no rows for them
	LIST("Theme", "contrast", UI_SET_THEME, v_theme, n_theme),
	LIST("Highlight color", "accent", UI_SET_ACCENT, v_accent, n_accent),
	TOGGLE("Filled status bar", "top line", UI_SET_STRIP),
#endif
	TOGGLE("Large text", "accessibility", UI_SET_LARGE_TEXT),
	TOGGLE("Hint row", "help", UI_SET_HINTS),
};
static const set_item_t s_talk[] = {
	NUMBER("Reply window", NULL, UI_SET_REPLY_S, 0, 30, 1, " s"),
	LIST("TX timeout", "TOT", UI_SET_TOT_S, v_tot, n_tot),
	NUMBER("Default squelch", NULL, UI_SET_SQUELCH, 0, 9, 1, NULL),
#if CONFIG_VFO
	LIST("VFO step", "frequency tuning channel spacing", UI_SET_VFO_STEP, v_step, n_step),
#endif
#if CONFIG_VFO && CONFIG_BCAST_FM
	LIST("FM broadcast step", "VFO tuning radio station 200 kHz odd", UI_SET_BCAST_STEP, v_bcast_step, n_bcast_step),
#endif
	NUMBER("Mic gain", NULL, UI_SET_MIC_GAIN, 0, 10, 1, NULL),
	NUMBER("Freq trim", "frequency offset reference oscillator calibration", UI_SET_REF_TRIM, -REF_TRIM_MAX_HZ,
	       REF_TRIM_MAX_HZ, REF_TRIM_STEP_HZ, " Hz"),
	TOGGLE("Mic AGC", "automatic gain level", UI_SET_MIC_AGC),
	TOGGLE("Mic noise reduction", "noise suppression background", UI_SET_NOISE_REDUCTION),
	TOGGLE("Monitor by default", "digital monitor promiscuous hear all", UI_SET_HEAR_ALL_DEFAULT),
	TOGGLE("Show talker alias", NULL, UI_SET_ALIAS_SHOW),
#if CONFIG_DUAL_WATCH
	TOGGLE("Dual watch talk back", "reply other channel A B", UI_SET_DUAL_TALK_BACK),
	LIST("Dual watch delay", "resume wait reply A B", UI_SET_DUAL_DELAY_MS, v_dual_wait, n_dual_wait),
#endif
#if CONFIG_SFR
	LIST("SFR receive", "single frequency repeater both copies dual-path rx", UI_SET_SFR_RX, v_sfrrx, n_sfrrx),
#endif
#if CONFIG_SFR_CWID
	LIST("SFR CW ID", "single frequency repeater morse callsign identify", UI_SET_SFR_CWID, v_sfrcwid, n_sfrcwid),
#endif
};
static const set_item_t s_privacy[] = {
	LIST("Key type", "privacy encryption", UI_SET_PRIVACY_TYPE, v_ptype, n_ptype),
	NUMBER("Key", "privacy BP", UI_SET_PRIVACY_KEY, 1, 255, 1, NULL),	// the range follows the type (set_range)
	LIST("Auto lock", "keylock", UI_SET_AUTO_LOCK_S, v_lock, n_lock),
	TOGGLE("PTT when locked", NULL, UI_SET_PTT_LOCKED),
};
static const set_item_t s_location[] = {
	TOGGLE("GPS", NULL, UI_SET_GPS),
	LIST("Position format", NULL, UI_SET_POS_FORMAT, v_posfmt, n_posfmt),
	LIST("Send position", "APRS", UI_SET_POS_SEND_S, v_possend, n_possend),
	TOGGLE("Answer GPS requests", "LRRP position location", UI_SET_POS_REPLY),
};
static const set_item_t s_connections[] = {
	TOGGLE("Hotspot allowed", "MMDVM", UI_SET_HOTSPOT_AUTO),
	TOGGLE("Answer radio checks", NULL, UI_SET_CHECK_REPLY),
	TOGGLE("Answer call alerts", NULL, UI_SET_ALERT_ANSWER),
#if CONFIG_TFTP
	TOGGLE("TFTP codeplug write", "network USB programming", UI_SET_TFTP_WRITE),
#endif
#if CONFIG_DMR_SMS
	LIST("Message format", "SMS text ETSI TMS", UI_SET_MSG_FORMAT, v_msgfmt, n_msgfmt),
#endif
#if CONFIG_ZONE_SHARE
	LIST("Zone offers", "zone sharing receive import", UI_SET_ZONE_OFFERS, v_zsoffers, n_zsoffers),
#endif
#if CONFIG_USB_NETWORK && CONFIG_HOSTLINK
	TOGGLE("USB network at start", "dispatch control station", UI_SET_USB_BOOT_NET),
#endif
#if CONFIG_DMR_CONTROL_STATION
	TOGGLE("PC answers data", "dispatch control station passthrough", UI_SET_PC_ENDPOINT),
	LIST("USB address", "dispatch IP link", UI_SET_USB_FIXED_LINK, v_netlink, n_netlink),
#endif
};
static const set_item_t s_power[] = {
	TOGGLE("Battery saver", NULL, UI_SET_BATTERY_SAVER),
	LIST("Auto power off", NULL, UI_SET_AUTO_OFF_MIN, v_autooff, n_autooff),
	NUMBER("Low battery warning", NULL, UI_SET_LOW_BATTERY, 5, 30, 5, "%"),
};
#if CONFIG_KEY_ASSIGN
// Settings > Keys: a row pair per key the board lets the user reassign, named as on the radio ("SK1", "SK1 hold")
_Static_assert(UI_KEY_ROLES == 9, "KEY_ROWS below");
static char key_label[2 * UI_KEY_ROLES][20];

static void key_labels(void)
{
	sbuf_t b;

	if (key_label[0][0] != 0)
		return;
	for (uint8_t r = 0; r < UI_KEY_ROLES; r++) {
		for (uint8_t h = 0; h < 2; h++) {
			sb_init(&b, key_label[2 * r + h], sizeof key_label[0]);
			if (board.side_key_names != NULL && r < board.side_key_count) {
				sb_str(&b, board.side_key_names[r]);
			} else {
				sb_str(&b, "Key ");
				sb_i32(&b, r + 1);
			}
			if (h)
				sb_str(&b, " hold");
		}
	}
}
#endif
static const set_item_t s_keys[] = {
	NUMBER("Long press time", NULL, UI_SET_LONG_PRESS_MS, 400, 1200, 100, " ms"),
	NUMBER("Key repeat", NULL, UI_SET_REPEAT_MS, 60, 300, 20, " ms"),
	LIST("Up/Down on Home", NULL, UI_SET_UPDOWN_HOME, v_updown, n_updown),
#if CONFIG_KEY_ASSIGN
#define KEY_ROWS(r) \
	LIST(key_label[2 * (r)], "button programmable", UI_SET_KEY_FIRST + 2 * (r), v_keyfn_press, n_keyfn_press), \
	LIST(key_label[2 * (r) + 1], "button programmable long press", UI_SET_KEY_FIRST + 2 * (r) + 1, v_keyfn_hold, \
	     n_keyfn_hold)
	KEY_ROWS(0), KEY_ROWS(1), KEY_ROWS(2), KEY_ROWS(3), KEY_ROWS(4), KEY_ROWS(5), KEY_ROWS(6), KEY_ROWS(7),
	KEY_ROWS(8),
#endif
};
static const set_item_t s_my_radio[] = {
	ACTION("DMR ID and name", "callsign", SA_MY_RADIO),
};
static const set_item_t s_advanced[] = {
	ACTION("About", "firmware version license authors", SA_FIRMWARE),
	ACTION("Codeplug", NULL, SA_CODEPLUG),
	ACTION("Diagnostics", "debug", SA_DIAG),
	ACTION("Factory reset", "erase", SA_RESET),
	// After Factory reset, so its place (and its tests' key presses) stays
#if CONFIG_CRASH_SCREEN
	ACTION("Crash log", "crash report fault debug", SA_CRASH),
#endif
};

#define GROUP(nm, a)		{ nm, nm, a, sizeof a / sizeof a[0] }
#define GROUP_T(nm, t, a)	{ nm, t, a, sizeof a / sizeof a[0] }
static const set_group_t groups[] = {
	GROUP("Sound", s_sound), GROUP("Display", s_display), GROUP_T("Talk & listen", "Talk & Listen", s_talk),
	GROUP("Privacy", s_privacy), GROUP("Location", s_location), GROUP("Connections", s_connections),
	GROUP("Power", s_power), GROUP("Keys", s_keys), GROUP_T("My radio", "My Radio", s_my_radio),
	GROUP("Advanced", s_advanced),
};
#define GROUPS	(sizeof groups / sizeof groups[0])

uint8_t ui_set_group_count(void) { return GROUPS; }
const char *ui_set_group_name(uint8_t g) { return g < GROUPS ? groups[g].name : ""; }

// Settings a radio without a battery (or a broadcast receiver, or the key) doesn't have are left out of the list, not
// shown disabled
static bool set_hidden(const set_item_t *s)
{
#if CONFIG_KEY_ASSIGN
	if (s->type != ST_ACTION && s->id >= UI_SET_KEY_FIRST && s->id <= UI_SET_KEY_LAST) {
		key_labels();
		return (s->id - UI_SET_KEY_FIRST) / 2 >= board.side_key_count;	// a key this radio doesn't have
	}
#endif
	if (s->type != ST_ACTION && s->id == UI_SET_VOLUME)
		return !(feats() & UI_FEAT_VOLUME);	// a knob sets the volume on the other radios
	if (s->type != ST_ACTION && s->id == UI_SET_BCAST_STEP)
		return !(feats() & UI_FEAT_BCAST);
	if (s->type != ST_ACTION && s->id == UI_SET_REF_TRIM)
		return !ref_trim_available();	// only a radio whose board has the trim
	return !board_has_battery() && s->type != ST_ACTION &&
	       (s->id == UI_SET_BATTERY_SAVER || s->id == UI_SET_LOW_BATTERY);
}

// The i-th setting of group g that this radio has, NULL past the end
static const set_item_t *set_nth(uint8_t g, uint16_t i)
{
	if (g >= GROUPS)
		return NULL;
	for (uint8_t k = 0; k < groups[g].count; k++) {
		if (set_hidden(&groups[g].items[k]))
			continue;
		if (i-- == 0)
			return &groups[g].items[k];
	}
	return NULL;
}

uint8_t ui_set_item_count(uint8_t g)
{
	uint8_t n = 0;
	while (set_nth(g, n) != NULL)
		n++;
	return n;
}

const char *ui_set_item_name(uint8_t g, uint8_t i, const char **synonyms)
{
	const set_item_t *s = set_nth(g, i);
	*synonyms = s != NULL ? s->syn : NULL;
	return s != NULL ? s->name : "";
}

void ui_open_setting(uint8_t g, uint8_t i)
{
	ui_push(SCR_SETTINGS, 0);
	ui_frame()->focus = g;
	ui_push(SCR_GROUP, g);
	ui_frame()->focus = i;
}

static uint16_t settings_count(void) { return GROUPS; }

static void settings_item(uint16_t i, ui_item_t *it)
{
	strcpy(it->label, groups[i].name);
	item_num(it, (uint16_t)(i + 1u));
}

static void settings_select(uint16_t i, bool hold)
{
	(void)hold;
	ui_push(SCR_GROUP, i);
}

static const ui_scr_def_t settings_screen = {
	.name = "settings", .flags = SF_LIST | SF_NUMBERED, .prefix = "Settings", .hint = "0-9 open  Back",
	.count = settings_count, .item = settings_item, .select = settings_select,
};

// A group: each row shows its value; toggles and lists change on OK, numbers open an inline editor

static bool set_editing;
static bool set_typed;		// digits typed into the editor since it opened
static int32_t set_value;

static const set_item_t *set_item_at(uint16_t i)
{
	uint16_t g = ui_frame()->arg;
	return g < GROUPS ? set_nth((uint8_t)g, i) : NULL;
}

static void set_value_text(const set_item_t *s, int32_t v, char *out, size_t len)
{
	sbuf_t b;
	sb_init(&b, out, len);
	switch (s->type) {
	case ST_TOGGLE:
		sb_str(&b, v ? "On" : "Off");
		break;
	case ST_LIST:
		for (uint8_t k = 0; k < s->n; k++) {
			if (s->values[k] == v) {
				sb_str(&b, s->names[k]);
				return;
			}
		}
		sb_i32(&b, v);
		break;
	case ST_NUMBER:
		sb_i32(&b, v);
		if (s->unit != NULL)
			sb_str(&b, s->unit);
		break;
	default:
		break;
	}
}

static uint16_t group_count(void) { return ui_set_item_count((uint8_t)ui_frame()->arg); }


// The board's vocoder takes this encode option (hal_vocoder_t.options)
static bool vocoder_has(uint32_t opt)
{
	return board.vocoder != NULL && board.vocoder->options != NULL && (board.vocoder->options() & opt) != 0;
}

// Mic AGC is on and in force: Mic gain doesn't apply (shown as "AGC", dimmed)
static bool mic_agc_on(void)
{
	return vocoder_has(VOCODER_OPT_AGC) && ui_svc_setting_get(UI_SET_MIC_AGC) != 0;
}

// Why a setting can't be used on this radio or in this build (the row is dimmed and OK says it), NULL if it can
static const char *set_why_not(const set_item_t *s)
{
	if (s->type == ST_ACTION)
		return NULL;
	switch (s->id) {
	case UI_SET_VOICE_PROMPTS:
		return "Voice prompts: not in this build";
	case UI_SET_TALK_PERMIT:
		return CONFIG_DMR && CONFIG_BEEP ? NULL : "Talk permit tone: not in this build";
	case UI_SET_MESSAGE_TONE:
		return feats() & UI_FEAT_SMS ? NULL : "Messages: not in this build";
	case UI_SET_VIBRATE:
		return feats() & UI_FEAT_VIBRATE ? NULL : "This radio has no vibration motor";
	case UI_SET_THEME:
	case UI_SET_ACCENT:
	case UI_SET_STRIP:
		return board.display != NULL && board.display->bpp == 1 ? "Colors are for color screens" : NULL;
	case UI_SET_MIC_GAIN:
		if (!mic_gain_available())
			return "Mic gain: not in this build";
		return mic_agc_on() ? "Mic AGC sets the level" : NULL;
	case UI_SET_NOISE_REDUCTION:
		if (!CONFIG_DMR)
			return "Noise reduction: not in this build";
		return vocoder_has(VOCODER_OPT_NOISE_REDUCTION) ? NULL : "Not on this radio";
	case UI_SET_MIC_AGC:
		if (!CONFIG_DMR)
			return "Mic AGC: not in this build";
		return vocoder_has(VOCODER_OPT_AGC) ? NULL : "Not on this radio";
	case UI_SET_HEAR_ALL_DEFAULT:
		return feats() & UI_FEAT_HEAR_ALL ? NULL : "Digital monitor: not in this build";
	case UI_SET_ALIAS_SHOW:
		return CONFIG_DMR_TALKER_ALIAS ? NULL : "Talker alias: not in this build";
	case UI_SET_PRIVACY_TYPE:
	case UI_SET_PRIVACY_KEY:
		if (!(feats() & UI_FEAT_PRIVACY))
			return "Privacy: not in this build";
		return s->id == UI_SET_PRIVACY_KEY && ui_svc_setting_get(UI_SET_PRIVACY_TYPE) == 0 ?
			"Pick a key type first" : NULL;
	case UI_SET_GPS:
	case UI_SET_POS_FORMAT:
	case UI_SET_POS_SEND_S:
		return board_has_gps() ? NULL : "No GPS on this radio";
	default:
		return NULL;
	}
}

// A number setting's range: the privacy key's follows its type (Motorola 1-255, Anytone 0-65535)
static void set_range(const set_item_t *s, int32_t *lo, int32_t *hi)
{
	*lo = s->min;
	*hi = s->max;
	if (s->id == UI_SET_PRIVACY_KEY && ui_svc_setting_get(UI_SET_PRIVACY_TYPE) == 2) {
		*lo = 0;
		*hi = 65535;
	}

}

static void group_item(uint16_t i, ui_item_t *it)
{
	const set_item_t *s = set_item_at(i);
	if (s == NULL)
		return;
	strcpy(it->label, s->name);
	bool edit = set_editing && i == ui_frame()->focus;
	if (s->type != ST_ACTION)
		set_value_text(s, edit ? set_value : ui_svc_setting_get(s->id), it->value, sizeof it->value);
	if (s->id == UI_SET_PRIVACY_KEY && s->type == ST_NUMBER && !edit && ui_svc_setting_get(UI_SET_PRIVACY_TYPE) == 0)
		strcpy(it->value, "-");
	if (s->id == UI_SET_MIC_GAIN && !edit && mic_agc_on())
		strcpy(it->value, "AGC");
	if (edit)
		it->flags |= UI_ITEM_EDIT;
	if (set_why_not(s) != NULL)
		it->flags |= UI_ITEM_DIM;
}

static void set_store(const set_item_t *s, int32_t v)
{
	if (ui_svc_setting_set(s->id, v)) {
		ui_apply_setting(s->id, v);	// the ones the UI acts on itself
		ui_svc_apply(s->id, v);		// and the ones other packages do
	} else {
		ui_toast("Couldn't save: try again");
	}
}

static void group_select(uint16_t i, bool hold)
{
	const set_item_t *s = set_item_at(i);
	if (s == NULL)
		return;
	const char *why = set_why_not(s);
	if (why != NULL) {
		set_editing = false;
		ui_toast(why);
		return;
	}
	int32_t v = ui_svc_setting_get(s->id);
	switch (s->type) {
	case ST_TOGGLE:
		set_store(s, !v);
		break;
	case ST_LIST: {
		uint8_t k = 0;
		while (k < s->n && s->values[k] != v)
			k++;
		set_store(s, s->values[(k + 1u) % s->n]);
		break;
	}
	case ST_NUMBER:
		if (set_editing) {
			set_editing = false;
			set_store(s, set_value);
		} else {
			set_editing = true;
			set_typed = false;
			set_value = v;
		}
		break;
	case ST_ACTION:
		switch (s->id) {
		case SA_DIAG:
			if (CONFIG_DIAG)
				ui_push(SCR_DIAG, 0);
			else
				ui_toast("Diagnostics: not in this build");
			break;
		case SA_FIRMWARE: ui_push(SCR_ABOUT, 0); break;
		case SA_CODEPLUG: ui_push(SCR_INFO, INFO_CODEPLUG); break;
		case SA_MY_RADIO:
#if CONFIG_DMR
			ui_push(SCR_DMR_ID, 0);
#else
			ui_push(SCR_INFO, INFO_MY_RADIO);
#endif
			break;
		case SA_RESET: ui_push(SCR_CONFIRM, 0); break;
#if CONFIG_CRASH_SCREEN
		case SA_CRASH: ui_push(SCR_CRASH, 0); break;
#endif
		default: break;
		}
		break;
	default:
		break;
	}
	if (hold && !set_editing && s->type != ST_ACTION)
		ui_toast("Settings are saved as you change them");
}

static bool group_key(const input_event_t *ev)
{
	if (!set_editing)
		return false;
	const set_item_t *s = set_item_at(ui_frame()->focus);
	if (s == NULL) {
		set_editing = false;
		return false;
	}
	int32_t lo, hi;
	set_range(s, &lo, &hi);
	int dir = ev->action == ACT_NAV_UP ? 1 : ev->action == ACT_NAV_DOWN ? -1 :
		ev->action == ACT_ENC_STEP ? (ev->value > 0 ? 1 : -1) : 0;
	if (dir != 0 && ev->gesture != GESTURE_UP) {
		int32_t v = set_value + dir * s->step;
		set_value = v < lo ? lo : v > hi ? hi : v;
		ui_dirty();
		return true;
	}
	// Digits type the value (a key, a level): each one appended, starting over when the number would be too big
	if (ev->action >= ACT_DIGIT_0 && ev->action <= ACT_DIGIT_9 &&
			(ev->gesture == GESTURE_PRESS || ev->gesture == GESTURE_LONG)) {
		int32_t d = ev->action - ACT_DIGIT_0;
		int32_t v = set_typed ? set_value * 10 + d : d;
		set_value = v > hi ? d : v;
		set_typed = true;
		if (set_value < lo && set_value * 10 > hi)
			set_value = lo;	// can't grow into the range any more
		ui_dirty();
		return true;
	}
	if (ev->action == ACT_BACK && ev->gesture == GESTURE_PRESS) {
		set_editing = false;	// BACK leaves the editor without changing the setting
		ui_dirty();
		return true;
	}
	if (ev->action == ACT_SELECT && (ev->gesture == GESTURE_PRESS || ev->gesture == GESTURE_LONG)) {
		set_editing = false;
		if (set_value < lo || set_value > hi || (s->step > 1 && (set_value - lo) % s->step != 0)) {
			// A typed value outside the range or between the steps: say what fits instead of guessing
			static char msg[40];
			sbuf_t b;
			sb_init(&b, msg, sizeof msg);
			sb_str(&b, "Not changed: ");
			sb_i32(&b, lo);
			sb_str(&b, " to ");
			sb_i32(&b, hi);
			if (s->step > 1) {
				sb_str(&b, ", steps of ");
				sb_i32(&b, s->step);
			}
			ui_toast(msg);
		} else {
			set_store(s, set_value);
		}
		ui_dirty();
		return true;
	}
	return false;
}

static void group_enter(void) { set_editing = false; }

static const char *group_title(void)
{
	uint8_t g = (uint8_t)ui_frame()->arg;
	return g < GROUPS ? groups[g].title : "";
}

static const ui_scr_def_t group_screen = {
	.name = "group", .flags = SF_LIST, .hint = "OK change  Back",
	.count = group_count, .item = group_item, .select = group_select, .key = group_key, .enter = group_enter,
	.title = group_title,
};

// ======================= Confirmation (destructive only, defaults to No; P7) =======================

static uint16_t confirm_count(void) { return 2; }

static void confirm_item(uint16_t i, ui_item_t *it)
{
	strcpy(it->label, i == 0 ? "No, keep settings" : "Yes, reset");
	if (i == 1)
		it->flags |= UI_ITEM_WARN;
}

static void confirm_select(uint16_t i, bool hold)
{
	(void)hold;
	if (i == 0) {
		ui_pop();
	} else if (ui_svc_factory_reset()) {
		ui_go_home();
		ui_toast("Factory settings restored");
	} else {
		ui_toast("Reset: not in this build");
	}
}

static const char *confirm_body(void)
{
	return "Reset all settings to factory defaults? Channels stay. This can't be undone.";
}

static const ui_scr_def_t confirm_screen = {
	.name = "confirm", .flags = SF_LIST, .prefix = "Factory Reset", .hint = "OK choose  Back cancel",
	.count = confirm_count, .item = confirm_item, .select = confirm_select, .body = confirm_body,
};
#else
uint8_t ui_set_group_count(void) { return 0; }
const char *ui_set_group_name(uint8_t g) { (void)g; return ""; }
uint8_t ui_set_item_count(uint8_t g) { (void)g; return 0; }
const char *ui_set_item_name(uint8_t g, uint8_t i, const char **synonyms) { (void)g; (void)i; *synonyms = NULL; return ""; }
void ui_open_setting(uint8_t g, uint8_t i) { (void)g; (void)i; }

static uint16_t settings_count(void) { return 1; }
static void settings_item(uint16_t i, ui_item_t *it)
{
	(void)i;
	strcpy(it->label, "Set them with qdmr");
	it->flags |= UI_ITEM_INFO;
}

static const ui_scr_def_t settings_screen = {
	.name = "settings", .flags = SF_LIST, .prefix = "Settings", .hint = "Back",
	.count = settings_count, .item = settings_item,
};
#define group_screen	settings_screen
#define confirm_screen	settings_screen
#endif

// ======================= Talk target picker (SK2, quick 8) =======================

#define PICK_MAX	12
typedef struct {
	ui_target_t t;
	const char *tag;
} pick_t;
static pick_t picks[PICK_MAX];
static uint8_t n_picks;

#if CONFIG_DMR
static void pick_add(const ui_target_t *t, const char *tag)
{
	if (n_picks >= PICK_MAX - 1 || t->id == 0)
		return;
	for (uint8_t i = 0; i < n_picks; i++)
		if (picks[i].t.id == t->id && picks[i].t.group == t->group)
			return;
	picks[n_picks].t = *t;
	picks[n_picks].tag = tag;
	n_picks++;
}
#endif

static void picker_enter(void)
{
	n_picks = 0;
#if CONFIG_DMR
	const radio_view_t *v = ui_view();
	ui_target_t t;
	contact_t c;
	// The VFO's own target (it has no channel to take one from)
	if (v->vfo && v->dmr.target_id != 0) {
		memset(&t, 0, sizeof t);
		t.id = v->dmr.target_id;
		t.group = !v->dmr.target_private;
		strcpy(t.name, v->dmr.target_name);
		pick_add(&t, "Current");
	}
	// The channel's default
	uint16_t def = codeplug_channel_talk_contact(&v->ch);
	if (def && codeplug_contact(def, &c)) {
		memset(&t, 0, sizeof t);
		t.id = c.id;
		t.group = c.type != CONTACT_PRIVATE;
		strcpy(t.name, c.name);
		pick_add(&t, "Default");
	}
	// Reply to the last heard
	if (v->dmr.last_ms != 0 && v->dmr.last.src != 0) {
		memset(&t, 0, sizeof t);
		if (v->dmr.last.group) {
			t.id = v->dmr.last.dst;
			t.group = true;
			strcpy(t.name, v->dmr.last.dst_name);
		} else {
			t.id = v->dmr.last.src;
			strcpy(t.name, v->dmr.last.src_name);
		}
		pick_add(&t, "Reply");
	}
	// The channel's listen list
	listen_list_t l;
	if (v->ch.listen_list && codeplug_listen_list(v->ch.listen_list, &l)) {
		for (uint8_t i = 0; i < l.count; i++) {
			if (codeplug_contact(l.contacts[i], &c)) {
				memset(&t, 0, sizeof t);
				t.id = c.id;
				t.group = c.type != CONTACT_PRIVATE;
				strcpy(t.name, c.name);
				pick_add(&t, "Listen");
			}
		}
	}
	// Recent talkgroups
	for (uint8_t i = 0; i < ui_recent_count(); i++) {
		const ui_recent_t *r = ui_recent_get(i);
		if (r->group) {
			memset(&t, 0, sizeof t);
			t.id = r->dst;
			t.group = true;
			pick_add(&t, "Recent");
		}
	}
#endif
	// Focus the reply target when there is one (task-flows.md 3: "TG 91 Reply" second)
	for (uint8_t i = 0; i < n_picks; i++)
		if (picks[i].tag[0] == 'R' && picks[i].tag[1] == 'e' && picks[i].tag[2] == 'p')
			ui_frame()->focus = i;
}

static uint16_t picker_count(void) { return (uint16_t)(n_picks + 1u); }

static void picker_item(uint16_t i, ui_item_t *it)
{
	if (i >= n_picks) {
		strcpy(it->label, "Type a number...");
		return;
	}
	ui_sb_target(it->label, sizeof it->label, &picks[i].t);
	sbuf_t b;	// the tag, left out where the target needs the room
	sb_init(&b, it->value, sizeof it->value);
	sb_str(&b, TEXT_ALT_OPEN);
	sb_str(&b, picks[i].tag);
	sb_str(&b, TEXT_ALT_NEXT TEXT_ALT_CLOSE);
}

static bool picker_target(uint16_t i, ui_target_t *t)
{
	if (i >= n_picks)
		return false;
	*t = picks[i].t;
	return true;
}

static void picker_select(uint16_t i, bool hold)
{
	if (i >= n_picks) {
		ui_push(SCR_FIND, 0);	// digits then OK / PTT: talk to the number typed
		ui_find_set("");
		return;
	}
	ui_talk(&picks[i].t, hold);
}

static void picker_context(uint16_t i)
{
	if (i < n_picks)
		ui_open_actions(&picks[i].t);
}

static const char *picker_title(void)
{
	return ui_view()->ch.name;
}

static const ui_scr_def_t picker_screen = {
	.name = "picker", .flags = SF_LIST | SF_FIELD, .prefix = "Talk to",
	.hint = TEXT_ALT_OPEN "PTT once  OK use  Hold OK set" TEXT_ALT_NEXT "OK use  Hold OK set" TEXT_ALT_CLOSE,
	.count = picker_count, .item = picker_item, .select = picker_select, .context = picker_context,
	.target = picker_target, .enter = picker_enter, .title = picker_title,
};

// ======================= Scan: what to do with the channel that stopped it =======================

static uint16_t scan_count(void) { return 3; }

static void scan_item(uint16_t i, ui_item_t *it)
{
	static const char *const names[] = { "Skip channel for now", "Stay here", "Always skip" };
	strcpy(it->label, names[i]);
	item_num(it, (uint16_t)(i + 1u));
	if (i == 2)
		it->flags |= UI_ITEM_DIM;
}

static void scan_select(uint16_t i, bool hold)
{
	(void)hold;
	if (i == 0) {
		ui_go_home();
		ui_toast(ui_svc_scan_skip() ? "Skipped until the scan stops" : "Nothing to skip yet");
	} else if (i == 1) {
		ui_svc_scan(false);
		ui_st.scanning = false;
		ui_go_home();
		ui_toast("Scan stopped here");
	} else {
		ui_toast("Edit scan lists in qdmr");
	}
}

static const ui_scr_def_t scan_screen = {
	.name = "scan", .flags = SF_LIST | SF_NUMBERED | SF_FIELD, .prefix = "Scan", .hint = "1-3 pick  Back",
	.count = scan_count, .item = scan_item, .select = scan_select,
};

// ======================= Screens with their own drawing =======================

static const ui_scr_def_t home_screen = {
	.name = "home", .flags = SF_FIELD, .key = ui_home_key, .draw = ui_home_draw,
};
static const ui_scr_def_t quick_screen = {
	.name = "quick", .flags = SF_FIELD, .key = ui_quick_key, .draw = ui_quick_draw, .enter = ui_quick_enter,
};
static const ui_scr_def_t diag_screen = {
	.name = "diag", .flags = 0, .key = ui_diag_key, .draw = ui_diag_draw,
};
static const ui_scr_def_t volume_screen = {
	.name = "volume", .flags = SF_FIELD, .key = ui_volume_key, .draw = ui_volume_draw,
};
static const ui_scr_def_t emergency_screen = {
	.name = "emergency", .flags = SF_FIELD, .key = ui_emergency_key, .draw = ui_emergency_draw,
};

const ui_scr_def_t *const ui_scr[SCR_COUNT] = {
	[SCR_HOME] = &home_screen, [SCR_QUICK] = &quick_screen, [SCR_HUB] = &hub_screen,
	[SCR_FIND] = &ui_find_screen, [SCR_ACTIONS] = &actions_screen, [SCR_CONTACTS] = &contacts_screen,
	[SCR_MESSAGES] = &messages_screen, [SCR_MESSAGE] = &message_screen, [SCR_COMPOSE] = &compose_screen,
	[SCR_RECENT] = &recent_screen, [SCR_ZONES] = &channels_screen, [SCR_ZONE] = &zone_screen,
	[SCR_TOOLS] = &tools_screen, [SCR_SETTINGS] = &settings_screen, [SCR_GROUP] = &group_screen,
	[SCR_PICKER] = &picker_screen, [SCR_INFO] = &info_screen, [SCR_CONFIRM] = &confirm_screen,
	[SCR_DIAG] = &diag_screen, [SCR_EMERGENCY] = &emergency_screen, [SCR_SCAN] = &scan_screen,
	[SCR_ROAM] = &roam_screen, [SCR_APRS] = &ui_aprs_screen, [SCR_APRS_HEARD] = &ui_aprs_heard_screen,
	[SCR_APRS_POS] = &ui_aprs_pos_screen, [SCR_ABOUT] = &about_screen,
	[SCR_VOLUME] = &volume_screen,
#if CONFIG_DMR
	[SCR_DMR_ID] = &dmr_id_screen,
#endif
#if CONFIG_CHANNEL_LIST
	[SCR_CHANNELS] = &chlist_screen, [SCR_CHANNEL] = &chinfo_screen,
#if CONFIG_CHANNEL_EDIT
	[SCR_TEXT] = &text_screen, [SCR_DISCARD] = &discard_screen,
#if CONFIG_ZONE_EDIT
	[SCR_ZCHANNEL] = &zchan_screen, [SCR_ZPICK] = &zpick_screen, [SCR_ZONEEDIT] = &zopt_screen,
	[SCR_ZONEDEL] = &zdel_screen,
#endif
#if CONFIG_DMR
	[SCR_CHPICK] = &chpick_screen,
#endif
#endif
#endif
#if CONFIG_CRASH_SCREEN
	[SCR_CRASH] = &crash_screen,
#endif
#if CONFIG_CRASH_REPORT && CONFIG_DMR
	[SCR_CRASH_TO] = &crash_to_screen,
#endif
#if CONFIG_ZONE_SHARE && !CONFIG_ZONE_EDIT
	[SCR_SHARE] = &share_screen,
#endif
#if CONFIG_ZONE_SHARE
	[SCR_SHARE_TO] = &share_to_screen, [SCR_OFFER] = &offer_screen,
	[SCR_OFFER_PREVIEW] = &preview_screen, [SCR_OFFER_ZONE] = &offer_zone_screen,
#endif
#if CONFIG_DUAL_WATCH
	[SCR_DUAL] = &ui_dual_screen, [SCR_DUAL_PICK] = &ui_dual_pick_screen,
	[SCR_DUAL_ZONE] = &ui_dual_zone_screen,
#endif
};
