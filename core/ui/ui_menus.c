// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
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
#include "input/keyassign.h"
#if CONFIG_HOTSPOT || CONFIG_USB_NETWORK
#include "hostlink/hostlink.h"
#endif
#include "odr_about.h"

#ifndef ODR_GIT
#define ODR_GIT "host"
#endif

extern const ui_scr_def_t ui_find_screen;
extern const ui_scr_def_t ui_aprs_screen, ui_aprs_heard_screen, ui_aprs_pos_screen;	// ui_aprs.c

static void item_num(ui_item_t *it, uint16_t n)
{
	it->num = (char)('0' + n % 10u);
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

#define CH_VFO	(CONFIG_VFO_FM ? 1u : 0u)	// raw row 0 is the VFO when it is built

static bool chans_is_vfo(uint16_t raw) { return CONFIG_VFO_FM && raw == 0; }
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

static void chans_select(uint16_t i, bool hold)
{
	static char msg[CODEPLUG_NAME_LEN + 8];
	static zone_t z;
	(void)hold;
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
		sb_str(&wb, ch->mode == RADIO_MODE_DMR ? "DMR" : "FM");
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
	return zone_of_frame()->count;
}

static bool zone_raw_label(uint16_t raw, char *buf, uint8_t len)
{
	static channel_t ch;
	const zone_t *z = zone_of_frame();
	if (raw >= z->count || !codeplug_channel(z->channels[raw], &ch))
		return false;
	sbuf_t b;
	sb_init(&b, buf, len);
	sb_u32(&b, raw + 1u, 1);
	sb_chr(&b, ' ');
	sb_str(&b, ch.name);
	return true;
}

static void zone_enter(void)
{
	const radio_view_t *v = ui_view();
	if (!v->vfo && v->zone_index == ui_frame()->arg)
		for (uint16_t i = 0; i < ui_filter_count(); i++)
			if (ui_filter_raw(i) == v->zone_pos)
				ui_frame()->focus = i;
}

static void zone_item(uint16_t i, ui_item_t *it)
{
	static channel_t ch;
	const zone_t *z = zone_of_frame();
	uint16_t raw = ui_filter_raw(i);
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

static void zone_select(uint16_t i, bool hold)
{
	(void)hold;
	const radio_view_t *v = ui_view();
	uint16_t zi = ui_frame()->arg;
	if (v->vfo)
		radio_req_vfo(false);
	if (v->zone_index != zi)
		radio_req_zone(zi);
	radio_req_zone_pos(ui_filter_raw(i));
	ui_go_home();
}

static const char *zone_title(void)
{
	return zone_of_frame()->name;
}

static const ui_scr_def_t zone_screen = {
	.name = "zone", .flags = SF_LIST | SF_FILTER | SF_FIELD, .prefix = NULL,
	.hint = "OK go  0-9 find  Back", .empty = "Empty zone",
	.count = ui_filter_count, .item = zone_item, .select = zone_select, .title = zone_title,
	.enter = zone_enter, .raw_count = zone_raw_count, .raw_label = zone_raw_label,
};

#if CONFIG_CHANNEL_LIST
// ======================= Channels: every codeplug channel by number (hub 5) =======================
// The raw row is the channel number - 1 (with CONFIG_CHANNEL_EDIT, raw 0 is "New from current" and the raw
// row is the channel number); empty slots have no label, so the filter leaves them out
#define CHLIST_NEW	(CONFIG_CHANNEL_EDIT ? 1u : 0u)
#define CHLIST_NUM(raw)	((uint16_t)((raw) + 1u - CHLIST_NEW))
#if CONFIG_CHANNEL_EDIT
static bool ced_from_current;		// the next push of the channel opens a copy of what the radio uses now
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
       CE_ZONES, CE_GO, CE_SAVE, CE_SAVE_NEW, CE_COUNT, CE_NONE = 0xFF };

static channel_t ced_ch;		// what the screen shows (the working copy when editing)
static uint16_t ced_num;		// its number, 0 = no such channel
static uint8_t ced_rows[CE_COUNT];
static uint8_t ced_n;

#if CONFIG_CHANNEL_EDIT
#define CE_TOT_MAX	495
#define CE_TOT_STEP	15
#define CE_ID_MAX	16777215
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
	sbuf_t b;
	char name[CODEPLUG_NAME_LEN + 1];
	sb_init(&b, name, sizeof name);
	if (v->vfo) {
		sb_freq(&b, ced_ch.rx_hz);
	} else {
		ced_ch.name[CODEPLUG_NAME_LEN - 2] = 0;	// room for " 2"
		sb_str(&b, ced_ch.name);
		sb_str(&b, " 2");
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
		if ((r == CE_ZONES || r == CE_GO || r == CE_SAVE_NEW) && fresh)
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
	return ced_ch.name[0] != 0 ? ced_ch.name : "New channel";
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
		"Battery saver", "Zones", "Go to it", "Save", "Save as new channel" };
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
	if (!ui_in_band(c->rx_hz, false))
		return "RX is outside the radio's bands";
	if (!c->rx_only && !ui_in_band(c->tx_hz, true))
		return "TX can't transmit there: set RX only";
	if (CONFIG_DMR && c->mode == RADIO_MODE_DMR && c->contact == 0)
		return "Pick a TX contact first";
	return NULL;
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
	if (!ui_svc_write_channel(num, &ced_ch)) {
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
			ui_toast("IDs go up to 16777215");
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
			c->tx_hz = ui_in_band(tx, false) ? tx : hz;
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
	case CE_ZONES: ui_toast("Zones: change them with qdmr for now"); break;
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
	.name = "discard", .flags = SF_LIST, .prefix = "Unsaved changes", .hint = TEXT_ALT_OPEN "OK choose  Back keep editing" TEXT_ALT_NEXT "OK choose  Back" TEXT_ALT_CLOSE,
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

static const char *chpick_title(void) { return ui_frame()->arg == 0 ? "TX contact" : "RX group list"; }

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

static const char *const info_titles[] = { "Position", "Hotspot", "Battery", "About", "Codeplug", "My radio",
					   "USB network", "Single-freq repeater" };

// The USB personality can be switched here (the host link's network adapter, CONFIG_USB_NETWORK)
#define USB_NET_UI	(CONFIG_USB_NETWORK && CONFIG_HOSTLINK)

static const char *info_title(void) { return info_titles[ui_frame()->arg]; }

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
	case INFO_SFR:
		sb_str(&b, CONFIG_SFR ? "Runs in hotspot mode: the hotspot host turns it on. Repeater and user roles on a "
			"channel: not built yet." : "Single-freq repeater: not in this build");
		break;
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

// ======================= About (Settings > Advanced) =======================
// A text page, every word shown (nothing shortened), no status strip while idle: the version, ODR's license, the
// authors, the third-party code and fonts in this image and the codec. From AUTHORS and platform/<platform>/THIRD_PARTY,
// through the header make generates (tools/gen_about.py)

static const char *const about_authors[] = ODR_ABOUT_AUTHORS;
static const char *const about_third[][2] = ODR_ABOUT_THIRD_PARTY;
static uint16_t about_first;	// first line shown
static uint16_t about_max;	// the largest first line (from the last draw)

static const char *about_text(void)
{
	static char t[400];
	sbuf_t b;
	sb_init(&b, t, sizeof t);
	sb_str(&b, "OpenDigitalRadio ");
	sb_str(&b, ODR_GIT);
	sb_str(&b, "\nRadio: ");
	sb_str(&b, board.name);
	sb_str(&b, "\nMIT License, (c) 2026 The OpenDigitalRadio Authors\n");
	sb_str(&b, ODR_ABOUT_AUTHOR_COUNT > 1 ? "Authors: " : "Author: ");
	for (unsigned i = 0; i < ODR_ABOUT_AUTHOR_COUNT; i++) {
		if (i)
			sb_str(&b, ", ");
		sb_str(&b, about_authors[i]);
	}
	for (unsigned i = 0; i < ODR_ABOUT_THIRD_PARTY_COUNT; i++) {
		sb_chr(&b, '\n');
		sb_str(&b, about_third[i][0]);
		sb_str(&b, ": ");
		sb_str(&b, about_third[i][1]);
	}
	sb_str(&b, "\nSpleen font: BSD-2-Clause");	// core/ui/fonts.c
#if CONFIG_FONT_AA
	sb_str(&b, "\nJetBrains Mono font: OFL-1.1");
#endif
	sb_str(&b, "\nAMBE+2 codec: DVSI (stock)");	// from the radio's own firmware, not part of ODR
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

// ======================= Tools =======================

// Diagnostics keeps number 7 (tests/hil/test_ui.py); Roaming follows (8, or 7 without diagnostics), then APRS
enum { T_SCAN, T_POSITION, T_HOTSPOT, T_USB_NET, T_SFR, T_BATTERY, T_DIAG, T_ROAM, T_APRS, T_COUNT };

static uint16_t tools_count(void) { return CONFIG_DIAG ? T_COUNT : T_COUNT - 1; }
static uint16_t tools_row(uint16_t i) { return !CONFIG_DIAG && i >= T_DIAG ? (uint16_t)(i + 1u) : i; }

static void tools_item(uint16_t row, ui_item_t *it)
{
	static const char *const names[] = { "Scan", "Position", "Hotspot", "USB network", "Single-freq repeater",
					     "Battery", "Diagnostics", "Roaming", "APRS" };
	uint16_t i = tools_row(row);
	strcpy(it->label, names[i]);
	item_num(it, (uint16_t)(row + 1u));
	sbuf_t b;
	sb_init(&b, it->value, sizeof it->value);
	switch (i) {
	case T_SCAN:
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
			ui_scan_toggle();	// says what scans: the zone, or the band in the VFO
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
	default: break;
	}
}

static const ui_scr_def_t tools_screen = {
	.name = "tools", .flags = SF_LIST | SF_NUMBERED | SF_FIELD, .prefix = "Tools",
	.hint = CONFIG_DIAG ? "1-9 open  Back" : "1-8 open  Back",
	.count = tools_count, .item = tools_item, .select = tools_select,
};

// ======================= Settings by intent (information-architecture.md 6) =======================
// CONFIG_UI_SETTINGS=n leaves the whole menu out (the settings keep their stored values; qdmr sets them)
#if CONFIG_UI_SETTINGS

typedef enum { ST_TOGGLE, ST_LIST, ST_NUMBER, ST_ACTION } set_type_t;
enum { SA_DIAG, SA_FIRMWARE, SA_CODEPLUG, SA_RESET, SA_MY_RADIO };

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
	const set_item_t *items;
	uint8_t count;
} set_group_t;

static const int16_t v_backlight[] = { 5, 10, 30, 60, 120, 0 };
static const char *const n_backlight[] = { "5 s", "10 s", "30 s", "1 min", "2 min", "Never" };
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
#if CONFIG_DMR
					 KEYFN_CALL_BACK,
#endif
};
static const char *const n_keyfn_press[] = { "Default", "Nothing", "Monitor", "Talk target", "Power level",
					     "Next zone", "Previous zone", "Scan", "Key lock", "Home zone",
#if CONFIG_DMR
					     "Call back",
#endif
};
static const int16_t v_keyfn_hold[] = { KEYFN_DEFAULT, KEYFN_NONE, KEYFN_TALK_TARGET, KEYFN_POWER, KEYFN_ZONE_UP,
					KEYFN_ZONE_DOWN, KEYFN_SCAN,
#if CONFIG_DMR_EMERGENCY
					KEYFN_EMERGENCY,
#endif
					KEYFN_KEY_LOCK, KEYFN_HOME_ZONE,
#if CONFIG_DMR
					KEYFN_CALL_BACK,
#endif
};
static const char *const n_keyfn_hold[] = { "Default", "Nothing", "Talk target", "Power level", "Next zone",
					    "Previous zone", "Scan",
#if CONFIG_DMR_EMERGENCY
					    "Emergency",
#endif
					    "Key lock", "Home zone",
#if CONFIG_DMR
					    "Call back",
#endif
};
#endif
static const int16_t v_updown[] = { 0, 1, 2 };
static const char *const n_updown[] = { "Channel", "Zone", "Talk target" };
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
#if CONFIG_DMR_CONTROL_STATION
static const int16_t v_netlink[] = { 0, 1 };
static const char *const n_netlink[] = { "By DMR ID", "192.168.10.x" };
#endif
static const int16_t v_ptype[] = { 0, 1, 2 };	// dmrPrivacyType_t
static const char *const n_ptype[] = { "Off", "Motorola", "Anytone" };

#define TOGGLE(nm, syn_, sid) { nm, syn_, ST_TOGGLE, sid, 0, 1, 1, NULL, NULL, 0, NULL }
#define NUMBER(nm, syn_, sid, lo, hi, st, u) { nm, syn_, ST_NUMBER, sid, lo, hi, st, NULL, NULL, 0, u }
#define LIST(nm, syn_, sid, v, n) { nm, syn_, ST_LIST, sid, 0, 0, 0, v, n, sizeof v / sizeof v[0], NULL }
#define ACTION(nm, syn_, a) { nm, syn_, ST_ACTION, a, 0, 0, 0, NULL, NULL, 0, NULL }

static const set_item_t s_sound[] = {
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
	LIST("Backlight time", "timeout", UI_SET_BACKLIGHT_S, v_backlight, n_backlight),
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
	NUMBER("Mic gain", NULL, UI_SET_MIC_GAIN, 0, 10, 1, NULL),
	TOGGLE("Monitor by default", "digital monitor promiscuous hear all", UI_SET_HEAR_ALL_DEFAULT),
	TOGGLE("Show talker alias", NULL, UI_SET_ALIAS_SHOW),
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
static const set_item_t s_keys[] = {
	NUMBER("Long press time", NULL, UI_SET_LONG_PRESS_MS, 400, 1200, 100, " ms"),
	NUMBER("Key repeat", NULL, UI_SET_REPEAT_MS, 60, 300, 20, " ms"),
	LIST("Up/Down on Home", NULL, UI_SET_UPDOWN_HOME, v_updown, n_updown),
#if CONFIG_KEY_ASSIGN
	LIST("Side key 1", "button programmable", UI_SET_KEY_SIDE1, v_keyfn_press, n_keyfn_press),
	LIST("Side key 1 hold", "button programmable long press", UI_SET_KEY_SIDE1_HOLD, v_keyfn_hold, n_keyfn_hold),
	LIST("Side key 2", "button programmable", UI_SET_KEY_SIDE2, v_keyfn_press, n_keyfn_press),
	LIST("Side key 2 hold", "button programmable long press", UI_SET_KEY_SIDE2_HOLD, v_keyfn_hold, n_keyfn_hold),
	LIST("Top key", "button programmable", UI_SET_KEY_TOP, v_keyfn_press, n_keyfn_press),
	LIST("Top key hold", "button programmable long press", UI_SET_KEY_TOP_HOLD, v_keyfn_hold, n_keyfn_hold),
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
};

#define GROUP(nm, a) { nm, a, sizeof a / sizeof a[0] }
static const set_group_t groups[] = {
	GROUP("Sound", s_sound), GROUP("Display", s_display), GROUP("Talk & listen", s_talk),
	GROUP("Privacy", s_privacy), GROUP("Location", s_location), GROUP("Connections", s_connections),
	GROUP("Power", s_power), GROUP("Keys", s_keys), GROUP("My radio", s_my_radio), GROUP("Advanced", s_advanced),
};
#define GROUPS	(sizeof groups / sizeof groups[0])

uint8_t ui_set_group_count(void) { return GROUPS; }
const char *ui_set_group_name(uint8_t g) { return g < GROUPS ? groups[g].name : ""; }
uint8_t ui_set_item_count(uint8_t g) { return g < GROUPS ? groups[g].count : 0; }

const char *ui_set_item_name(uint8_t g, uint8_t i, const char **synonyms)
{
	*synonyms = NULL;
	if (g >= GROUPS || i >= groups[g].count)
		return "";
	*synonyms = groups[g].items[i].syn;
	return groups[g].items[i].name;
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
	return g < GROUPS && i < groups[g].count ? &groups[g].items[i] : NULL;
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


// Why a setting can't be used on this radio or in this build (the row is dimmed and OK says it), NULL if it can
static const char *set_why_not(const set_item_t *s)
{
	if (s->type == ST_ACTION)
		return NULL;
	switch (s->id) {
	case UI_SET_VOICE_PROMPTS:
		return "Voice prompts: not in this build";
#if CONFIG_KEY_ASSIGN
	case UI_SET_KEY_SIDE1:
	case UI_SET_KEY_SIDE1_HOLD:
	case UI_SET_KEY_SIDE2:
	case UI_SET_KEY_SIDE2_HOLD:
	case UI_SET_KEY_TOP:
	case UI_SET_KEY_TOP_HOLD:
		if ((s->id - UI_SET_KEY_SIDE1) / 2 >= board.side_key_count)
			return "This radio has no such key";
		return NULL;
#endif
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
		return mic_gain_available() ? NULL : "Mic gain: not in this build";
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
		case SA_MY_RADIO: ui_push(SCR_INFO, INFO_MY_RADIO); break;
		case SA_RESET: ui_push(SCR_CONFIRM, 0); break;
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

static const char *group_title(void) { return ui_set_group_name((uint8_t)ui_frame()->arg); }

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
	.name = "confirm", .flags = SF_LIST, .prefix = "Factory reset", .hint = "OK choose  Back cancel",
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
#if CONFIG_CHANNEL_LIST
	[SCR_CHANNELS] = &chlist_screen, [SCR_CHANNEL] = &chinfo_screen,
#if CONFIG_CHANNEL_EDIT
	[SCR_TEXT] = &text_screen, [SCR_DISCARD] = &discard_screen,
#if CONFIG_DMR
	[SCR_CHPICK] = &chpick_screen,
#endif
#endif
#endif
};
