// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The list screens and the screen table (information-architecture.md 4, 6, 8): the Hub with its six destinations,
// actions on a station, Contacts, Messages, Recent, Channels, Tools, Settings grouped by intent, the talk target
// picker, information pages and the one confirmation (factory reset, P7).
#include <string.h>
#include "board.h"
#include "ui_nav.h"
#include "ui_svc.h"
#include "fmt.h"
#include "codeplug/codeplug.h"
#include "config.h"
#if CONFIG_HOTSPOT
#include "hostlink/hostlink.h"
#endif

#ifndef ODR_GIT
#define ODR_GIT "host"
#endif

extern const ui_scr_def_t ui_find_screen;

static void item_num(ui_item_t *it, uint16_t n)
{
	it->num = (char)('0' + n % 10u);
}

// ======================= Hub =======================

enum { HUB_LAST_CALLER, HUB_CONTACTS, HUB_MESSAGES, HUB_RECENT, HUB_CHANNELS, HUB_TOOLS, HUB_SETTINGS };

static bool hub_has_last(void)
{
	return !ui_has_action(ACT_CONTEXT);	// few-key boards: "Last caller" stands in for CONTEXT (7.5)
}

static uint16_t hub_count(void) { return hub_has_last() ? 7 : 6; }

static uint8_t hub_entry(uint16_t i)
{
	return (uint8_t)(hub_has_last() ? i : i + 1u);
}

static void hub_item(uint16_t i, ui_item_t *it)
{
	static const char *const names[] = { "Last caller", "Contacts", "Messages", "Recent", "Channels", "Tools",
					     "Settings" };
	uint8_t e = hub_entry(i);
	strcpy(it->label, names[e]);
	item_num(it, e);
	sbuf_t b;
	sb_init(&b, it->value, sizeof it->value);
	if (e == HUB_MESSAGES && ui_st.unread) {
		sb_u32(&b, ui_st.unread, 1);
		sb_str(&b, " new");
		it->flags |= UI_ITEM_WARN;
	} else if (e == HUB_CHANNELS) {
		sb_str(&b, ui_view()->vfo ? "VFO" : ui_view()->zone_name);
	} else if (e == HUB_TOOLS && ui_st.scanning) {
		sb_str(&b, "scanning");
	}
}

static void hub_select(uint16_t i, bool hold)
{
	static const uint8_t scr[] = { SCR_RECENT, SCR_CONTACTS, SCR_MESSAGES, SCR_RECENT, SCR_CHANNELS, SCR_TOOLS,
				       SCR_SETTINGS };
	(void)hold;
	uint8_t e = hub_entry(i);
	if (e == HUB_LAST_CALLER) {
		if (ui_recent_count() == 0) {
			ui_toast("No one heard yet: callers show up here");
			return;
		}
		ui_target_t t;
		ui_recent_target(ui_recent_get(0), &t);
		ui_open_actions(&t);
		return;
	}
	ui_push(scr[e], 0);
}

static const ui_scr_def_t hub_screen = {
	.name = "hub", .flags = SF_LIST | SF_NUMBERED | SF_FIELD, .hint = "1-6 open  Back home",
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
		sb_str(&b, ui_target.group ? "Message the group" : "Message");
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
		sb_str(&b, "Save changes to channel");
		if (!(feats() & UI_FEAT_SAVE_CHANNEL))
			it->flags |= UI_ITEM_DIM;
		break;
	case A_MAKE_DEFAULT:
		sb_str(&b, "Make default here");
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
			ui_toast("Text messages aren't in this build yet");
		break;
	case A_CHECK:
		if (ui_svc_radio_check(ui_target.id)) {
			sb_str(&b, "Checking ");
			sb_str(&b, name);
			sb_str(&b, "...");
			ui_go_home();
			ui_toast(msg);
		} else {
			ui_toast("Radio check isn't in this build yet");
		}
		break;
	case A_ALERT:
		if (ui_svc_call_alert(ui_target.id)) {
			sb_str(&b, "Call alert to ");
			sb_str(&b, name);
			ui_go_home();
			ui_toast(msg);
		} else {
			ui_toast("Call alert isn't in this build yet");
		}
		break;
	case A_POSITION:
		ui_toast("Position requests aren't in this build yet");
		break;
	case A_SAVE_CONTACT:
		ui_toast(ui_target.saved ? "Already in your contacts" : "Add contacts in qdmr for now");
		break;
	case A_SPEAK:
		ui_toast("Voice prompts aren't in this build yet");
		break;
	case A_SAVE_CHANNEL:
		if (ui_svc_save_channel()) {
			ui_go_home();
			ui_toast("Saved to channel");
		} else {
			ui_toast("Saving needs the codeplug writer: use qdmr");
		}
		break;
	default:
		break;
	}
}

static const char *act_title(void)
{
	static char t[40];
	ui_sb_target(t, sizeof t, &ui_target);
	if (!ui_target.group && ui_target.heard_tg) {
		sbuf_t b;
		sb_init(&b, t + strlen(t), sizeof t - strlen(t));
		sb_str(&b, " on TG");
		sb_u32(&b, ui_target.heard_tg, 1);
	}
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
		ui_toast("Couldn't queue the message: try again");
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
		ui_toast("Typed messages come later: pick a canned one");
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
		sb_str(&vb, "All");
	} else {
		sb_str(&vb, t.group ? "TG " : "");
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
	.empty = "Contacts are for DMR: not in this build", .count = none_count, .item = none_item,
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
	.empty = "No messages yet. Messages you get show up here.",
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
			ui_toast("Couldn't delete it: try again");
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
		return "This message is gone.";
	sbuf_t b;
	sb_init(&b, t, sizeof t);
	sb_str(&b, m.sent ? "To " : "From ");
	if (m.name[0]) {
		sb_str(&b, m.name);
		sb_chr(&b, ' ');
	}
	sb_u32(&b, m.src, 1);
	sb_str(&b, ", ");
	char age[8];
	ui_sb_age(age, sizeof age, m.age_ms);
	sb_str(&b, age);
	sb_str(&b, " ago\n");
	sb_str(&b, m.text);
	return t;
}

static const ui_scr_def_t message_screen = {
	.name = "message", .flags = SF_LIST | SF_FIELD, .prefix = "Msg", .hint = "OK send  # more  Back",
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
			sb_str(&vb, "TG");
			sb_u32(&vb, r->dst, 1);
		} else {
			sb_str(&vb, r->to_us ? "you" : "priv");
		}
		sb_chr(&vb, ' ');
	}
	char age[8];
	ui_sb_age(age, sizeof age, ui_st.now_ms - r->ms);
	sb_str(&vb, age);
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
	.empty = "No calls yet. Stations you hear show up here.",
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
			sb_str(&vb, "on");
		return;
	}
	uint16_t zi = (uint16_t)(raw - CH_VFO);
	if (!codeplug_zone(zi, &z))
		return;
	strcpy(it->label, z.name);
	if (!v->vfo && zi == v->zone_index)
		sb_str(&vb, "here ");
	sb_u32(&vb, z.count, 1);
	sb_str(&vb, " ch");
}

static void chans_select(uint16_t i, bool hold)
{
	static char msg[CODEPLUG_NAME_LEN + 8];
	static zone_t z;
	(void)hold;
	const radio_view_t *v = ui_view();
	uint16_t raw = ui_filter_raw(i);
	if (chans_is_vfo(raw)) {
		radio_req_vfo(!v->vfo);
		ui_go_home();
		ui_toast(v->vfo ? "Back to channels" : "VFO: type a frequency");
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
	.name = "channels", .flags = SF_LIST | SF_FILTER | SF_FIELD, .prefix = "Channels",
	.hint = "OK go  # channels  0-9 find", .empty = "No zones",
	.count = ui_filter_count, .item = chans_item, .select = chans_select, .context = chans_context,
	.enter = chans_enter, .raw_count = chans_raw_count, .raw_label = chans_raw_label,
};

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
	sb_str(&b, ch.mode == RADIO_MODE_DMR ? "DMR" : "FM");
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
	.hint = "OK go  0-9 find  Back", .empty = "This zone is empty",
	.count = ui_filter_count, .item = zone_item, .select = zone_select, .title = zone_title,
	.enter = zone_enter, .raw_count = zone_raw_count, .raw_label = zone_raw_label,
};

// ======================= Info pages =======================

enum { INFO_POSITION, INFO_HOTSPOT, INFO_BATTERY, INFO_FIRMWARE, INFO_CODEPLUG, INFO_MY_RADIO, INFO_KEYS,
       INFO_USB_NET, INFO_SFR };

static const char *const info_titles[] = { "Position", "Hotspot", "Battery", "Firmware", "Codeplug", "My radio",
					   "Privacy keys", "USB network", "SFR" };

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
	static char t[160];
	sbuf_t b;
	sb_init(&b, t, sizeof t);
	switch (ui_frame()->arg) {
	case INFO_POSITION: {
		ui_position_t p;
		if (!ui_svc_position(&p)) {
			sb_str(&b, "No GPS on this radio, or it is off (Settings > Location).");
		} else if (!p.fix) {
			sb_str(&b, "Searching for satellites: ");
			sb_u32(&b, p.sats, 1);
			sb_str(&b, " seen. Go outside for a fix.");
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
		sb_str(&b, "Connect USB and start MMDVMHost (or Pi-Star) on the computer: the radio becomes the hotspot "
			"by itself. OK or hold Back leaves hotspot mode.");
#else
		sb_str(&b, "The hotspot isn't in this build.");
#endif
		break;
	case INFO_BATTERY:
		sb_str(&b, "Battery ");
		sb_u32(&b, ui_st.battery_pct, 1);
		sb_str(&b, ui_st.battery_pct <= 15 ? "%: charge soon." : "%.");
		break;
	case INFO_FIRMWARE:
		sb_str(&b, "OpenDigitalRadio\n");
		sb_str(&b, board.name);
		sb_str(&b, "\ngit ");
		sb_str(&b, ODR_GIT);
		break;
	case INFO_CODEPLUG: {
		static const char *const src[] = { "none (built-in channels)", "built-in channels", "from the CPS" };
		codeplug_source_t s = codeplug_source();
		sb_str(&b, "Codeplug: ");
		sb_str(&b, (unsigned)s < 3 ? src[s] : "?");
		sb_str(&b, "\n");
		sb_u32(&b, codeplug_zone_count(), 1);
		sb_str(&b, " zones, channels up to ");
		sb_u32(&b, codeplug_channel_count(), 1);
		break;
	}
	case INFO_MY_RADIO:
#if CONFIG_DMR
		sb_str(&b, "DMR ID ");
		if (codeplug_dmr_id())
			sb_u32(&b, codeplug_dmr_id(), 1);
		else
			sb_str(&b, "not set: set it in qdmr");
		sb_chr(&b, '\n');
#endif
		sb_str(&b, "Radio ");
		sb_str(&b, board.name);
		break;
	case INFO_KEYS:
		sb_str(&b, "Privacy keys are loaded with the codeplug (qdmr). A channel or contact with a key can use "
			"privacy from Quick settings (6).");
		break;
	case INFO_USB_NET:
		sb_str(&b, CONFIG_USB_NETWORK ? "USB network: connect USB, the computer sees a network adapter." :
			"USB network isn't in this build.");
		break;
	case INFO_SFR:
		sb_str(&b, CONFIG_SFR ? "Single frequency repeater: set the role in Settings > Connections." :
			"Single frequency repeater isn't in this build.");
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
	return 0;
}

static void info_item(uint16_t i, ui_item_t *it)
{
	(void)i;
	strcpy(it->label, "Stop hotspot");
}

static void info_select(uint16_t i, bool hold)
{
	(void)i;
	(void)hold;
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

// ======================= Tools =======================

enum { T_SCAN, T_POSITION, T_HOTSPOT, T_USB_NET, T_SFR, T_BATTERY, T_DIAG, T_COUNT };

static uint16_t tools_count(void) { return CONFIG_DIAG ? T_COUNT : T_COUNT - 1; }

static void tools_item(uint16_t i, ui_item_t *it)
{
	static const char *const names[] = { "Scan", "Position", "Hotspot", "USB network", "Single freq. repeater",
					     "Battery", "Diagnostics" };
	strcpy(it->label, names[i]);
	item_num(it, (uint16_t)(i + 1u));
	sbuf_t b;
	sb_init(&b, it->value, sizeof it->value);
	switch (i) {
	case T_SCAN:
		sb_str(&b, ui_st.scanning ? "on" : "off");
		if (!(feats() & UI_FEAT_SCAN))
			it->flags |= UI_ITEM_DIM;
		break;
	case T_POSITION:
		if (!(feats() & UI_FEAT_GPS))
			it->flags |= UI_ITEM_DIM;
		break;
	case T_HOTSPOT:
#if CONFIG_HOTSPOT
		sb_str(&b, hostlink_mode() == HOSTLINK_HOTSPOT ? "on" : "off");
#else
		it->flags |= UI_ITEM_DIM;
#endif
		break;
	case T_USB_NET:
		if (!CONFIG_USB_NETWORK)
			it->flags |= UI_ITEM_DIM;
		break;
	case T_SFR:
		if (!CONFIG_SFR)
			it->flags |= UI_ITEM_DIM;
		break;
	case T_BATTERY:
		sb_u32(&b, ui_st.battery_pct, 1);
		sb_chr(&b, '%');
		break;
	default:
		break;
	}
}

static void tools_select(uint16_t i, bool hold)
{
	(void)hold;
	switch (i) {
	case T_SCAN:
		if (!(feats() & UI_FEAT_SCAN)) {
			ui_toast("Scan isn't in this build yet");
		} else if (ui_svc_scan(!ui_st.scanning)) {
			ui_st.scanning = !ui_st.scanning;
			ui_go_home();
			ui_toast(ui_st.scanning ? "Scanning this zone" : "Scan stopped");
		}
		break;
	case T_POSITION: ui_push(SCR_INFO, INFO_POSITION); break;
	case T_HOTSPOT: ui_push(SCR_INFO, INFO_HOTSPOT); break;
	case T_USB_NET: ui_push(SCR_INFO, INFO_USB_NET); break;
	case T_SFR: ui_push(SCR_INFO, INFO_SFR); break;
	case T_BATTERY: ui_push(SCR_INFO, INFO_BATTERY); break;
	case T_DIAG: ui_push(SCR_DIAG, 0); break;
	default: break;
	}
}

static const ui_scr_def_t tools_screen = {
	.name = "tools", .flags = SF_LIST | SF_NUMBERED | SF_FIELD, .prefix = "Tools", .hint = "1-7 open  Back",
	.count = tools_count, .item = tools_item, .select = tools_select,
};

// ======================= Settings by intent (information-architecture.md 6) =======================
// CONFIG_UI_SETTINGS=n leaves the whole menu out (the settings keep their stored values; qdmr sets them)
#if CONFIG_UI_SETTINGS

typedef enum { ST_TOGGLE, ST_LIST, ST_NUMBER, ST_ACTION } set_type_t;
enum { SA_DIAG, SA_FIRMWARE, SA_CODEPLUG, SA_RESET, SA_MY_RADIO, SA_KEYS };

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
static const int16_t v_theme[] = { 0, 1, 2 };
static const char *const n_theme[] = { "Dark", "Light", "High contrast" };
static const int16_t v_tot[] = { 0, 30, 60, 120, 180, 300, 600 };
static const char *const n_tot[] = { "Off", "30 s", "1 min", "2 min", "3 min", "5 min", "10 min" };
static const int16_t v_lock[] = { 0, 30, 60, 300 };
static const char *const n_lock[] = { "Off", "30 s", "1 min", "5 min" };
static const int16_t v_posfmt[] = { 0, 1, 2 };
static const char *const n_posfmt[] = { "Degrees", "Deg min sec", "Grid" };
static const int16_t v_possend[] = { 0, 60, 300, 900 };
static const char *const n_possend[] = { "Off", "1 min", "5 min", "15 min" };
static const int16_t v_updown[] = { 0, 1, 2 };
static const char *const n_updown[] = { "Channel", "Zone", "Talk target" };
static const int16_t v_autooff[] = { 0, 30, 60, 120 };
static const char *const n_autooff[] = { "Off", "30 min", "1 h", "2 h" };

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
};
static const set_item_t s_display[] = {
	NUMBER("Brightness", "backlight", UI_SET_BRIGHTNESS, 10, 100, 10, "%"),
	LIST("Backlight time", "timeout", UI_SET_BACKLIGHT_S, v_backlight, n_backlight),
	LIST("Theme", "contrast", UI_SET_THEME, v_theme, n_theme),
	TOGGLE("Large text", "accessibility", UI_SET_LARGE_TEXT),
	TOGGLE("Hint row", "help", UI_SET_HINTS),
};
static const set_item_t s_talk[] = {
	NUMBER("Reply window", NULL, UI_SET_REPLY_S, 0, 30, 1, " s"),
	LIST("TX timeout", "TOT", UI_SET_TOT_S, v_tot, n_tot),
	NUMBER("Default squelch", NULL, UI_SET_SQUELCH, 0, 9, 1, NULL),
	NUMBER("Mic gain", NULL, UI_SET_MIC_GAIN, 0, 10, 1, NULL),
	TOGGLE("Hear all by default", "promiscuous", UI_SET_HEAR_ALL_DEFAULT),
	TOGGLE("Show talker alias", NULL, UI_SET_ALIAS_SHOW),
};
static const set_item_t s_privacy[] = {
	ACTION("Privacy keys", "encryption", SA_KEYS),
	LIST("Auto lock", "keylock", UI_SET_AUTO_LOCK_S, v_lock, n_lock),
	TOGGLE("PTT when locked", NULL, UI_SET_PTT_LOCKED),
};
static const set_item_t s_location[] = {
	TOGGLE("GPS", NULL, UI_SET_GPS),
	LIST("Position format", NULL, UI_SET_POS_FORMAT, v_posfmt, n_posfmt),
	LIST("Send position", "APRS", UI_SET_POS_SEND_S, v_possend, n_possend),
	TOGGLE("Answer position requests", "LRRP", UI_SET_POS_REPLY),
};
static const set_item_t s_connections[] = {
	TOGGLE("Auto hotspot", "MMDVM", UI_SET_HOTSPOT_AUTO),
	TOGGLE("Answer radio checks", NULL, UI_SET_CHECK_REPLY),
	TOGGLE("Answer call alerts", NULL, UI_SET_ALERT_ANSWER),
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
};
static const set_item_t s_my_radio[] = {
	ACTION("DMR ID and name", "callsign", SA_MY_RADIO),
};
static const set_item_t s_advanced[] = {
	ACTION("Firmware", "version", SA_FIRMWARE),
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

static void group_item(uint16_t i, ui_item_t *it)
{
	const set_item_t *s = set_item_at(i);
	if (s == NULL)
		return;
	strcpy(it->label, s->name);
	bool edit = set_editing && i == ui_frame()->focus;
	if (s->type != ST_ACTION)
		set_value_text(s, edit ? set_value : ui_svc_setting_get(s->id), it->value, sizeof it->value);
	if (edit)
		it->flags |= UI_ITEM_EDIT;
}

// Settings the UI itself uses take effect at once
static void apply_setting(uint8_t id, int32_t v)
{
	if (id == UI_SET_BRIGHTNESS)
		ui_st.brightness = (uint8_t)v;
	else if (id == UI_SET_HINTS)
		ui_st.hints = v != 0;
	else if (id == UI_SET_LARGE_TEXT)
		ui_st.large_text = v != 0;
}

static void set_store(const set_item_t *s, int32_t v)
{
	if (ui_svc_setting_set(s->id, v)) {
		apply_setting(s->id, v);
		ui_svc_apply(s->id, v);
	} else {
		ui_toast("Couldn't save the setting: try again");
	}
}

static void group_select(uint16_t i, bool hold)
{
	const set_item_t *s = set_item_at(i);
	if (s == NULL)
		return;
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
			set_value = v;
		}
		break;
	case ST_ACTION:
		switch (s->id) {
		case SA_DIAG:
			if (CONFIG_DIAG)
				ui_push(SCR_DIAG, 0);
			else
				ui_toast("Diagnostics aren't in this build");
			break;
		case SA_FIRMWARE: ui_push(SCR_INFO, INFO_FIRMWARE); break;
		case SA_CODEPLUG: ui_push(SCR_INFO, INFO_CODEPLUG); break;
		case SA_MY_RADIO: ui_push(SCR_INFO, INFO_MY_RADIO); break;
		case SA_KEYS: ui_push(SCR_INFO, INFO_KEYS); break;
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
	int dir = ev->action == ACT_NAV_UP ? 1 : ev->action == ACT_NAV_DOWN ? -1 :
		ev->action == ACT_ENC_STEP ? (ev->value > 0 ? 1 : -1) : 0;
	if (dir != 0 && ev->gesture != GESTURE_UP) {
		int32_t v = set_value + dir * s->step;
		set_value = v < s->min ? s->min : v > s->max ? s->max : v;
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
		set_store(s, set_value);
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
	strcpy(it->label, i == 0 ? "No, keep everything" : "Yes, reset");
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
		ui_toast("Reset isn't in this build yet: nothing changed");
	}
}

static const char *confirm_body(void)
{
	return "Erase all settings and go back to factory defaults? Channels stay. This can't be undone.";
}

static const ui_scr_def_t confirm_screen = {
	.name = "confirm", .flags = SF_LIST, .prefix = "Factory reset", .hint = "OK choose  Back keeps",
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
	if (v->ch.contact && codeplug_contact(v->ch.contact, &c)) {
		memset(&t, 0, sizeof t);
		t.id = c.id;
		t.group = c.type != CONTACT_PRIVATE;
		strcpy(t.name, c.name);
		pick_add(&t, "default");
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
		pick_add(&t, "reply");
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
				pick_add(&t, "listen");
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
			pick_add(&t, "recent");
		}
	}
#endif
	// Focus the reply target when there is one (task-flows.md 3: "TG 91 reply" second)
	for (uint8_t i = 0; i < n_picks; i++)
		if (picks[i].tag[0] == 'r' && picks[i].tag[1] == 'e' && picks[i].tag[2] == 'p')
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
	strcpy(it->value, picks[i].tag);
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
	.name = "picker", .flags = SF_LIST | SF_FIELD, .prefix = "Talk to", .hint = "PTT once  OK use  Hold OK set",
	.count = picker_count, .item = picker_item, .select = picker_select, .context = picker_context,
	.target = picker_target, .enter = picker_enter, .title = picker_title,
};

// ======================= Scan: what to do with the channel that stopped it =======================

static uint16_t scan_count(void) { return 3; }

static void scan_item(uint16_t i, ui_item_t *it)
{
	static const char *const names[] = { "Skip this channel for now", "Stay here", "Always skip" };
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
		ui_toast("Edit scan lists in qdmr for now");
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
	[SCR_RECENT] = &recent_screen, [SCR_CHANNELS] = &channels_screen, [SCR_ZONE] = &zone_screen,
	[SCR_TOOLS] = &tools_screen, [SCR_SETTINGS] = &settings_screen, [SCR_GROUP] = &group_screen,
	[SCR_PICKER] = &picker_screen, [SCR_INFO] = &info_screen, [SCR_CONFIRM] = &confirm_screen,
	[SCR_DIAG] = &diag_screen, [SCR_EMERGENCY] = &emergency_screen, [SCR_SCAN] = &scan_screen,
};
