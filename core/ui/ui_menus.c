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
#include "ui_svc_aprs.h"
#include "aprs/aprs_fmt.h"
#include "font.h"
#include "fmt.h"
#include "codeplug/codeplug.h"
#include "codeplug/codeplug_modes.h"
#include "codeplug/codeplug_tag.h"
#if CONFIG_P25
#include "p25/p25_codeplug.h"
#endif
#include "dmr/dmrDataService.h"
#include "config.h"
#include "hal_time.h"
#include "radio/tones.h"
#include "radio/mic_gain.h"
#include "radio/ref_trim.h"
#include "radio/radio_am.h"
#include "radio/radio_bands.h"
#include "input/keyassign.h"
#if CONFIG_HOTSPOT || CONFIG_USB_NETWORK
#include "hostlink/hostlink.h"
#endif
#include "rfoundry_about.h"
#if CONFIG_SFR
#include "dmr/sfr.h"
#endif

// Settings > Connections > Data services: the ARS server and the IP gateway
#define DATA_IDS	(CONFIG_DMR && (CONFIG_DMR_ARS || CONFIG_DMR_IP_GATEWAY))

#ifndef RFOUNDRY_GIT
#define RFOUNDRY_GIT "host"
#endif
#ifndef RFOUNDRY_RELEASE
#define RFOUNDRY_RELEASE ""
#endif
#ifndef RFOUNDRY_GIT_DATE
#define RFOUNDRY_GIT_DATE ""
#endif

extern const ui_scr_def_t ui_find_screen;
extern const ui_scr_def_t ui_aprs_screen, ui_aprs_heard_screen, ui_aprs_station_screen;	// ui_aprs.c
#if CONFIG_POSITION
extern const ui_scr_def_t ui_fixed_pos_screen;	// ui_position.c
#endif
extern const ui_scr_def_t ui_gps_screen;	// ui_gps.c
#if CONFIG_DUAL_WATCH
extern const ui_scr_def_t ui_dual_screen, ui_dual_pick_screen, ui_dual_zone_screen;	// ui_dual.c
#endif

static void item_num(ui_item_t *it, uint16_t n)
{
	it->num = n >= 1u && n <= 10u ? (char)('0' + n % 10u) : 0;	// rows after the 10th have no digit
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
	static const tr_id_t names[] = { TRN(HUB_ROW_LAST_CALLER, "Last caller"), TRN(TITLE_CONTACTS, "Contacts"),
		TRN(TITLE_MESSAGES, "Messages"), TRN(TITLE_RECENT, "Recent"), TRN(TITLE_ZONES, "Zones"),
		TRN(TITLE_CHANNELS, "Channels"), TRN(TITLE_TOOLS, "Tools"), TRN(TITLE_SETTINGS, "Settings") };
	uint8_t e = hub_entry(i);
	str_copy(it->label, sizeof it->label, tr(names[e]));
	item_num(it, e);
	sbuf_t b;
	sb_init(&b, it->value, sizeof it->value);
	if (e == HUB_MESSAGES && ui_st.unread) {
		sb_u32(&b, ui_st.unread, 1);
		sb_str(&b, TR(HUB_VALUE_NEW, " new"));
		it->flags |= UI_ITEM_WARN;
	} else if (e == HUB_ZONES) {
		// The zone (or VFO), left out where it doesn't fit beside the label (an empty last alternate)
		sb_str(&b, TEXT_ALT_OPEN);
		sb_str(&b, ui_view()->vfo ? "VFO" : ui_view()->zone_name);
		sb_str(&b, TEXT_ALT_NEXT TEXT_ALT_CLOSE);
	} else if (e == HUB_TOOLS && ui_st.scanning) {
		sb_str(&b, TR(HUB_VALUE_SCANNING, "Scanning"));
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
			ui_toast(TR(EMPTY_NO_CALLS_HEARD, "No calls heard yet"));
			return;
		}
		ui_target_t t;
		ui_recent_target(ui_recent_get(0), &t);
		ui_open_actions(&t);
		return;
	}
	if (!CONFIG_CHANNEL_LIST && e == HUB_CHANNELS) {
		// the numbers stay the same in every build
		ui_toast(TR(WHY_CHANNELS_NOT_IN_BUILD, "Channels: not in this build"));
		return;
	}
	ui_push(scr[e], 0);
}

static const ui_scr_def_t hub_screen = {
	.name = "hub", .flags = SF_LIST | SF_NUMBERED | SF_FIELD, .hint = TRN(HINT_1_7_OPEN_BACK, "1-7 open  Back"),
	.count = hub_count, .item = hub_item, .select = hub_select,
};

// ======================= Actions on a station or talkgroup =======================

enum { A_TALK, A_PRIVATE, A_MESSAGE, A_CHECK, A_ALERT, A_POSITION, A_SAVE_CONTACT, A_SPEAK, A_SAVE_CHANNEL,
       A_MAKE_DEFAULT, A_POLL };

static uint8_t act_list(uint8_t *out)
{
	uint8_t n = 0;
	if (ui_target.group) {
		out[n++] = A_TALK;
		out[n++] = A_MAKE_DEFAULT;
		out[n++] = A_MESSAGE;
		if (CONFIG_CONTACT_EDIT && !ui_target.saved && ui_target.id != 0xFFFFFFu)
			out[n++] = A_SAVE_CONTACT;
	} else {
		static const uint8_t person[] = { A_TALK, A_PRIVATE, A_MESSAGE, A_CHECK, A_ALERT, A_POSITION,
						  A_SAVE_CONTACT, A_SPEAK };
		memcpy(out, person, sizeof person);
		n = sizeof person;
#if CONFIG_DMR_TRUNK_TX
		if (ui_view()->t3.on)
			out[n++] = A_POLL;	// short data polling: a trunked system's service (dmr-trunking.md 7.6)
#endif
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
static bool compose_aprs(void);

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
		sb_str(&b, TR(ACT_ROW_TALK_TO, "Talk to "));
		sb_str(&b, name);
		if (!(feats() & UI_FEAT_TALK_TARGET))
			it->flags |= UI_ITEM_DIM;
		break;
	case A_PRIVATE:
		sb_str(&b, TR(ACT_ROW_PRIVATE_CALL, "Private call"));
		if (!(feats() & UI_FEAT_TALK_TARGET))
			it->flags |= UI_ITEM_DIM;
		break;
	case A_MESSAGE:
		sb_str(&b, TR(WORD_MESSAGE, "Message"));
		if (!(feats() & UI_FEAT_SMS))
			it->flags |= UI_ITEM_DIM;
		break;
	case A_CHECK:
		sb_str(&b, TR(ACT_ROW_RADIO_CHECK, "Radio check"));
		if (!(feats() & UI_FEAT_SIGNALING))
			it->flags |= UI_ITEM_DIM;
		break;
	case A_ALERT:
		sb_str(&b, TR(ACT_ROW_CALL_ALERT, "Call alert"));
		if (!(feats() & UI_FEAT_SIGNALING))
			it->flags |= UI_ITEM_DIM;
		break;
	case A_POSITION:
		sb_str(&b, TR(TITLE_POSITION, "Position"));
		it->flags |= UI_ITEM_DIM;
		break;
	case A_SAVE_CONTACT:
		sb_str(&b, ui_target.saved ? TR(ACT_ROW_IN_CONTACTS, "In contacts") :
			TR(ACT_ROW_SAVE_AS_CONTACT, "Save as contact"));
		if (ui_target.saved || !CONFIG_CONTACT_EDIT)
			it->flags |= UI_ITEM_DIM;
		break;
	case A_SPEAK:
		sb_str(&b, TR(ACT_ROW_SPEAK_STATUS, "Speak status"));
		it->flags |= UI_ITEM_DIM;
		break;
	case A_SAVE_CHANNEL:
		sb_str(&b, TR(ACT_ROW_SAVE_TO_CHANNEL, "Save to channel"));
		if (!(feats() & UI_FEAT_SAVE_CHANNEL))
			it->flags |= UI_ITEM_DIM;
		break;
	case A_MAKE_DEFAULT:
		sb_str(&b, TR(ACT_ROW_MAKE_CHANNEL_DEFAULT, "Make channel default"));
		if (!(feats() & UI_FEAT_TALK_TARGET))
			it->flags |= UI_ITEM_DIM;
		break;
	case A_POLL:
		sb_str(&b, TR(TRUNK_ROW_POLL_SHORT_DATA, "Poll short data"));
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
			ui_toast(TR(WHY_MESSAGES_NOT_IN_BUILD, "Messages: not in this build"));
		break;
	case A_CHECK:
		if (ui_svc_radio_check(ui_target.id)) {
			sb_str(&b, TR(ACT_TOAST_CHECKING, "Checking "));
			sb_str(&b, name);
			sb_str(&b, "...");
			ui_go_home();
			ui_toast(msg);
		} else {
			ui_toast(TR(WHY_RADIO_CHECK_NOT_IN_BUILD, "Radio check: not in this build"));
		}
		break;
	case A_ALERT:
		if (ui_svc_call_alert(ui_target.id)) {
			sb_str(&b, TR(ACT_TOAST_CALL_ALERT_TO, "Call alert to "));
			sb_str(&b, name);
			ui_go_home();
			ui_toast(msg);
		} else {
			ui_toast(TR(WHY_CALL_ALERT_NOT_IN_BUILD, "Call alert: not in this build"));
		}
		break;
	case A_POSITION:
		ui_toast(TR(WHY_POS_REQUESTS_NOT_IN_BUILD, "Position requests: not in this build"));
		break;
	case A_SAVE_CONTACT:
#if CONFIG_CONTACT_EDIT
		if (!ui_target.saved) {
			ui_new_contact(&ui_target);
			break;
		}
#endif
		ui_toast(ui_target.saved ? TR(ACT_TOAST_ALREADY_IN_CONTACTS, "Already in your contacts") :
			TR(ACT_TOAST_ADD_CONTACTS_IN_QDMR, "Add contacts in qdmr"));
		break;
	case A_SPEAK:
		ui_toast(TR(WHY_VOICE_PROMPTS_NOT_IN_BUILD, "Voice prompts: not in this build"));
		break;
	case A_SAVE_CHANNEL:
		if (ui_try_save_channel())
			ui_go_home();
		break;
#if CONFIG_DMR_TRUNK_TX
	case A_POLL:
		ui_svc_t3_poll(ui_target.id);	// the answer arrives as a message; ui_app says when none comes
		sb_str(&b, TR(TRUNK_TOAST_POLLING, "Polling "));
		sb_str(&b, name);
		ui_go_home();
		ui_toast(msg);
		break;
#endif
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
	.name = "actions", .flags = SF_LIST | SF_NUMBERED | SF_FIELD, .hint = TRN(ACT_HINT, "1-9 pick  PTT talk  Back"),
	.count = act_count, .item = act_item, .select = act_select, .target = act_target, .title = act_title,
};

// ======================= Message composer (canned first) =======================

static const tr_id_t canned[] = { TRN(MSG_BODY_CANNED_OK, "OK"), TRN(MSG_BODY_CANNED_ON_MY_WAY, "On my way"),
	TRN(MSG_BODY_CANNED_CALL_ME, "Call me"), TRN(MSG_BODY_CANNED_CANT_TALK, "Can't talk now") };
#define CANNED_N	(sizeof canned / sizeof canned[0])
// The inbox body and composer cannot be on screen at the same time.
static char message_buffer[UI_MSG_TEXT_MAX + 48];
static char aprs_to[AX25_CALL_LEN + 4];
static char aprs_compose_to[AX25_CALL_LEN + 4];
static uint8_t aprs_message_flags;
static bool aprs_composing;
#if CONFIG_APRS_AFSK
static void ui_text_open_to(char *target, uint8_t max);
#endif
#if CONFIG_DMR_SMS || CONFIG_APRS_AFSK
static void ui_text_open_message(bool aprs);
#endif
static bool compose_aprs(void) { return aprs_composing; }

void ui_aprs_compose(const char *call)
{
	str_copy(aprs_compose_to, sizeof aprs_compose_to, call);
	ui_push(SCR_COMPOSE, 1);
}

static uint16_t compose_count(void) { return CANNED_N + 1u + (compose_aprs() ? 3u : 0u); }

static void compose_item(uint16_t i, ui_item_t *it)
{
	if (compose_aprs()) {
		if (i == 0) {
			str_copy(it->label, sizeof it->label, TR(MSG_LABEL_TO_CALLSIGN, "To callsign"));
			str_copy(it->value, sizeof it->value, aprs_to[0] ? aprs_to : TR(MSG_VALUE_ENTER, "Enter"));
			return;
		}
		i--;
	}
	if (i < CANNED_N) {
		sbuf_t b;
		sb_init(&b, it->label, sizeof it->label);
		sb_chr(&b, '"');
		sb_str(&b, tr(canned[i]));
		sb_chr(&b, '"');
	} else if (i == CANNED_N) {
		str_copy(it->label, sizeof it->label, TR(MSG_ROW_WRITE, "Write..."));
	} else if (i == CANNED_N + 1u) {
		str_copy(it->label, sizeof it->label, TR(MSG_LABEL_REQUEST_ACK, "Request acknowledgment"));
		str_copy(it->value, sizeof it->value, tr((aprs_message_flags & APRS_MSG_ACK) ? TRN(VALUE_ON, "On") :
			TRN(VALUE_OFF, "Off")));
	} else {
		str_copy(it->label, sizeof it->label, TR(MSG_LABEL_INCLUDE_POSITION, "Include position"));
		str_copy(it->value, sizeof it->value, tr((aprs_message_flags & APRS_MSG_POSITION) ?
			TRN(VALUE_ON, "On") : TRN(VALUE_OFF, "Off")));
	}
}

static void send_text(const char *text, bool aprs)
{
	static char msg[40];
	char name[24];
	if (aprs) {
		str_copy(name, sizeof name, aprs_to);
		if (text != message_buffer)
			str_copy(message_buffer, sizeof message_buffer, text);
		for (char *p = message_buffer; *p != '\0'; p++) {
			if ((uint8_t)*p >= 0x80u)
				*p = font_fold(*p);
			if ((uint8_t)*p < 0x20u || (uint8_t)*p > 0x7eu)
				*p = '?';
		}
		text = message_buffer;
	} else {
		ui_sb_target(name, sizeof name, &ui_target);
	}
	if (aprs ? !ui_svc_aprs_send_message(aprs_to, text, aprs_message_flags) :
	    !ui_svc_msg_send(ui_target.id, ui_target.group, text)) {
		ui_toast(TR(MSG_TOAST_SEND_FAILED, "Couldn't send: try again"));
		return;
	}
	sbuf_t b;
	sb_init(&b, msg, sizeof msg);
	sb_str(&b, TR(TOAST_SENDING_TO, "Sending to "));
	sb_str(&b, name);
	ui_go_home();
	ui_toast(msg);
}

static void compose_select(uint16_t i, bool hold)
{
	(void)hold;
	if (compose_aprs()) {
		if (i == 0) {
#if CONFIG_APRS_AFSK
			ui_text_open_to(aprs_to, sizeof(aprs_to) - 1u);
#endif
			return;
		}
		i--;
	}
	if (i < CANNED_N) {
		bool aprs = compose_aprs();
		send_text(tr(canned[i]), aprs);
	}
	else if (i == CANNED_N) {
#if CONFIG_DMR_SMS || CONFIG_APRS_AFSK
		ui_text_open_message(compose_aprs());
	#endif
	} else if (compose_aprs()) {
		aprs_message_flags ^= i == CANNED_N + 1u ? APRS_MSG_ACK : APRS_MSG_POSITION;
	}
}

static const char *compose_title(void)
{
	static char t[32];
	if (compose_aprs())
		str_copy(t, sizeof t, aprs_to[0] ? aprs_to : TR(TEXT_TITLE_CALLSIGN, "Callsign"));
	else
		ui_sb_target(t, sizeof t, &ui_target);
	return t;
}

static void compose_enter(void)
{
	aprs_composing = ui_frame()->arg == 1u;
	str_copy(aprs_to, sizeof aprs_to, compose_aprs() ? aprs_compose_to : "");
	aprs_compose_to[0] = 0;
	aprs_message_flags = APRS_MSG_ACK;
}

static const ui_scr_def_t compose_screen = {
	.name = "compose", .flags = SF_LIST | SF_FIELD, .prefix = TRN(MSG_TITLE_TO, "To"),
	.hint = TRN(MSG_HINT_COMPOSE, "OK send  Back"),
	.count = compose_count, .item = compose_item, .select = compose_select, .title = compose_title, .enter = compose_enter,
};

// ======================= Contacts (type to find) =======================

#if CONFIG_DMR
static uint16_t contacts_raw_count(void) { return CODEPLUG_CONTACTS_MAX; }

static void contact_label(const contact_t *c, char *buf, uint8_t len, bool with_id)
{
	sbuf_t b;
	sb_init(&b, buf, len);
	sb_str(&b, c->name);
	if (with_id) {
		sb_chr(&b, ' ');
		sb_u32(&b, c->id, 1);	// digits find by ID too
	}
}

static bool contacts_raw_label(uint16_t raw, char *buf, uint8_t len)
{
	contact_t c;
	if (!codeplug_contact((uint16_t)(raw + 1u), &c))
		return false;
	contact_label(&c, buf, len, true);
	return true;
}

// Every contact as raw rows from first on (raw_each): one pass of block reads (codeplug_contacts_each)
typedef struct {
	ui_raw_fn fn;
	void *ctx;
	uint16_t first;
	bool with_id, stopped;
} ct_rows_t;

static bool ct_row(uint16_t index, const contact_t *c, void *p)
{
	ct_rows_t *r = p;
	char label[40];
	if (c != NULL)
		contact_label(c, label, sizeof label, r->with_id);
	r->stopped = !r->fn((uint16_t)(r->first + index - 1u), c != NULL ? label : NULL, r->ctx);
	return !r->stopped;
}

// false: fn stopped the pass. with_id: the label is the name and the ID (contacts_raw_label), else the name
static bool contacts_rows(uint16_t first, ui_raw_fn fn, void *ctx, bool with_id)
{
	ct_rows_t r = { .fn = fn, .ctx = ctx, .first = first, .with_id = with_id };
	codeplug_contacts_each(ct_row, &r);
	return !r.stopped;
}

// Contact raw + 1 (raw 0 is contact 1) as a target
static bool contact_target_raw(uint16_t raw, ui_target_t *t)
{
	contact_t c;
	if (!codeplug_contact((uint16_t)(raw + 1u), &c))
		return false;
	memset(t, 0, sizeof *t);
	t->id = c.id;
	t->group = c.type != CONTACT_PRIVATE;
	t->saved = true;
	strcpy(t->name, c.name);
	return true;
}

#if CONFIG_ZONE_SHARE || CONFIG_CRASH_REPORT	// the lists that use these: Share To, the crash report's Send To
// Share To's rows: raw rows are the contacts
static bool contacts_target(uint16_t i, ui_target_t *t)
{
	return i < ui_filter_count() && contact_target_raw(ui_filter_raw(i), t);
}
#endif

static void contact_row(const ui_target_t *tp, ui_item_t *it)
{
	const ui_target_t t = *tp;
	sbuf_t b;
	sb_init(&b, it->label, sizeof it->label);
	sbuf_t vb;
	sb_init(&vb, it->value, sizeof it->value);
	sb_str(&b, t.name[0] ? t.name : TR(VALUE_NO_NAME, "(no name)"));
	if (t.id == 0xFFFFFFu) {
		sb_str(&vb, TR(VALUE_ALL_CALL, "All call"));
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

#if CONFIG_ZONE_SHARE || CONFIG_CRASH_REPORT
static void contacts_item(uint16_t i, ui_item_t *it)
{
	ui_target_t t;
	if (contacts_target(i, &t))
		contact_row(&t, it);
}
#endif

// Contacts: with the contact editor raw row 0 is New contact, first so a long list (and a typed filter's first matches)
// keeps it in reach; the contacts follow it
#define CONTACTS_NEW	(CONFIG_CONTACT_EDIT ? 1u : 0u)

static bool contacts_is_new(uint16_t i) { return CONFIG_CONTACT_EDIT && i < ui_filter_count() && ui_filter_raw(i) == 0; }
static uint16_t contacts_list_raw_count(void) { return (uint16_t)(contacts_raw_count() + CONTACTS_NEW); }

static bool contacts_list_raw_label(uint16_t raw, char *buf, uint8_t len)
{
#if CONFIG_CONTACT_EDIT
	if (raw == 0) {
		str_copy(buf, len, TR(CONTACT_ROW_NEW, "New contact"));
		return true;
	}
#endif
	return contacts_raw_label((uint16_t)(raw - CONTACTS_NEW), buf, len);
}

static void contacts_list_raw_each(ui_raw_fn fn, void *ctx, bool labels)
{
	(void)labels;
#if CONFIG_CONTACT_EDIT
	if (!fn(0, TR(CONTACT_ROW_NEW, "New contact"), ctx))
		return;
#endif
	contacts_rows(CONTACTS_NEW, fn, ctx, true);
}

static bool contacts_list_target(uint16_t i, ui_target_t *t)
{
	if (i >= ui_filter_count() || contacts_is_new(i))
		return false;
	return contact_target_raw((uint16_t)(ui_filter_raw(i) - CONTACTS_NEW), t);
}

static void contacts_list_item(uint16_t i, ui_item_t *it)
{
	ui_target_t t;
	if (contacts_is_new(i))
		str_copy(it->label, sizeof it->label, TR(CONTACT_ROW_NEW, "New contact"));
	else if (contacts_list_target(i, &t))
		contact_row(&t, it);
}

static void contacts_select(uint16_t i, bool hold)
{
	ui_target_t t;
#if CONFIG_CONTACT_EDIT
	if (contacts_is_new(i)) {
		memset(&t, 0, sizeof t);
		t.group = true;		// the editor starts empty, on a talkgroup
		ui_new_contact(&t);
		return;
	}
#endif
	if (contacts_list_target(i, &t))
		ui_talk(&t, hold);
}

static void contacts_context(uint16_t i)
{
	ui_target_t t;
	if (contacts_list_target(i, &t))
		ui_open_actions(&t);
}

static const ui_scr_def_t contacts_screen = {
	.name = "contacts", .flags = SF_LIST | SF_FILTER | SF_FIELD, .prefix = TRN(TITLE_CONTACTS, "Contacts"),
	.hint = TRN(CONTACT_HINT_LIST, TEXT_ALT_OPEN "0-9 find  OK talk  # more"
		TEXT_ALT_NEXT "0-9  OK talk  # more" TEXT_ALT_CLOSE),
	.empty = CONFIG_CONTACT_EDIT ? TRN(CONTACT_EMPTY_NO_MATCH, "No contact matches") :
		TRN(EMPTY_NO_CONTACTS_USE_QDMR, "No contacts: add them in qdmr"),
	.count = ui_filter_count, .item = contacts_list_item, .select = contacts_select, .context = contacts_context,
	.target = contacts_list_target,
	.filter = &(const ui_filter_def_t){ .raw_count = contacts_list_raw_count, .raw_label = contacts_list_raw_label,
		.raw_each = contacts_list_raw_each },
};
#else
static uint16_t none_count(void) { return 0; }
static void none_item(uint16_t i, ui_item_t *it) { (void)i; (void)it; }
static const ui_scr_def_t contacts_screen = {
	.name = "contacts", .flags = SF_LIST | SF_FIELD, .prefix = TRN(TITLE_CONTACTS, "Contacts"),
	.hint = TRN(HINT_BACK, "Back"),
	.empty = TRN(WHY_CONTACTS_NOT_IN_BUILD, "Contacts: not in this build"), .count = none_count, .item = none_item,
};
#endif

// A page of text (About, a message): Up / Down and the knob scroll a line (the knob's forward = down, as in lists),
// up to max (ui_draw_text_page's answer); BACK leaves
static bool text_page_key(const input_event_t *ev, uint16_t *first, uint16_t max)
{
	int dir = ev->action == ACT_NAV_UP ? -1 : ev->action == ACT_NAV_DOWN ? 1 :
		ev->action == ACT_ENC_STEP ? (ev->value > 0 ? 1 : -1) : 0;
	if (dir == 0 || (ev->gesture != GESTURE_PRESS && ev->gesture != GESTURE_REPEAT && ev->gesture != GESTURE_LONG &&
			ev->action != ACT_ENC_STEP))
		return false;
	int n = ev->action == ACT_ENC_STEP ? (ev->value < 0 ? -ev->value : ev->value) : 1;
	int f = *first + dir * n;
	*first = (uint16_t)(f < 0 ? 0 : f > max ? max : f);
	ui_dirty();
	return true;
}

// ======================= Messages =======================

#define MSG_APRS_ARG	0x8000u
#define MSG_LONG_ARG	0x4000u	// the message screen shortens the text: Full message is the first row (set on entry)
#define MSG_INDEX(arg)	((uint16_t)((arg) & ~(MSG_APRS_ARG | MSG_LONG_ARG)))
_Static_assert(DMR_DATA_INBOX_SIZE <= MSG_LONG_ARG && APRS_MESSAGES <= MSG_LONG_ARG,
	       "an inbox index must stay under the arg's flag bits");

static bool aprs_new_available(void)
{
#if CONFIG_APRS_AFSK
	aprs_view_t v;
	return ui_svc_aprs_view(&v) && v.call[0] != '\0';
#else
	return false;
#endif
}

static uint16_t msgs_count(void)
{
	return ui_svc_msg_count() + ui_svc_aprs_msg_count() + (aprs_new_available() ? 1u : 0u);
}

// Both inboxes are newest first. Merge their small RAM lists by age for the shared Messages screen.
static bool msg_row(uint16_t row, bool *aprs, uint16_t *index)
{
	uint16_t d = 0, a = 0;
	uint16_t nd = ui_svc_msg_count(), na = ui_svc_aprs_msg_count();
	if (row >= nd + na)
		return false;
	for (uint16_t n = 0; n <= row; n++) {
		bool take_aprs = d == nd;
		if (!take_aprs && a < na) {
			// One message at a time: both copies at once put this list on the UI task's deepest chain
			uint32_t dmr_age;
			{
				ui_msg_t dm;
				if (!ui_svc_msg_get(d, &dm))
					return false;
				dmr_age = dm.age_ms;
			}
			aprs_msg_t am;
			if (!ui_svc_aprs_msg((uint8_t)a, &am, false))
				return false;
			take_aprs = ui_st.now_ms - am.ms < dmr_age;
		}
		*aprs = take_aprs;
		*index = take_aprs ? a++ : d++;
	}
	return true;
}

static void msgs_item(uint16_t i, ui_item_t *it)
{
	uint16_t dmr_count = ui_svc_msg_count();
	uint16_t aprs_count = ui_svc_aprs_msg_count();
	if (aprs_new_available() && i == dmr_count + aprs_count) {
		str_copy(it->label, sizeof it->label, TR(MSG_ROW_NEW_APRS, "New APRS message"));
		return;
	}
	bool aprs;
	uint16_t index;
	if (!msg_row(i, &aprs, &index))
		return;
	if (aprs) {
		aprs_msg_t m;
		if (!ui_svc_aprs_msg((uint8_t)index, &m, false))
			return;
		sbuf_t b;
		sb_init(&b, it->label, sizeof it->label);
		sb_str(&b, m.from);
		sb_str(&b, ": ");
		sb_str(&b, m.text);
		ui_sb_age(it->value, sizeof it->value, ui_st.now_ms - m.ms);
		it->flags |= UI_ITEM_PREVIEW;
		if (!m.read)
			it->flags |= UI_ITEM_WARN;
		return;
	}
	ui_msg_t m;
	if (!ui_svc_msg_get(index, &m))
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
	if (aprs_new_available() && i == ui_svc_msg_count() + ui_svc_aprs_msg_count())
		ui_aprs_compose("");
	else {
		bool aprs;
		uint16_t index;
		if (msg_row(i, &aprs, &index))
			ui_push(SCR_MESSAGE, aprs ? (uint16_t)(MSG_APRS_ARG | index) : index);
	}
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
	bool aprs;
	uint16_t index;
	if (!msg_row(i, &aprs, &index))
		return;
	if (aprs) {
		aprs_msg_t m;
		if (ui_svc_aprs_msg((uint8_t)index, &m, false))
			ui_aprs_compose(m.from);
		return;
	}
	ui_target_t t;
	msg_target_of(index, &t);
	ui_open_actions(&t);
}

static const ui_scr_def_t messages_screen = {
	.name = "messages", .flags = SF_LIST | SF_FIELD, .prefix = TRN(TITLE_MESSAGES, "Messages"),
	.hint = TRN(MSG_HINT_LIST, "OK open  # reply  Back"),
	.empty = TRN(MSG_EMPTY, "No messages yet"),
	.count = msgs_count, .item = msgs_item, .select = msgs_select, .context = msgs_context,
};

// One message: the text, then canned replies first, call, delete. A text the screen shortens gets a Full message row
// first, which shows all of it on a page of its own

enum { MV_READ, MV_REPLY_OK, MV_REPLY_WAY, MV_CALL, MV_DELETE, MV_COUNT };

static bool message_aprs(void) { return (ui_frame()->arg & MSG_APRS_ARG) != 0; }
static uint16_t message_index(void) { return MSG_INDEX(ui_frame()->arg); }
static uint8_t message_aprs_index(void) { return (uint8_t)message_index(); }

static const char *msg_title(void);
static const char *msg_body(void);

// The action of row i. Whether the Full message row is there is decided once, on entry, and kept in the frame's arg:
// decided at each draw, a row would appear or go when "9m ago" became "10m ago", and OK on Call could land on Delete
static uint8_t msg_action(uint16_t i)
{
	return (uint8_t)(i + ((ui_frame()->arg & MSG_LONG_ARG) ? MV_READ : MV_REPLY_OK));
}

static void msg_enter(void)
{
	if (message_aprs()) {
		aprs_msg_t m;
		(void)ui_svc_aprs_msg(message_aprs_index(), &m, true);
	} else {
		ui_svc_msg_read(message_index());
	}
	ui_list_t l;
	memset(&l, 0, sizeof l);
	l.prefix = TR(WORD_MESSAGE, "Message");
	l.name = msg_title();
	l.body = msg_body();
	l.count = MV_COUNT;
	if (!ui_list_body_fits(&ui_st, &l))
		ui_frame()->arg |= MSG_LONG_ARG;
}

static uint16_t msg_count(void) { return (uint16_t)(MV_COUNT - msg_action(0)); }

static void msg_item(uint16_t i, ui_item_t *it)
{
	static const tr_id_t names[] = { TRN(MSG_ROW_FULL_MESSAGE, "Full message"),
		TRN(MSG_ROW_REPLY_OK, "Reply \"OK\""), TRN(MSG_ROW_REPLY_ON_MY_WAY, "Reply \"On my way\""),
		TRN(MSG_ROW_CALL, "Call"), TRN(MSG_ROW_DELETE, "Delete") };
	uint8_t a = msg_action(i);
	str_copy(it->label, sizeof it->label, tr(names[a]));
	if ((a == MV_REPLY_OK || a == MV_REPLY_WAY) && !message_aprs() && !(feats() & UI_FEAT_SMS))
		it->flags |= UI_ITEM_DIM;
	if (a == MV_CALL && message_aprs())
		it->flags |= UI_ITEM_DIM;
}

static void msg_select(uint16_t i, bool hold)
{
	(void)hold;
	uint8_t a = msg_action(i);
	if (a == MV_READ) {
		ui_push(SCR_MESSAGE_READ, ui_frame()->arg);
		return;
	}
	i = a;
	if (!message_aprs())
		msg_target_of(message_index(), &ui_target);
	switch (i) {
	case MV_REPLY_OK:
	case MV_REPLY_WAY:
		if (message_aprs()) {
			aprs_msg_t m;
			if (ui_svc_aprs_msg(message_aprs_index(), &m, false)) {
				str_copy(aprs_to, sizeof aprs_to, m.from);
				aprs_message_flags = APRS_MSG_ACK;
				send_text(tr(i == MV_REPLY_OK ? canned[0] : canned[1]), true);
			}
		} else {
			send_text(tr(i == MV_REPLY_OK ? canned[0] : canned[1]), false);
		}
		break;
	case MV_CALL:
		if (!message_aprs())
			ui_talk(&ui_target, false);
		break;
	case MV_DELETE:
		if (message_aprs() ? ui_svc_aprs_msg_delete(message_aprs_index()) :
		    ui_svc_msg_delete(message_index())) {
			ui_pop();
			ui_toast(TR(MSG_TOAST_DELETED, "Message deleted"));
		} else {
			ui_toast(TR(MSG_TOAST_DELETE_FAILED, "Couldn't delete: try again"));
		}
		break;
	default:
		break;
	}
}

static bool msg_target(uint16_t i, ui_target_t *t)
{
	(void)i;
	if (message_aprs())
		return false;
	msg_target_of(message_index(), t);
	return t->id != 0;
}

static void msg_context(uint16_t i)
{
	(void)i;
	if (message_aprs()) {
		aprs_msg_t m;
		if (ui_svc_aprs_msg(message_aprs_index(), &m, false))
			ui_aprs_compose(m.from);
	} else {
		ui_target_t t;
		msg_target_of(message_index(), &t);
		ui_open_actions(&t);
	}
}

static const char *msg_title(void)
{
	static char t[24];
	sbuf_t b;
	sb_init(&b, t, sizeof t);
	uint16_t row = 0, index;
	bool aprs;
	while (msg_row(row, &aprs, &index) &&
	       (aprs != message_aprs() || index != message_index()))
		row++;
	sb_u32(&b, row + 1u, 1);
	sb_str(&b, TR(PART_OF, " of "));
	sb_u32(&b, ui_svc_msg_count() + ui_svc_aprs_msg_count(), 1);
	return t;
}

static const char *msg_body(void)
{
	if (message_aprs()) {
		aprs_msg_t m;
		if (!ui_svc_aprs_msg(message_aprs_index(), &m, false))
			return TR(MSG_TOAST_DELETED, "Message deleted");
		sbuf_t b;
		sb_init(&b, message_buffer, sizeof message_buffer);
		sb_str(&b, TR(MSG_BODY_FROM, "From "));
		sb_str(&b, m.from);
		sb_str(&b, ", ");
		sb_age_long(&b, ui_st.now_ms - m.ms);
		sb_chr(&b, '\n');
		sb_str(&b, m.text);
		return message_buffer;
	}
	ui_msg_t m;
	if (!ui_svc_msg_get(message_index(), &m))
		return TR(MSG_TOAST_DELETED, "Message deleted");
	// "From Joe Smith, 2m ago" (the ID where there is no name; # gives the station's actions), then the text
	sbuf_t b;
	sb_init(&b, message_buffer, sizeof message_buffer);
	sb_str(&b, m.sent ? TR(MSG_BODY_TO, "To ") : TR(MSG_BODY_FROM, "From "));
	if (m.name[0])
		sb_str(&b, m.name);
	else
		sb_u32(&b, m.src, 1);
	sb_str(&b, ", ");
	sb_age_long(&b, m.age_ms);
	sb_chr(&b, '\n');
	sb_str(&b, m.text);
	return message_buffer;
}

static const ui_scr_def_t message_screen = {
	.name = "message", .flags = SF_LIST | SF_FIELD, .prefix = TRN(WORD_MESSAGE, "Message"),
	.hint = TRN(HINT_OK_CHOOSE_MORE_BACK, "OK choose  # more  Back"),
	.count = msg_count, .item = msg_item, .select = msg_select, .context = msg_context, .target = msg_target,
	.title = msg_title, .body = msg_body, .enter = msg_enter,
};

// The whole text of a message on a page of its own (the message screen's Full message row; arg: the message screen's)
static uint16_t msg_read_first;	// first line shown
static uint16_t msg_read_max;	// the largest first line (from the last draw)

static void msg_read_enter(void)
{
	msg_read_first = 0;
}

static void msg_read_draw(void)
{
	msg_read_max = (uint16_t)ui_draw_text_page(ui_view(), &ui_st, TR(WORD_MESSAGE, "Message"), msg_body(),
		&msg_read_first, false);
}

static bool msg_read_key(const input_event_t *ev)
{
	return text_page_key(ev, &msg_read_first, msg_read_max);
}

// One "row", so PTT calls the sender as on the message screen
static uint16_t msg_read_count(void) { return 1; }

static const ui_scr_def_t message_read_screen = {
	.name = "message-read", .flags = SF_FIELD | SF_NO_STRIP, .key = msg_read_key, .draw = msg_read_draw,
	.enter = msg_read_enter, .count = msg_read_count, .target = msg_target,
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
			sb_str(&vb, r->to_us ? TR(CALL_VALUE_TO_YOU, "To you") : TR(VALUE_PRIVATE, "Private"));
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
	.name = "recent", .flags = SF_LIST | SF_FIELD, .prefix = TRN(TITLE_RECENT, "Recent"),
	.hint = TRN(RECENT_HINT, "PTT talk  # more  Back"),
	.empty = TRN(EMPTY_NO_CALLS_HEARD, "No calls heard yet"),
	.count = recent_count, .item = recent_item, .select = recent_select, .context = recent_context,
	.target = recent_target,
};

// ======================= Channels: VFO, the zones; a zone's channels =======================

#define CH_VFO	((CONFIG_VFO_FM || CONFIG_VFO_DMR) ? 1u : 0u)	// raw row 0 is the VFO when it is built

static bool chans_is_vfo(uint16_t raw) { return (CONFIG_VFO_FM || CONFIG_VFO_DMR) && raw == 0; }
// With the zone editor the last row makes a new zone (named first, then empty: its list adds channels)
static bool chans_is_new(uint16_t raw) { return CONFIG_ZONE_EDIT && raw == CH_VFO + codeplug_zone_count(); }
static uint16_t chans_raw_count(void) { return (uint16_t)(CH_VFO + codeplug_zone_count() + (CONFIG_ZONE_EDIT ? 1u : 0u)); }
#if CONFIG_ZONE_EDIT
static void zones_new(void);
#endif

static bool chans_raw_label(uint16_t raw, char *buf, uint8_t len)
{
	char name[CODEPLUG_NAME_LEN + 1];
	if (chans_is_vfo(raw)) {
		strncpy(buf, "VFO", len);
		return true;
	}
	if (chans_is_new(raw)) {
		str_copy(buf, len, TR(ZONE_ROW_CREATE, "Create New Zone"));
		return true;
	}
	if (!codeplug_zone_name((uint16_t)(raw - CH_VFO), name))
		return false;
	str_copy(buf, len, name);
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
			sb_str(&vb, TR(VALUE_ON, "On"));
		return;
	}
	if (chans_is_new(raw)) {
		str_copy(it->label, sizeof it->label, TR(ZONE_ROW_CREATE, "Create New Zone"));
		return;
	}
	uint16_t zi = (uint16_t)(raw - CH_VFO);
	if (!codeplug_zone(zi, &z))
		return;
	str_copy(it->label, sizeof it->label, z.name);
	// The zone's name whole; beside it "Here" for the zone in use and the channel count, as much as fits:
	// "Here, 16 channels", "Here, 16 ch", "Here" (or "16 channels", "16 ch", nothing)
	bool here = !v->vfo && zi == v->zone_index;
	sb_str(&vb, TEXT_ALT_OPEN);
	for (int form = 0; form < 3; form++) {
		if (form)
			sb_str(&vb, TEXT_ALT_NEXT);
		if (here)
			sb_str(&vb, form < 2 ? TR(ZONE_VALUE_HERE_AND, "Here, ") : TR(ZONE_VALUE_HERE, "Here"));
		if (form < 2) {
			sb_u32(&vb, z.count, 1);
			sb_str(&vb, form == 0 ? (z.count == 1 ? TR(UNIT_CHANNEL, " channel") :
				TR(UNIT_CHANNELS, " channels")) : TR(UNIT_CH, " ch"));
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
#if CONFIG_ZONE_EDIT
	if (chans_is_new(raw)) {
		zones_new();
		return;
	}
#endif
	if (chans_is_vfo(raw)) {
		bool was = v->vfo;	// the view may change at once (the host simulator)
		radio_req_vfo(!was);
		ui_go_home();
		ui_toast(was ? TR(VFO_TOAST_BACK_TO_CHANNELS, "Back to channels") :
			TR(ZONE_TOAST_VFO_TYPE_FREQUENCY, "VFO: type a frequency"));
		return;
	}
	uint16_t zi = (uint16_t)(raw - CH_VFO);
	if (v->vfo)
		radio_req_vfo(false);
	radio_req_zone(zi);
	ui_go_home();
	if (codeplug_zone(zi, &z)) {
		sbuf_t b;
		sb_init(&b, msg, sizeof msg);
		sb_str(&b, TR(ZONE_TOAST_ZONE, "Zone: "));
		sb_str(&b, z.name);
		ui_toast(msg);
	}
}

static void chans_context(uint16_t i)
{
	uint16_t raw = ui_filter_raw(i);
	if (chans_is_vfo(raw)) {
		ui_toast(TR(ZONE_TOAST_VFO_NO_CHANNEL_LIST, "The VFO has no channel list"));
		return;
	}
#if CONFIG_ZONE_EDIT
	if (chans_is_new(raw)) {
		zones_new();
		return;
	}
#endif
	ui_push(SCR_ZONE, (uint16_t)(raw - CH_VFO));
}

static const ui_scr_def_t channels_screen = {
	.name = "zones", .flags = SF_LIST | SF_FILTER | SF_FIELD, .prefix = TRN(TITLE_ZONES, "Zones"),
	.hint = TRN(ZONE_HINT_LIST, TEXT_ALT_OPEN "OK go  # channels  0-9 find"
		TEXT_ALT_NEXT "OK go  # channels" TEXT_ALT_CLOSE),
	.empty = TRN(ZONE_EMPTY_NO_ZONES, "No zones"),
	.count = ui_filter_count, .item = chans_item, .select = chans_select, .context = chans_context,
	.enter = chans_enter,
	.filter = &(const ui_filter_def_t){ .raw_count = chans_raw_count, .raw_label = chans_raw_label },
};

// Whether ch runs P25: a stored P25 channel, or a plain one in P25 for now (the VFO)
static bool ced_is_p25(const channel_t *ch)
{
	return CONFIG_P25 && (ch->kind == CODEPLUG_KIND_P25 || (ch->kind == CODEPLUG_KIND_NONE && ch->mode == RADIO_MODE_P25));
}

#if CONFIG_CHANNEL_LIST
// The same for NXDN
static bool ced_is_nxdn(const channel_t *ch)
{
	return CONFIG_NXDN && (ch->kind == CODEPLUG_KIND_NXDN || (ch->kind == CODEPLUG_KIND_NONE && ch->mode == RADIO_MODE_NXDN));
}
#endif

// A trunked channel whose system isn't set up (dmr-trunking-codeplug.md 4) is locked out: selecting it says why
static bool ch_locked(uint16_t num)
{
#if CONFIG_DMR_TRUNK
	static channel_t ch;
	bool usable = true;
	if (codeplug_channel(num, &ch) && ui_svc_t3_channel(num, &ch, &usable) >= 0 && !usable) {
		ui_toast(TR(TRUNK_WHY_SYSTEM_NOT_SET_UP, "System not set up"));
		return true;
	}
#else
	(void)num;
#endif
	return false;
}

// A channel row's value: its RX frequency and what it talks to, the talkgroup on DMR ("TG3113", "ID9991234" for a
// private contact), on P25 its name and talkgroup ("P25 TG201"), or "FM". The whole name comes first; where both don't
// fit beside it, the frequency and the bare number (P25: the frequency and "P25"), then the talkgroup alone (text
// alternates). A trunked channel (dmr-trunking-codeplug.md 4) names its system where the frequency would be. Returns
// false for a trunked channel whose system isn't set up: the caller dims the row
static bool sb_channel_row(sbuf_t *b, uint16_t num, const channel_t *ch)
{
	char what[16];
	uint8_t mid = 0;	// the middle form: what from here ("TG" left out), or for P25 its kind's name alone
	sbuf_t wb;
	sb_init(&wb, what, sizeof what);
	bool p25 = ced_is_p25(ch);
	// Another kind (docs/spec/channel-modes.md): its short name. A trunked channel this build runs says its talkgroup
	if (ch->kind != CODEPLUG_KIND_NONE && !p25 && !(CONFIG_DMR_TRUNK && ch->kind == CODEPLUG_KIND_DMR_TRUNK)) {
		sb_str(&wb, codeplug_kind_name(ch->kind));
	} else
#if CONFIG_DMR
	if (p25 || ch->mode == RADIO_MODE_DMR) {
		// DMR: TG 9 when the contact can't be read; P25: TG 1 with no contact (p25-codeplug.md 2)
		static contact_t c;
		uint16_t talk = p25 ? ch->contact : codeplug_channel_talk_contact(ch);
		bool known = talk != 0 && codeplug_contact(talk, &c);
		if (p25)
			sb_str(&wb, "P25 ");
		sb_str(&wb, known && c.type == CONTACT_PRIVATE ? "ID" : "TG");
		mid = 2;
		sb_u32(&wb, known ? c.id : p25 ? 1u : 9u, 1);
	} else
#endif
	if (p25) {
		sb_str(&wb, "P25");
	} else if (ch->kind != CODEPLUG_KIND_NONE) {
		sb_str(&wb, codeplug_kind_name(ch->kind));	// another kind (docs/spec/channel-modes.md): its short name
	} else {
		sb_str(&wb, ch->mode == RADIO_MODE_FM && radio_am_channel(ch) ? "AM" : ui_mode_name(ch->mode));
	}
#if CONFIG_DMR_TRUNK
	bool usable;
	int sys = ui_svc_t3_channel(num, ch, &usable);
	if (sys >= 0) {
		char name[CODEPLUG_NAME_LEN + 1];
		if (!usable) {
			sb_str(b, TR(TRUNK_WHY_SYSTEM_NOT_SET_UP, "System not set up"));
			return false;
		}
		ui_svc_t3_name((uint8_t)sys, name);
		sb_str(b, TEXT_ALT_OPEN);
		sb_str(b, name);
		sb_chr(b, ' ');
		sb_str(b, what);
		sb_str(b, TEXT_ALT_NEXT);
		sb_str(b, what);
		sb_str(b, TEXT_ALT_CLOSE);
		return true;
	}
#else
	(void)num;
#endif
	sb_str(b, TEXT_ALT_OPEN);
	sb_freq(b, ch->rx_hz);
	sb_chr(b, ' ');
	sb_str(b, what);
	if (mid) {
		sb_str(b, TEXT_ALT_NEXT);
		sb_freq(b, ch->rx_hz);
		sb_chr(b, ' ');
		if (p25)
			sb_str(b, "P25");
		else
			sb_str(b, what + mid);
	}
	sb_str(b, TEXT_ALT_NEXT);
	sb_str(b, what);
	sb_str(b, TEXT_ALT_CLOSE);
	return true;
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
static char ze_name[CODEPLUG_NAME_LEN + 1];	// a zone name being typed (Rename, Create New Zone)

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
	ui_toast(TR(TOAST_CANT_SAVE_NOW, "Couldn't save now: try again"));
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
		return TR(ZONE_ROW_ADD_CHANNEL, "Add channel");
	return raw > zone_of_frame()->count ? TR(ZONE_ROW_OPTIONS, "Zone options") : NULL;
}
#endif

static bool zone_raw_label(uint16_t raw, char *buf, uint8_t len)
{
	char name[CODEPLUG_NAME_LEN + 1];
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
	if (pos >= z->count || !codeplug_channel_name(z->channels[pos], name))
		return false;
	sb_u32(&b, pos + 1u, 1);
	sb_chr(&b, ' ');
	sb_str(&b, name);
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
static bool zones_new_pending;	// Zones focuses the zone named ze_name once its write has landed

void ui_menus_resume(void)
{
	if (ui_frame()->scr == SCR_ZONE && zone_focus_num != 0)
		zone_focus(zone_focus_num);
	if (zones_new_pending && !ui_svc_zone_pending()) {
		// A new zone takes the lowest free record, which can be ahead of others: found by its name. Once written
		// the flag goes, whatever screen is up, so a later visit to Zones isn't moved
		char name[CODEPLUG_NAME_LEN + 1];
		for (uint16_t i = 0; ui_frame()->scr == SCR_ZONES && i < ui_filter_count(); i++) {
			uint16_t raw = ui_filter_raw(i);
			if (!chans_is_vfo(raw) && !chans_is_new(raw) && chans_raw_label(raw, name, sizeof name) &&
			    strcmp(name, ze_name) == 0)
				ui_frame()->focus = i;
		}
		zones_new_pending = false;
	}
}
#endif


static void zone_item(uint16_t i, ui_item_t *it)
{
	static channel_t ch;
	const zone_t *z = zone_of_frame();
	uint16_t raw = ui_filter_raw(i);
#if CONFIG_ZONE_EDIT
	if (zone_edit_row(raw) != NULL) {
		str_copy(it->label, sizeof it->label, zone_edit_row(raw));
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
	if (!sb_channel_row(&b, z->channels[raw], &ch))
		it->flags |= UI_ITEM_DIM;	// a trunked channel whose system isn't set up: locked out
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
	const zone_t *z = zone_of_frame();
	uint16_t pos = (uint16_t)(raw - ZE_ADD);
	if (pos < z->count && ch_locked(z->channels[pos]))
		return;
	if (v->vfo)
		radio_req_vfo(false);
	if (v->zone_index != zi)
		radio_req_zone(zi);
	radio_req_zone_pos(pos);
	ui_go_home();
}

static const char *zone_title(void)
{
	return zone_of_frame()->name;
}

#if CONFIG_ZONE_SHARE || CONFIG_CONTACT_EDIT
// The toast text of Share To's sending and of the contact editor, one buffer as one toast shows at a time (the toast
// keeps the pointer). The longest: the sending toast and its narrow form, "Sending to <name> via <freq>"
static char toast_msg[84];
#endif

#if CONFIG_ZONE_SHARE
// ======================= Zone sharing (docs/spec/zone-share.md; docs/user/07-channels-zones-vfo.md) =======================
// CONTEXT in a zone: send the zone, or the channel focused, to a contact or talkgroup (with the zone editor: from
// its Channel actions and Zone options). An offer from another radio comes as a banner (CONTEXT opens it) and waits
// under Tools while it lasts: Import, Preview, Reject
// The rows that share, with the share icon in front
#define SHARE_ZONE	TR(SHARE_ROW_ZONE_TO, GLYPH_SHARE " Share zone to...")
#define SHARE_CHANNEL	TR(SHARE_ROW_CHANNEL_TO, GLYPH_SHARE " Share channel to...")

static uint16_t share_zone, share_pos;	// the zone and the channel position CONTEXT was pressed on
static bool share_one;			// send only that channel
static char share_name[CODEPLUG_NAME_LEN + 1];

// Share To for the zone, or (one) only its channel at pos; from a menu one level above the zone's list, which
// share_to_select leaves with it. Zone UI_ZONE_NONE: pos is a channel's number (the channel editor, which stays)
static void share_send_to(uint16_t zone, uint16_t pos, bool one, const char *name)
{
	if (!(feats() & UI_FEAT_ZONE_SHARE)) {
		ui_toast(TR(WHY_ZONE_SHARING_NOT_IN_BUILD, "Zone sharing: not in this build"));
		return;
	}
#if CONFIG_ZONE_EDIT
	if (ui_svc_zone_pending()) {
		// the send reads the stored zone, its positions as saved
		ui_toast(TR(SHARE_TOAST_SAVING_ZONE, "Saving the zone: try again"));
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
	str_copy(it->label, sizeof it->label, i == SH_ZONE ? SHARE_ZONE : SHARE_CHANNEL);
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
	.name = "zone_share", .flags = SF_LIST | SF_NUMBERED, .prefix = TR_NONE,
	.hint = TRN(HINT_OK_PICK_BACK, "OK pick  Back"),
	.count = share_count, .item = share_item, .select = share_select, .title = share_title,
};
#endif

// Share To: the codeplug's contacts, private or group (type to find, as Contacts). Digits that make an ID add two rows
// after them, the ID as a private call and as a talkgroup, so a station that isn't a contact can be sent to
#define SHARE_TYPED	CODEPLUG_CONTACTS_MAX	// raw rows past the contacts: the typed ID, private then talkgroup

// The digits typed as an ID, 0 when they aren't one (letters typed by name match the contacts only)
static uint32_t share_typed_id(void)
{
	const char *k = ui_filter_text();
	uint32_t id = 0;
	for (; *k; k++) {
		if (id > 1677721u)
			return 0;
		id = id * 10u + (uint32_t)(*k - '0');
	}
	return id <= 0xFFFFFEu ? id : 0;
}

static uint16_t share_raw_count(void) { return SHARE_TYPED + 2u; }

static bool share_raw_label(uint16_t raw, char *buf, uint8_t len)
{
	if (raw < SHARE_TYPED)
		return contacts_raw_label(raw, buf, len);
	uint32_t id = share_typed_id();
	if (id == 0 || (raw == SHARE_TYPED && id > CODEPLUG_DMR_ID_MAX))
		return false;
	strncpy(buf, ui_filter_text(), len - 1u);	// matches the keys typed
	buf[len - 1u] = 0;
	return true;
}

static void share_raw_each(ui_raw_fn fn, void *ctx, bool labels)
{
	(void)labels;
	if (!contacts_rows(0, fn, ctx, true))
		return;
	char label[40];
	for (uint16_t raw = SHARE_TYPED; raw < share_raw_count(); raw++) {
		if (!fn(raw, share_raw_label(raw, label, sizeof label) ? label : NULL, ctx))
			return;
	}
}

static bool share_target(uint16_t i, ui_target_t *t)
{
	if (i >= ui_filter_count())
		return false;
	uint16_t raw = ui_filter_raw(i);
	if (raw < SHARE_TYPED)
		return contacts_target(i, t);
	memset(t, 0, sizeof *t);
	t->id = share_typed_id();
	t->group = raw != SHARE_TYPED;
	return t->id != 0;
}

static void share_to_item(uint16_t i, ui_item_t *it)
{
	ui_target_t t;
	if (ui_filter_raw(i) < SHARE_TYPED) {
		contacts_item(i, it);
		return;
	}
	if (!share_target(i, &t))
		return;
	sbuf_t b;
	sb_init(&b, it->label, sizeof it->label);
	sb_str(&b, t.group ? "TG " : TR(SHARE_ROW_PRIVATE_ID, "Private ID "));
	sb_u32(&b, t.id, 1);
	str_copy(it->value, sizeof it->value,
		TR(CHIP_NOT_SAVED, TEXT_ALT_OPEN "Not saved" TEXT_ALT_NEXT TEXT_ALT_CLOSE));
}

static void share_to_select(uint16_t i, bool hold)
{
	(void)hold;
	ui_target_t t;
	uint8_t total = 0;
	const radio_view_t *v = ui_view();
	if (!share_target(i, &t))
		return;
	if (v->ch.mode != RADIO_MODE_DMR) {
		// it goes as DMR data on the channel in use (zone-share.md 7)
		ui_toast(TR(SHARE_TOAST_GO_TO_DMR_CHANNEL, "Go to a DMR channel to send"));
		return;
	}
	uint8_t n = ui_svc_zone_send(share_zone, share_pos, share_one, t.id, t.group, &total);
	char to[24], via[16];
	sbuf_t b;
	if (n == 0) {
		ui_toast(TR(TOAST_CANT_SEND_NOW, "Can't send now: try again"));
		return;
	}
	if (t.name[0] != 0)
		strcpy(to, t.name);
	else
		ui_sb_target(to, sizeof to, &t);
	// The frequency it goes on: the channel's receive frequency, which is a repeater's output
	sb_init(&b, via, sizeof via);
	sb_str(&b, TR(SHARE_TOAST_VIA, " via "));
	sb_freq(&b, v->ch.rx_hz);
	sb_init(&b, toast_msg, sizeof toast_msg);
	sb_str(&b, TEXT_ALT_OPEN);
	if (!share_one && n < total) {
		// The record holds what fits (zone-share.md 4): say how much goes
		sb_str(&b, TR(SHARE_TOAST_SENDING_COUNT, "Sending "));
		sb_u32(&b, n, 1);
		sb_str(&b, TR(PART_OF, " of "));
		sb_u32(&b, total, 1);
		sb_str(&b, TR(UNIT_CHANNELS, " channels"));
	} else {
		sb_str(&b, TR(TOAST_SENDING_TO, "Sending to "));
		sb_str(&b, to);
	}
	sb_str(&b, via);
	sb_str(&b, TEXT_ALT_NEXT);
	sb_str(&b, TR(SHARE_TOAST_SENDING, "Sending"));	// the narrow displays: where
	sb_str(&b, via);
	sb_str(&b, TEXT_ALT_CLOSE);
	ui_filter_reset();	// the keys typed here aren't the list's under it
	ui_pop();
	if (share_zone != UI_ZONE_NONE)
		ui_pop();	// and the menu it came from; the channel editor stays
	ui_toast(toast_msg);
}

static const ui_scr_def_t share_to_screen = {
	.name = "zone_share_to", .flags = SF_LIST | SF_FILTER, .prefix = TRN(SHARE_TITLE_TO, "Share To"),
	.hint = TRN(SHARE_HINT_TO, TEXT_ALT_OPEN "0-9 find or ID  OK send  Back"
		TEXT_ALT_NEXT "0-9 find or ID  OK send" TEXT_ALT_CLOSE),
	.empty = TRN(SHARE_EMPTY_TYPE_AN_ID, "No contacts: type an ID"),
	.count = ui_filter_count, .item = share_to_item, .select = share_to_select,
	.filter = &(const ui_filter_def_t){ .raw_count = share_raw_count, .raw_label = share_raw_label,
		.raw_each = share_raw_each },
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
	static const tr_id_t names[] = { TRN(SHARE_ROW_IMPORT, "Import"), TRN(SHARE_ROW_PREVIEW, "Preview"),
		TRN(SHARE_ROW_REJECT, "Reject") };
	str_copy(it->label, sizeof it->label, tr(names[i]));
	item_num(it, (uint16_t)(i + 1u));
	if (i == OF_IMPORT && offer.in_band == 0)
		it->flags |= UI_ITEM_DIM;
}

static void offer_enter(void)
{
	if (ui_st.banner.kind == UI_BANNER_ZONE_OFFER)
		ui_st.banner.kind = UI_BANNER_NONE;	// opened from Tools: the banner about it has done its job
	if (offer_get())
		ui_svc_zone_offer_open(offer.serial);	// open: it doesn't expire while the user reads it
}

static void offer_import(uint16_t zone)
{
	if (!ui_svc_zone_offer_import(offer.serial, zone)) {
		ui_toast(TR(TOAST_BUSY_SAVING, "Busy saving: try again"));
		return;
	}
	ui_go_home();
	ui_toast(TR(SHARE_TOAST_IMPORTING, "Importing..."));
}

static void offer_select(uint16_t i, bool hold)
{
	(void)hold;
	if (!offer_get())
		return;
	switch (i) {
	case OF_IMPORT:
		if (offer.in_band == 0)
			ui_toast(TR(SHARE_ERR_NO_CHANNEL_IN_BANDS, "No channel in this radio's bands"));
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
		ui_toast(TR(SHARE_TOAST_OFFER_REJECTED, "Offer rejected"));
		break;
	}
}

// "Zone Club, 24 channels, from Joe 9991234" and how many can't be received here
static const char *offer_body(void)
{
	static char body[128];
	sbuf_t b;
	sb_init(&b, body, sizeof body);
	if (!offer_get())
		return NULL;
	sb_str(&b, offer.channel ? TR(SHARE_BODY_CHANNEL, "Channel ") : TR(SHARE_BODY_ZONE, "Zone "));
	sb_str(&b, offer.name);
	if (!offer.channel) {
		sb_str(&b, ", ");
		sb_u32(&b, offer.channels, 1);
		sb_str(&b, offer.channels == 1 ? TR(UNIT_CHANNEL, " channel") : TR(UNIT_CHANNELS, " channels"));
	}
	sb_str(&b, TR(SHARE_BODY_FROM, ", from "));
	if (offer.from[0]) {
		sb_str(&b, offer.from);
		sb_chr(&b, ' ');
	}
	sb_u32(&b, offer.src, 1);
	if (offer.in_band < offer.channels) {
		// Left out by the import (the result banner says the rest come in receive only)
		sb_str(&b, ". ");
		sb_u32(&b, (uint32_t)(offer.channels - offer.in_band), 1);
		sb_str(&b, TR(SHARE_BODY_NOT_IN_BAND, " not in band"));
	}
	if (offer.unavailable != 0) {
		// Trunked channels whose system neither this radio nor the share has (zone-share.md 5.2)
		sb_str(&b, ". ");
		sb_u32(&b, offer.unavailable, 1);
		sb_str(&b, offer.unavailable == 1 ?
			   TR(SHARE_BODY_CHANNEL_NO_SYSTEM, " trunked channel unavailable: system not set up") :
			   TR(SHARE_BODY_CHANNELS_NO_SYSTEM, " trunked channels unavailable: system not set up"));
	}
	return body;
}

// The longest title that fits: a whole zone or one channel
static const char *offer_title(void)
{
	if (offer_get() && offer.channel)
		return TR(SHARE_TITLE_CHANNEL_RECEIVED, TEXT_ALT_OPEN "Channel Share Received " GLYPH_DOWNLOAD
			TEXT_ALT_NEXT "Ch Share Received "
			  GLYPH_DOWNLOAD TEXT_ALT_NEXT "Ch Received " GLYPH_DOWNLOAD TEXT_ALT_NEXT "Ch Received" TEXT_ALT_CLOSE);
	return TR(SHARE_TITLE_ZONE_RECEIVED, TEXT_ALT_OPEN "Zone Share Received " GLYPH_DOWNLOAD
		TEXT_ALT_NEXT "Zone Received " GLYPH_DOWNLOAD
		  TEXT_ALT_NEXT "Zone Received" TEXT_ALT_CLOSE);
}

static const ui_scr_def_t offer_screen = {
	.name = "zone_offer", .flags = SF_LIST | SF_NUMBERED, .hint = TRN(HINT_OK_PICK_BACK, "OK pick  Back"),
	.empty = TRN(SHARE_EMPTY_NO_SHARE_WAITING, "No share waiting"),
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
	sb_str(&b, ch.name[0] ? ch.name : TR(VALUE_NO_NAME, "(no name)"));
	sb_init(&b, it->value, sizeof it->value);
	if (!ch.in_band) {
		sb_str(&b, TR(SHARE_CHIP_NOT_IN_BAND, TEXT_ALT_OPEN "Not in band" TEXT_ALT_NEXT "Out" TEXT_ALT_CLOSE));
		it->flags |= UI_ITEM_DIM;
		return;
	}
	if (ch.unavailable) {
		sb_str(&b, TR(SHARE_VALUE_NO_SYSTEM, TEXT_ALT_OPEN "System not set up" TEXT_ALT_NEXT "No system" TEXT_ALT_CLOSE));
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
	.name = "zone_offer_preview", .flags = SF_LIST, .prefix = TR_NONE,
	.hint = TRN(SHARE_HINT_PREVIEW, "OK or Back returns"),
	.empty = TRN(SHARE_EMPTY_NO_ZONE_OFFER, "No zone offer waiting"),
	.count = preview_count, .item = preview_item, .select = preview_select, .title = preview_title,
};

// A channel offer joins a zone the user picks
static uint16_t offer_zone_count(void) { return codeplug_zone_count(); }

static void offer_zone_item(uint16_t i, ui_item_t *it)
{
	static zone_t z;
	if (!codeplug_zone(i, &z))
		return;
	str_copy(it->label, sizeof it->label, z.name);
	sbuf_t b;
	sb_init(&b, it->value, sizeof it->value);
	sb_u32(&b, z.count, 1);
	sb_str(&b, TR(SHARE_UNIT_CHANNELS, TEXT_ALT_OPEN " channels" TEXT_ALT_NEXT " ch" TEXT_ALT_CLOSE));
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
	.name = "zone_offer_zone", .flags = SF_LIST, .prefix = TRN(SHARE_TITLE_ADD_TO_ZONE, "Add to Zone"),
	.hint = TRN(SHARE_HINT_ADD_TO_ZONE, "OK add  Back"),
	.empty = TRN(ZONE_EMPTY_NO_ZONES, "No zones"),
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
#define ZONE_HINT	TRN(ZONE_HINT_CHANNELS_EDIT, TEXT_ALT_OPEN "OK go  # edit  0-9 find" TEXT_ALT_NEXT "OK go  # edit" TEXT_ALT_CLOSE)
#define ZONE_CONTEXT	zone_context
#elif CONFIG_ZONE_SHARE
#define ZONE_HINT	TRN(ZONE_HINT_CHANNELS_SHARE, TEXT_ALT_OPEN "OK go  # share  0-9 find" TEXT_ALT_NEXT "OK go  # share" TEXT_ALT_CLOSE)
#define ZONE_CONTEXT	zone_context
#else
#define ZONE_HINT	TRN(ZONE_HINT_CHANNELS, "OK go  0-9 find  Back")
#define ZONE_CONTEXT	NULL
#endif

static const ui_scr_def_t zone_screen = {
	.name = "zone", .flags = SF_LIST | SF_FILTER | SF_FIELD, .prefix = TR_NONE,
	.hint = ZONE_HINT, .empty = TRN(ZONE_EMPTY_NO_CHANNELS, "Empty zone"),
	.count = ui_filter_count, .item = zone_item, .select = zone_select, .context = ZONE_CONTEXT,
	.title = zone_title, .enter = zone_enter,
	.filter = &(const ui_filter_def_t){ .raw_count = zone_raw_count, .raw_label = zone_raw_label },
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

#if CONFIG_CHANNEL_EDIT || CONFIG_APRS_AFSK || CONFIG_DMR_SMS
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
static char text_name_buf[CODEPLUG_NAME_LEN + 1];
static char *text_buf;
static uint8_t text_len;
static int8_t text_key = -1;		// the key that typed the last character, -1 = none to cycle
static uint8_t text_idx;
static uint32_t text_ms;
enum { TEXT_UPPER, TEXT_DIGITS, TEXT_LOWER, TEXT_MODES };	// the order # cycles through
static uint8_t text_mode;

static void text_open(char *target, uint8_t max, uint8_t screen)
{
	text_target = target;
	text_done = NULL;
	text_buf = screen == SCR_MESSAGE_TEXT ? message_buffer : text_name_buf;
	text_max = screen == SCR_MESSAGE_TEXT ? max :
		(max < CODEPLUG_NAME_LEN ? max : CODEPLUG_NAME_LEN);
	ui_push(screen, 0);
}

#if CONFIG_DMR_SMS || CONFIG_APRS_AFSK
static void message_text_kept(void)
{
	if (message_buffer[0] != '\0')
		send_text(message_buffer, compose_aprs());
}

static void ui_text_open_message(bool aprs)
{
	message_buffer[0] = '\0';
	text_open(message_buffer, aprs ? APRS_MSG_TEXT_MAX : UI_MSG_TEXT_MAX, SCR_MESSAGE_TEXT);
	text_done = message_text_kept;
}
#endif

#if CONFIG_CHANNEL_EDIT
static void ui_text_open(char *target, uint8_t max) { text_open(target, max, SCR_TEXT); }
#endif
#if CONFIG_APRS_AFSK
static void ui_text_open_to(char *target, uint8_t max) { text_open(target, max, SCR_APRS_TO); }
#endif

static void text_enter(void)
{
	if (text_buf != text_target)
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
	sb_str(&b, text_len >= sizeof it->label ? text_buf + text_len - sizeof it->label + 2u : text_buf);
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
		ui_toast(TR(TEXT_TOAST_FULL, "That's the most it can hold"));
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
			if (text_target != text_buf)
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

// The title carries the mode, so it shows however long the name is: "Name Â· ABC"
static const char *text_title(void)
{
	static const char *const modes[TEXT_MODES] = { "ABC", "123", "abc" };
	return modes[text_mode];
}

#if CONFIG_CHANNEL_EDIT
static const ui_scr_def_t text_screen = {
	.name = "text", .flags = SF_LIST, .prefix = TRN(LABEL_NAME, "Name"), .title = text_title,
	.hint = TRN(TEXT_HINT_KEEP, TEXT_ALT_OPEN "Back delete  # ABC/123/abc  OK keep"
		TEXT_ALT_NEXT "Back del  # mode  OK" TEXT_ALT_CLOSE),
	.count = text_count, .item = text_item, .key = text_key_fn, .enter = text_enter,
};
#endif

#if CONFIG_APRS_AFSK
static const ui_scr_def_t aprs_to_screen = {
	.name = "aprs-to", .flags = SF_LIST, .prefix = TRN(TEXT_TITLE_CALLSIGN, "Callsign"), .title = text_title,
	.hint = TRN(TEXT_HINT_KEEP, TEXT_ALT_OPEN "Back delete  # ABC/123/abc  OK keep"
		TEXT_ALT_NEXT "Back del  # mode  OK" TEXT_ALT_CLOSE),
	.count = text_count, .item = text_item, .key = text_key_fn, .enter = text_enter,
};
#endif

#if CONFIG_DMR_SMS || CONFIG_APRS_AFSK
static const ui_scr_def_t message_text_screen = {
	.name = "message-text", .flags = SF_LIST, .prefix = TRN(WORD_MESSAGE, "Message"), .title = text_title,
	.hint = TRN(TEXT_HINT_SEND, TEXT_ALT_OPEN "Back delete  # ABC/123/abc  OK send"
		TEXT_ALT_NEXT "Back del  # mode  OK" TEXT_ALT_CLOSE),
	.count = text_count, .item = text_item, .key = text_key_fn, .enter = text_enter,
};
#endif

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

// A channel row's label, "12 Name": a search finds it by number and name
static void ch_label(uint16_t num, const char *name, char *buf, uint8_t len)
{
	sbuf_t b;
	sb_init(&b, buf, len);
	sb_u32(&b, num, 1);
	sb_chr(&b, ' ');
	sb_str(&b, name);
}

static bool chlist_raw_label(uint16_t raw, char *buf, uint8_t len)
{
	char name[CODEPLUG_NAME_LEN + 1];
	if (CHLIST_NEW && raw == 0) {
		str_copy(buf, len, TR(CHAN_ROW_NEW_FROM_CURRENT, "New from current"));
		return true;
	}
	if (!codeplug_channel_name(CHLIST_NUM(raw), name))
		return false;
	ch_label(CHLIST_NUM(raw), name, buf, len);
	return true;
}

// Every channel as raw rows, raw = number - 1 + first (raw_each): one pass of block reads of the names
// (codeplug_channel_names_each). The channels in skip have no row
typedef struct {
	ui_raw_fn fn;
	void *ctx;
	uint16_t first;
	const zone_t *skip;
} ch_rows_t;

static bool ch_row(uint16_t num, const char *name, void *p)
{
	const ch_rows_t *r = p;
	char label[40];
	if (name != NULL && r->skip != NULL && zone_list_find(r->skip, num) >= 0)
		name = NULL;
	if (name != NULL)
		ch_label(num, name, label, sizeof label);
	return r->fn((uint16_t)(r->first + num - 1u), name != NULL ? label : NULL, r->ctx);
}

static void ch_rows(uint16_t first, const zone_t *skip, ui_raw_fn fn, void *ctx, bool labels)
{
	ch_rows_t r = { .fn = fn, .ctx = ctx, .first = first, .skip = skip };
	codeplug_channel_names_each(ch_row, &r, labels);
}

static bool chlist_raw_used(uint16_t raw)
{
	return (CHLIST_NEW && raw == 0) || codeplug_channel_used(CHLIST_NUM(raw));
}

static void chlist_raw_each(ui_raw_fn fn, void *ctx, bool labels)
{
	if (CHLIST_NEW && !fn(0, TR(CHAN_ROW_NEW_FROM_CURRENT, "New from current"), ctx))
		return;
	ch_rows(CHLIST_NEW, NULL, fn, ctx, labels);
}

static void chlist_enter(void)
{
	uint16_t cur = chlist_current();
	for (uint16_t i = 0; cur != 0 && i < ui_filter_count(); i++) {
		if (CHLIST_NUM(ui_filter_raw(i)) == cur) {
			ui_frame()->focus = i;
			break;
		}
	}
}

static void chlist_item(uint16_t i, ui_item_t *it)
{
	static channel_t ch;
	if (CHLIST_NEW && ui_filter_raw(i) == 0) {
		str_copy(it->label, sizeof it->label, TR(CHAN_ROW_NEW_FROM_CURRENT, "New from current"));
		return;
	}
	uint16_t num = CHLIST_NUM(ui_filter_raw(i));
	if (!codeplug_channel(num, &ch))
		return;
	ch_label(num, ch.name, it->label, sizeof it->label);
	sbuf_t b;
	sb_init(&b, it->value, sizeof it->value);
	if (!sb_channel_row(&b, num, &ch))
		it->flags |= UI_ITEM_DIM;
	if (num == chlist_current())
		it->flags |= UI_ITEM_TEMP;	// the channel we are on
}

// Go to channel num: in the zone in use if it holds it, else the first zone that does. false: no zone holds it
static bool chlist_tune(uint16_t num)
{
	static zone_t z;
	const radio_view_t *v = ui_view();
	uint16_t zones = codeplug_zone_count();
	if (ch_locked(num))
		return false;
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
	ui_toast(TR(CHAN_TOAST_NOT_IN_ANY_ZONE, "Not in any zone: add it to one to use it"));
	return false;
}

static void chlist_select(uint16_t i, bool hold)
{
	(void)hold;
#if CONFIG_CHANNEL_EDIT
	if (ui_filter_raw(i) == 0) {
		uint16_t num = codeplug_free_channel();
		if (num == 0) {
			ui_toast(TR(TOAST_NO_FREE_CHANNEL, "No free channel"));
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
	.name = "channels", .flags = SF_LIST | SF_FILTER | SF_FIELD, .prefix = TRN(TITLE_CHANNELS, "Channels"),
	.hint = TRN(CHAN_HINT_LIST, TEXT_ALT_OPEN "OK details  # go  0-9 find"
		TEXT_ALT_NEXT "OK details  # go" TEXT_ALT_CLOSE),
	.empty = TRN(CHAN_EMPTY_NO_CHANNELS, "No channels"),
	.count = ui_filter_count, .item = chlist_item, .select = chlist_select, .context = chlist_context,
	.enter = chlist_enter,
	.filter = &(const ui_filter_def_t){ .raw_count = chlist_raw_count, .raw_label = chlist_raw_label,
		.raw_each = chlist_raw_each, .raw_used = chlist_raw_used },
};

// ======================= One channel's settings, edited in place (ui.md 2.1) =======================
// Without CONFIG_CHANNEL_EDIT the rows are read only and OK goes to the channel

enum { CE_NAME, CE_NUMBER, CE_MODE, CE_CODE, CE_RATE, CE_RX, CE_TX, CE_POWER, CE_RX_ONLY, CE_BW, CE_RX_TONE, CE_TX_TONE, CE_VOX,
       CE_BURST, CE_CC, CE_TS, CE_CONTACT, CE_LIST, CE_ID, CE_DMO, CE_BEEP, CE_ALIAS1, CE_ALIAS2, CE_TOT, CE_SCAN, CE_ECO,
       CE_ZONES, CE_GO, CE_SHARE, CE_SAVE, CE_SAVE_NEW, CE_COUNT, CE_NONE = 0xFF };

static channel_t ced_ch;		// what the screen shows (the working copy when editing)
static uint16_t ced_num;		// its number, 0 = no such channel
#if CONFIG_CHANNEL_EDIT
static uint16_t ced_src;		// the stored channel it was read from (a copy's source), 0 = none (the VFO)
#endif
#if CONFIG_P25 || CONFIG_NXDN
static ui_air_conv_t ced_air;		// a P25 or NXDN channel's own settings, read once on entry (the list redraws often)
#if CONFIG_CHANNEL_EDIT
static ui_air_conv_t ced_air_orig;	// as stored
#endif

// A P25 or NXDN channel's settings where none are stored: the default NAC both ways; NXDN at 9600 bit/s on RAN 1
static void ced_air_default(uint8_t mode)
{
	memset(&ced_air, 0, sizeof ced_air);
#if CONFIG_P25
	if (mode == RADIO_MODE_P25) {
		ced_air.p25.rx_nac = ced_air.p25.tx_nac = P25_NAC_DEFAULT;
		ced_air.p25.squelch = P25_SQUELCH_NORMAL;
	}
#endif
#if CONFIG_NXDN
	if (mode == RADIO_MODE_NXDN) {
		ced_air.nxdn.rate9600 = true;
		ced_air.nxdn.rx_ran = ced_air.nxdn.tx_ran = NXDN_CP_TX_RAN_DEFAULT;
		ced_air.nxdn.squelch = NXDN_SQUELCH_RAN;
	}
#endif
	(void)mode;
}
#endif
static uint8_t ced_rows[CE_COUNT];
static uint8_t ced_n;

#if CONFIG_CHANNEL_EDIT
#define CE_TOT_MAX	495
#define CE_TOT_STEP	15
#define CE_ID_MAX	((int32_t)CODEPLUG_DMR_ID_MAX)
#define CE_DIGITS	UI_ENTRY_MAX	// a frequency, or an ID (8 digits too)
#define CE_RAN_N	64		// NXDN RANs 0 (RX: every RAN) to 63
#define CE_NAC_DIGITS	3

// A hex digit's value ('0'-'9', 'A'-'F')
static int hex_value(char c)
{
	return c >= 'A' ? c - 'A' + 10 : c - '0';
}

static channel_t ced_orig;		// what is stored
static bool ced_new;			// not stored yet (New from current settings)
static uint8_t ced_edit = CE_NONE;	// the row whose editor is open
static int32_t ced_val;			// spinner: power level (power_count = the radio's), color code, time-out
static bool ced_typed;
static subtone_t ced_tone;		// spinner: a tone
static bool ced_typed_tone(subtone_t *t);
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
	[CE_BURST] = CE_FIELD(tone_burst),
	[CE_CC] = CE_FIELD(color_code), [CE_TS] = CE_FIELD(timeslot), [CE_CONTACT] = CE_FIELD(contact),
	[CE_LIST] = CE_FIELD(listen_list), [CE_ID] = CE_FIELD(dmr_id), [CE_DMO] = CE_FIELD(dmo),
	[CE_BEEP] = CE_FIELD(no_beep), [CE_ALIAS1] = { offsetof(channel_t, alias_tx), 1, CODEPLUG_ALIAS_TS1_TEXT },
	[CE_ALIAS2] = { offsetof(channel_t, alias_tx), 1, CODEPLUG_ALIAS_TS2_TEXT }, [CE_TOT] = CE_FIELD(tot_s),
	[CE_SCAN] = CE_FIELD(scan_skip), [CE_ECO] = CE_FIELD(no_eco),
};

// The Mode row's steps, in the order OK goes through them (channel-modes.md 2.4): the plain modes, the air modes, then
// a kind of its own (a trunked channel). Each is offered where it can run; the step back to the one the channel was
// opened with restores it as stored (its system, its NAC or RAN), so a slip never costs a channel its mode
enum { CED_FM, CED_DMR, CED_P25, CED_NXDN, CED_KIND, CED_STEPS };

static uint8_t ced_step_of(const channel_t *ch)
{
	if (ced_is_p25(ch))
		return CED_P25;
	if (ced_is_nxdn(ch))
		return CED_NXDN;
	if (ch->kind != CODEPLUG_KIND_NONE)
		return CED_KIND;
	return CONFIG_DMR && ch->mode == RADIO_MODE_DMR ? CED_DMR : CED_FM;
}

// Whether OK may step to s. A listen band is FM (AM) only. P25 and NXDN where the board runs them, as the VFO's mode
// tile (ui_svc_features), and not for Copy here: it writes through the zone job, which takes the source's tag and has
// no block of its own. A kind only back to the one it was opened with: its system can't be picked here
static bool ced_step_ok(uint8_t s, bool listen)
{
	bool orig = s == ced_step_of(&ced_orig), into = false;
#if CONFIG_ZONE_EDIT
	into = ced_into;
#endif
	switch (s) {
	case CED_FM:
		return true;
	case CED_DMR:
		return CONFIG_DMR && !listen;
	case CED_P25:
		return !listen && (orig || (!into && (feats() & UI_FEAT_P25) != 0));
	case CED_NXDN:
		return !listen && (orig || (!into && (feats() & UI_FEAT_NXDN) != 0));
	default:
		return !listen && orig;
	}
}

// c in step s: as stored for the step it was opened with, else plain FM or DMR, or a new P25 or NXDN channel with the
// default settings (Save writes its tag)
static void ced_step_set(channel_t *c, uint8_t s)
{
	if (s == ced_step_of(&ced_orig)) {
		c->kind = ced_orig.kind;
		c->system = ced_orig.system;
		c->mode = ced_orig.mode;
#if CONFIG_P25 || CONFIG_NXDN
		ced_air = ced_air_orig;
#endif
		return;
	}
	c->kind = CODEPLUG_KIND_NONE;
	c->system = CODEPLUG_SYSTEM_NONE;
	c->mode = s == CED_DMR ? RADIO_MODE_DMR : RADIO_MODE_FM;
#if CONFIG_P25
	if (s == CED_P25) {
		c->kind = CODEPLUG_KIND_P25;
		c->mode = RADIO_MODE_P25;
		ced_air_default(RADIO_MODE_P25);
	}
#endif
#if CONFIG_NXDN
	if (s == CED_NXDN) {
		c->kind = CODEPLUG_KIND_NXDN;
		c->mode = RADIO_MODE_NXDN;
		ced_air_default(RADIO_MODE_NXDN);
	}
#endif
}

#if CONFIG_P25 || CONFIG_NXDN
// The NAC or RAN row (CE_CODE) and the rate's: what the block holds for them; all of it in a mode the channel wasn't
// stored in
static bool ced_air_changed(uint8_t r)
{
	if (ced_step_of(&ced_ch) != ced_step_of(&ced_orig))
		return true;
	const ui_air_conv_t *a = &ced_air, *b = &ced_air_orig;
#if CONFIG_P25
	if (ced_is_p25(&ced_ch))
		return r == CE_CODE && (a->p25.rx_nac != b->p25.rx_nac || a->p25.tx_nac != b->p25.tx_nac);
#endif
#if CONFIG_NXDN
	if (ced_is_nxdn(&ced_ch))
		return r == CE_CODE ? a->nxdn.rx_ran != b->nxdn.rx_ran || a->nxdn.tx_ran != b->nxdn.tx_ran :
		       a->nxdn.rate9600 != b->nxdn.rate9600;
#endif
	(void)a;
	(void)b;
	(void)r;
	return false;
}
#endif

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
#if CONFIG_P25 || CONFIG_NXDN
	if (r == CE_CODE || r == CE_RATE)
		return ced_air_changed(r);
#endif
	const uint8_t *a = (const uint8_t *)&ced_ch + ced_fields[r].off, *b = (const uint8_t *)&ced_orig + ced_fields[r].off;
	// The Mode row shows the kind: a kind's channel made plain is a change also where the record's mode is the same
	if (r == CE_MODE && (ced_ch.kind != ced_orig.kind || ced_ch.system != ced_orig.system))
		return true;
	if (r == CE_NAME)
		return strcmp((const char *)a, (const char *)b) != 0;	// the bytes past the end may differ
	if (r == CE_RX_TONE || r == CE_TX_TONE)
		return !tone_same((const subtone_t *)a, (const subtone_t *)b);
	if (ced_fields[r].bit != 0)
		return ((*a ^ *b) & ced_fields[r].bit) != 0;
	return memcmp(a, b, ced_fields[r].len) != 0;
}

// RX in a receive-only band (the air band, broadcast FM: docs/spec/airband-and-broadcast.md 2): the copy becomes RX
// only, and FM (AM there) in a listen band, which is all the radio does there: a P25 or NXDN channel becomes a plain
// one (the radio core refuses those modes there), and Save asks before a stored one loses its tag
static void ced_rx_band(channel_t *c)
{
	if (!board_in_band(c->rx_hz, false) || board_in_band(c->rx_hz, true))
		return;
	c->rx_only = true;
	c->tx_hz = c->rx_hz;
	if (radio_listen_band(c->rx_hz)) {
		if (ced_is_p25(c) || ced_is_nxdn(c)) {
			c->kind = CODEPLUG_KIND_NONE;
			c->system = CODEPLUG_SYSTEM_NONE;
		}
		c->mode = RADIO_MODE_FM;
	}
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
	ced_rx_band(&ced_ch);
}
#endif

// ced_air for ch: a stored channel's block, read at number (before a copy's rename: the tag is the source's name's); a
// plain channel in P25 or NXDN, the radio's settings from its view
static void ced_air_read(uint16_t number, const channel_t *ch)
{
#if CONFIG_P25 || CONFIG_NXDN
	const radio_view_t *v = ui_view();
	bool here = ch->kind == CODEPLUG_KIND_NONE && (v->vfo ? number == 0 : v->channel_number == number);
	memset(&ced_air, 0, sizeof ced_air);
#if CONFIG_P25
	if (ced_is_p25(ch)) {
		p25_conv_t *c = &ced_air.p25;
		if (v->ch.mode == RADIO_MODE_P25 && here) {
			// The view has the NAC received only: one NAC both ways (any NAC: the default sent)
			c->rx_nac = v->p25.rx_nac;
			c->tx_nac = c->rx_nac == P25_NAC_ANY ? P25_NAC_DEFAULT : c->rx_nac;
		} else if (!p25_conv_params(number, ch, c)) {
			ced_air_default(RADIO_MODE_P25);
		}
	}
#endif
#if CONFIG_NXDN
	if (ced_is_nxdn(ch)) {
		nxdn_conv_t *c = &ced_air.nxdn;
		static nxdn_conv_cfg_t cfg;	// static: not on the UI task's stack
		if (v->ch.mode == RADIO_MODE_NXDN && here) {
			c->rate9600 = (v->nxdn.flags & RADIO_NXDN_9600) != 0;
			c->rx_ran = v->nxdn.rx_ran;
			c->tx_ran = v->nxdn.tx_ran;
		} else if (nxdn_conv_load(number, ch, &cfg)) {
			*c = cfg.conv;
		} else {
			ced_air_default(RADIO_MODE_NXDN);
		}
	}
#endif
#if CONFIG_CHANNEL_EDIT
	ced_air_orig = ced_air;
#endif
#else
	(void)number;
	(void)ch;
#endif
}

static void ced_build(void)
{
	bool listen = radio_listen_band(ced_ch.rx_hz);
	bool p25 = ced_is_p25(&ced_ch), nxdn = ced_is_nxdn(&ced_ch), air = p25 || nxdn;
	bool dmr = CONFIG_DMR && ced_ch.mode == RADIO_MODE_DMR && !listen && !air;
	bool edit = CONFIG_CHANNEL_EDIT;
	bool fresh = false, changed = false, into = false;
#if CONFIG_CHANNEL_EDIT
	fresh = ced_new;
	changed = ced_changed(CE_COUNT);
#endif
#if CONFIG_ZONE_EDIT
	into = ced_into;	// Copy here takes the source's tag as it is: its block is changed after
#endif
	ced_n = 0;
	for (uint8_t r = 0; r < CE_COUNT; r++) {
		bool fm_only = r == CE_BW || r == CE_RX_TONE || r == CE_TX_TONE || r == CE_VOX || r == CE_BURST;
		bool dmr_only = r >= CE_CC && r <= CE_ALIAS2;
		bool tone = r == CE_RX_TONE || r == CE_TX_TONE;
		// P25 and NXDN: no sub-audio tones or width; of the digital rows the talk target and unit ID, P25's
		// talkaround; the NAC or RAN, NXDN's rate
		bool air_row = air && ((r == CE_CONTACT && CONFIG_DMR) || r == CE_ID || (r == CE_DMO && p25) ||
				       (r == CE_CODE && !into) || (r == CE_RATE && nxdn && !into));
		if (r == CE_CODE || r == CE_RATE ? !air_row : air && (fm_only || (dmr_only && !air_row)))
			continue;
		if (!air_row && ((fm_only && dmr) || (tone && listen) || (dmr_only && !dmr) || (r == CE_MODE && !CONFIG_DMR)))
			continue;
		// The radio core skips the burst on a board with no TX tone, so no row for it there (a host board
		// without a radio keeps the row)
		if (r == CE_BURST && (!CONFIG_FM_TX_TONES || (board.radio != NULL && board.radio->tx_tone == NULL)))
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
		ced_src = src;
		ced_air_read(src, &ced_ch);
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
		const radio_view_t *cv = ui_view();
		ced_src = cv->vfo ? 0 : cv->channel_number;
		ced_from_view();
		ced_air_read(ced_src, &cv->ch);
		ced_orig = ced_ch;
		return;
	}
	ced_src = ced_num;
#endif
	if (!codeplug_channel(ced_num, &ced_ch))
		ced_num = 0;
	ced_air_read(ced_num, &ced_ch);
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
	return ced_ch.name[0] != 0 ? ced_ch.name : TR(CHAN_TITLE_NEW, "New Channel");
}

static zone_t chz_z;	// a zone read to find a channel in it (the Zones row, the channel editor's Zones)

// The zones holding the channel, as much as fits: "Fav, Local, Ga" / "Fav +2" / "3 zones" (text alternates)
static void sb_chinfo_zones(sbuf_t *b, uint16_t num)
{
	char all[24], first[CODEPLUG_NAME_LEN + 1] = "";
	sbuf_t ab;
	sb_init(&ab, all, sizeof all);
	uint16_t found = 0;
	for (uint16_t zi = 0; zi < codeplug_zone_count(); zi++) {
		if (!codeplug_zone(zi, &chz_z))
			continue;
		for (uint16_t pos = 0; pos < chz_z.count; pos++) {
			if (chz_z.channels[pos] != num)
				continue;
			if (found++)
				sb_str(&ab, ", ");
			else
				str_copy(first, sizeof first, chz_z.name);
			sb_str(&ab, chz_z.name);
			break;
		}
	}
	if (found == 0) {
		sb_str(b, TR(VALUE_NONE, "None"));
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
	sb_str(b, found == 1 ? TR(CHAN_UNIT_ZONE, " zone") : TR(CHAN_UNIT_ZONES, " zones"));
	sb_str(b, TEXT_ALT_CLOSE);
}

static void sb_onoff(sbuf_t *b, bool on)
{
	sb_str(b, on ? TR(VALUE_ON, "On") : TR(VALUE_OFF, "Off"));
}

static void sb_ced_power(sbuf_t *b, int32_t level, uint32_t tx_hz)
{
	sb_str(b, level < 0 || level >= board.power_count ? TR(CHAN_VALUE_RADIO_DEFAULT, "Radio's") :
		ui_power_name((uint8_t)level, tx_hz));
}

static void sb_ced_tot(sbuf_t *b, int32_t s)
{
	if (s == 0) {
		sb_str(b, TR(CHAN_VALUE_RADIO_DEFAULT, "Radio's"));
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

#if CONFIG_P25 || CONFIG_NXDN
// The NAC (hex) or RAN, received then sent where they differ ("F7E/293"); editing, the value on the spinner
static void sb_ced_code(sbuf_t *b, const channel_t *ch, bool editing)
{
	uint16_t rx = 0, tx = 0;
	bool hex = ced_is_p25(ch);
#if CONFIG_P25
	if (hex) {
		rx = ced_air.p25.rx_nac;
		tx = ced_air.p25.tx_nac;
	}
#endif
#if CONFIG_NXDN
	if (!hex) {
		rx = ced_air.nxdn.rx_ran;
		tx = ced_air.nxdn.tx_ran;
	}
#endif
#if CONFIG_CHANNEL_EDIT
	if (editing)
		rx = tx = (uint16_t)ced_val;
#else
	(void)editing;
#endif
	for (uint8_t k = 0; k < 2; k++) {
		uint16_t x = k ? tx : rx;
		if (hex)
			sb_hex(b, x, 3);
		else
			sb_u32(b, x, 1);
		if (rx == tx)
			break;
		if (k == 0)
			sb_chr(b, '/');
	}
}
#endif

static void chinfo_item(uint16_t i, ui_item_t *it)
{
	static const tr_id_t labels[CE_COUNT] = { TRN(LABEL_NAME, "Name"), TRN(CHAN_LABEL_NUMBER, "Number"),
		TRN(LABEL_MODE, "Mode"), TRX(CHAN_LABEL_NAC, "NAC"), TRN(CHAN_LABEL_RATE, "Rate"), TRX(CHAN_LABEL_RX, "RX"), TRX(CHAN_LABEL_TX, "TX"), TRN(LABEL_POWER, "Power"),
		TRN(WORD_RX_ONLY, "RX only"), TRN(CHAN_LABEL_BANDWIDTH, "Bandwidth"),
		TRN(CHAN_LABEL_RX_TONE, "RX CTCSS/DCS"), TRN(CHAN_LABEL_TX_TONE, "TX CTCSS/DCS"), TRX(WORD_VOX, "VOX"),
		TRN(WORD_TONE_BURST, "Tone burst"), TRN(CHAN_LABEL_COLOR_CODE, "Color code"),
		TRN(LABEL_TIMESLOT, "Timeslot"), TRN(CHAN_LABEL_TX_CONTACT, "TX contact"),
		TRN(CHAN_LABEL_RX_GROUP_LIST, "RX group list"), TRN(CHAN_LABEL_OWN_ID, "Own ID"),
		TRN(CHAN_LABEL_DIRECT_MODE, "Direct mode"), TRN(CHAN_LABEL_TALK_PERMIT_BEEPS, "Talk permit beeps"),
		TRN(CHAN_LABEL_TALKER_ALIAS_TS1, "Talker alias TS1"),
		TRN(CHAN_LABEL_TALKER_ALIAS_TS2, "Talker alias TS2"), TRN(CHAN_LABEL_TX_TIMEOUT, "TX time-out"),
		TRN(WORD_SCAN, "Scan"), TRN(LABEL_BATTERY_SAVER, "Battery saver"), TRN(TITLE_ZONES, "Zones"),
		TRN(CHAN_ROW_TUNE_TO, "Tune to this ch"), TRN(SHARE_ROW_CHANNEL_TO, GLYPH_SHARE " Share channel to..."),
		TRN(ROW_SAVE, "Save"), TRN(CHAN_ROW_SAVE_AS_NEW, "Save as new channel") };
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
	str_copy(it->label, sizeof it->label, r == CE_DMO && ced_is_p25(ch) ? TR(CHAN_LABEL_TALKAROUND, "Talkaround") :
		r == CE_CODE && ced_is_nxdn(ch) ? "RAN" : tr(labels[r]));
#if CONFIG_ZONE_SHARE
	if (r == CE_SHARE && !(feats() & UI_FEAT_ZONE_SHARE))
		it->flags |= UI_ITEM_DIM;
#endif
	sbuf_t b;
	sb_init(&b, it->value, sizeof it->value);
#if CONFIG_CHANNEL_EDIT
	if (editing && (r == CE_RX || r == CE_TX || r == CE_ID || (r == CE_CODE && ced_is_p25(ch) && ced_len > 0))) {
		// What has been typed so far: "446.07_", "3101_"
		for (uint8_t k = 0; k < ced_len; k++) {
			if ((r == CE_RX || r == CE_TX) && k == UI_ENTRY_MHZ)
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
		// What the radio does with it: a listen band is AM or FM whatever the channel says
		if (ced_is_p25(ch))
			sb_str(&b, "P25");
		else if (ced_is_nxdn(ch))
			sb_str(&b, "NXDN");
		else if (ch->kind != CODEPLUG_KIND_NONE)
			sb_str(&b, codeplug_kind_name(ch->kind));
		else if (radio_am_tx_refused(ch->rx_hz))
			sb_str(&b, "AM");
		else
			sb_str(&b, ch->mode == RADIO_MODE_DMR && !radio_listen_band(ch->rx_hz) ? "DMR" : "FM");
		break;
#if CONFIG_P25 || CONFIG_NXDN
	case CE_CODE:
		sb_ced_code(&b, ch, editing);
		break;
	case CE_RATE:
#if CONFIG_NXDN
		sb_str(&b, ced_air.nxdn.rate9600 ? "9600 bit/s" : "4800 bit/s");
#endif
		break;
#endif
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
			sb_ced_power(&b, ced_val, ch->tx_hz);
			break;
		}
#endif
		sb_ced_power(&b, ch->power == CODEPLUG_POWER_DEFAULT ? -1 : ch->power, ch->tx_hz);
		break;
	case CE_RX_ONLY:
		sb_str(&b, ch->rx_only ? TR(VALUE_YES, "Yes") : TR(VALUE_NO, "No"));
		break;
	case CE_BW:
		sb_str(&b, ch->bw == RADIO_BW_25K ? "25 kHz" : "12.5 kHz");
		break;
	case CE_RX_TONE:
	case CE_TX_TONE: {
		const subtone_t *t = r == CE_RX_TONE ? &ch->rx_tone : &ch->tx_tone;
#if CONFIG_CHANNEL_EDIT
		if (editing) {
			static subtone_t typed;	// static: the UI task's stack has 8 bytes to spare
			t = &ced_tone;
			if (ced_typed_tone(&typed)) {
				t = &typed;
				if (typed.type == TONE_DCS) {	// the digits as typed: "D02"
					sb_chr(&b, 'D');
					sb_str(&b, ced_digits + 1);
					break;
				}
			}
		}
#endif
		sb_tone(&b, t);
		break;
	}
	case CE_VOX:
		sb_onoff(&b, ch->vox);
		break;
	case CE_BURST:
		sb_onoff(&b, ch->tone_burst);
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
			sb_str(&b, TR(VALUE_NONE, "None"));
		break;
	}
	case CE_LIST: {
		static listen_list_t l;
		if (ch->listen_list != 0 && codeplug_listen_list(ch->listen_list, &l))
			sb_str(&b, l.name);
		else
			sb_str(&b, TR(VALUE_NONE, "None"));
		break;
	}
#endif
	case CE_ID:
		if (ch->dmr_id != 0)
			sb_u32(&b, ch->dmr_id, 1);
		else
			sb_str(&b, TR(CHAN_VALUE_RADIO_DEFAULT, "Radio's"));
		break;
	case CE_DMO:
		// P25 talkaround: on, or automatic (on where RX = TX; p25-codeplug.md 2.2)
		if (ced_is_p25(ch) && !ch->dmo)
			sb_str(&b, TR(VALUE_AUTO, "Auto"));
		else
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
		sb_str(&b, ch->scan_skip ? TR(CHAN_VALUE_SCAN_SKIPPED, "Skipped") : TR(CHAN_VALUE_SCANNED, "Scanned"));
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
static bool discard_contact;	// Unsaved Changes asks about the contact editor's new contact (else a channel)
static bool discard_kind;	// ... or before a save that takes a stored channel's own mode away (its tag)
static bool ced_drop_ok;	// the answer was yes

// The first problem that keeps the copy from being saved, as a toast; NULL when it can be saved
static const char *ced_problem(void)
{
	const channel_t *c = &ced_ch;
	if (c->name[0] == 0)
		return TR(TOAST_NAME_FIRST, "Give it a name first");
	if (!board_in_band(c->rx_hz, false))
		return TR(CHAN_WHY_RX_OUTSIDE_BANDS, "RX is outside the radio's bands");
	if (!c->rx_only && !board_in_band(c->tx_hz, true))
		return TR(CHAN_WHY_TX_NOT_ALLOWED, "TX can't transmit there: set RX only");
	if (CONFIG_DMR && c->mode == RADIO_MODE_DMR && radio_listen_band(c->rx_hz))
		return TR(CHAN_WHY_RX_ONLY_BAND_SET_FM, "Receive only band: set FM");
	// The contact is whom PTT calls, so a channel that never transmits needs none
	// (P25 and NXDN: none is talk group 1)
	if (CONFIG_DMR && c->mode == RADIO_MODE_DMR && !c->rx_only && c->contact == 0 && !ced_is_p25(c) && !ced_is_nxdn(c))
		return TR(CHAN_WHY_PICK_TX_CONTACT, "Pick a TX contact first");
#if CONFIG_NXDN
	// A DMR channel's own ID kept when the Mode row made it NXDN: past NXDN's range, as the ID row refuses it
	if (ced_is_nxdn(c) && c->dmr_id > NXDN_CP_UNIT_MAX)
		return TR(CHAN_TOAST_NXDN_ID_MAX, "NXDN IDs go up to 65519");
#endif
	return NULL;
}

// Write the copy: as a channel, or for Copy here as a channel put in its zone in the same job (ZONE_OP_ADD_NEW)
static bool ced_write(uint16_t num, bool as_new)
{
#if CONFIG_ZONE_EDIT
	if (ced_into && ced_new && !as_new) {
		ze_op_new(ZONE_OP_ADD_NEW, ced_into_pos)->channel = num;
		ze_op.ch = ced_ch;
		ze_op.from = ced_src;
		if (!ui_svc_zone_edit(&ze_op))
			return false;
		ced_into = false;
		zone_focus_num = num;	// the zone's list shows it when the editor closes
		return true;
	}
#endif
	(void)as_new;
#if CONFIG_P25 || CONFIG_NXDN
	// A changed NAC, RAN or rate goes with the channel, as does the whole block of one the Mode row made P25 or
	// NXDN; unchanged, the tag is written as without the editor (a plain channel in P25 or NXDN takes the radio's
	// settings, a copy its source's tag)
	if ((ced_is_p25(&ced_ch) || ced_is_nxdn(&ced_ch)) && (ced_air_changed(CE_CODE) || ced_air_changed(CE_RATE)))
		return ui_svc_write_channel_air(num, &ced_ch, ced_src, &ced_air);
#endif
	return ui_svc_write_channel(num, &ced_ch, ced_src);
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
		ui_toast(TR(CHAN_TOAST_NO_CHANGES, "No changes to save"));
		return;
	}
	uint16_t num = as_new ? codeplug_free_channel() : ced_num;
	if (num == 0) {
		ui_toast(TR(TOAST_NO_FREE_CHANNEL, "No free channel"));
		return;
	}
	// Writing a stored channel of a kind as a plain one or one of another kind replaces its tag: the system it names,
	// its NAC or RAN, its fallback. Never without asking (a new channel takes nothing away)
	if (!as_new && !ced_new && ced_orig.kind != CODEPLUG_KIND_NONE && ced_ch.kind != ced_orig.kind && !ced_drop_ok) {
		discard_contact = false;
		discard_kind = true;
		ui_push(SCR_DISCARD, 0);
		return;
	}
	if (!ced_write(num, as_new)) {
		ui_toast(TR(TOAST_CANT_SAVE_NOW, "Couldn't save now: try again"));
		return;
	}
	ced_num = num;
	ced_src = num;
	ui_frame()->arg = num;
	ced_orig = ced_ch;
#if CONFIG_P25 || CONFIG_NXDN
	ced_air_orig = ced_air;
#endif
	sbuf_t b;
	sb_init(&b, msg, sizeof msg);
	sb_str(&b, as_new || ced_new ? TR(CHAN_TOAST_SAVED_AS, "Saved as channel ") :
		TR(CHAN_TOAST_SAVED, "Saved channel "));
	sb_u32(&b, num, 1);
	ced_new = false;
	ui_toast(msg);
}

// The tone typed so far. CTCSS in Hz with the last digit the tenths ("885" 88.5); under 60 Hz it reads as whole Hz
// ("67" 67.0, "100" 100.0). A leading 0 types a DCS code ("023" D023N). False: nothing typed
static bool ced_typed_tone(subtone_t *t)
{
	if (ced_len == 0)
		return false;
	uint16_t v = 0;
	if (ced_digits[0] == '0') {
		for (uint8_t i = 1; i < ced_len; i++)
			v = (uint16_t)(v * 8u + (uint16_t)(ced_digits[i] - '0'));
		*t = (subtone_t){ TONE_DCS, v };
	} else {
		for (uint8_t i = 0; i < ced_len; i++)
			v = (uint16_t)(v * 10u + (uint16_t)(ced_digits[i] - '0'));
		*t = (subtone_t){ TONE_CTCSS, v < 600u ? (uint16_t)(v * 10u) : v };
	}
	return true;
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
	case CE_CODE:
		if (!ced_is_p25(&ced_ch)) {
			ced_val = (ced_val + dir + CE_RAN_N) % CE_RAN_N;
		} else if (ced_len > 0) {
			// The last hex digit typed steps through 0-F: the keys type 0-9 only
			char *d = &ced_digits[ced_len - 1];
			int n = (hex_value(*d) + dir + 16) % 16;
			*d = (char)(n < 10 ? '0' + n : 'A' + n - 10);
		} else {
			ced_val = (ced_val + dir) & 0xFFF;
		}
		break;
	case CE_TOT:
		ced_val += dir * CE_TOT_STEP;
		ced_val = ced_val < 0 ? 0 : ced_val > CE_TOT_MAX ? CE_TOT_MAX : ced_val;
		break;
	case CE_RX_TONE:
	case CE_TX_TONE:
		if (ced_len > 0)
			(void)ced_typed_tone(&ced_tone);	// steps on from what was typed
		ced_len = 0;
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
			ui_toast(TR(TOAST_ID_MAX_PRIVATE, "IDs go up to 16776415"));
			break;
		}
		ced_val = v;
		ced_digits[ced_len++] = d;
		ced_digits[ced_len] = 0;
		break;
	case CE_RX_TONE:
	case CE_TX_TONE:
		if (ced_len >= 4)
			break;		// 4 digits: "1318", "0754"
		if (ced_len > 0 && ced_digits[0] == '0' && d > '7') {
			ui_toast(TR(CHAN_TOAST_DCS_DIGITS, "DCS codes use digits 0-7"));
			break;
		}
		ced_digits[ced_len++] = d;
		ced_digits[ced_len] = 0;
		break;
	case CE_CODE:
		if (ced_is_p25(&ced_ch)) {
			if (ced_len >= CE_NAC_DIGITS)
				ced_len = 0;	// a fourth digit starts over
			ced_digits[ced_len++] = d;
			ced_digits[ced_len] = 0;
			break;
		}
		// An NXDN RAN is typed as a color code
		// fall through
	case CE_CC:
	case CE_TOT: {
		int32_t hi = ced_edit == CE_CC ? 15 : ced_edit == CE_CODE ? CE_RAN_N - 1 : CE_TOT_MAX;
		v = ced_typed ? v : d - '0';
		ced_val = v > hi ? d - '0' : v;
		ced_typed = true;
		break;
	}
	default:
		break;
	}
}

#if CONFIG_P25 || CONFIG_NXDN
// The NAC or RAN into the block, both ways; the NAC that takes every NAC (F7E) and RAN 0 for receiving only. False:
// not one a radio uses (a toast says so)
static bool ced_commit_code(void)
{
	uint16_t v = (uint16_t)ced_val;
#if CONFIG_P25
	if (ced_is_p25(&ced_ch)) {
		if (ced_len > 0) {
			v = 0;
			for (uint8_t k = 0; k < ced_len; k++)
				v = (uint16_t)(v * 16u + (uint16_t)hex_value(ced_digits[k]));
		}
		if (v == P25_NAC_REPEAT_ANY) {
			ui_toast(TR(CHAN_TOAST_NAC_REPEATER, "F7F is for repeaters"));
			return false;
		}
		ced_air.p25.rx_nac = v;
		if (v != P25_NAC_ANY)
			ced_air.p25.tx_nac = v;
		ced_len = 0;
		return true;
	}
#endif
#if CONFIG_NXDN
	ced_air.nxdn.rx_ran = (uint8_t)v;
	if (v != 0)
		ced_air.nxdn.tx_ran = (uint8_t)v;
#endif
	return true;
}
#endif

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
			// TX moves with RX, keeping a repeater's offset within one transmit band; a move to another band (or a
			// channel with no TX frequency) makes TX RX
			uint32_t tx = c->tx_hz - c->rx_hz + hz;
			c->tx_hz = board_offset_carries(c->rx_hz, hz, tx) ? tx : hz;
			c->rx_hz = hz;
			ced_rx_band(c);
		} else {
			c->tx_hz = hz;
		}
		break;
	}
	case CE_ID:
#if CONFIG_NXDN
		if (ced_len > 0 && ced_is_nxdn(c) && ced_val > (int32_t)NXDN_CP_UNIT_MAX) {
			ui_toast(TR(CHAN_TOAST_NXDN_ID_MAX, "NXDN IDs go up to 65519"));
			return;
		}
#endif
		if (ced_len > 0)
			c->dmr_id = (uint32_t)ced_val;	// 0 = the radio's
		break;
#if CONFIG_P25 || CONFIG_NXDN
	case CE_CODE:
		if (!ced_commit_code())
			return;
		break;
#endif
	case CE_POWER:
		c->power = ced_val >= board.power_count ? CODEPLUG_POWER_DEFAULT : (uint8_t)ced_val;
		break;
	case CE_CC:
		c->color_code = (uint8_t)ced_val;
		break;
	case CE_TOT:
		if (ced_val % CE_TOT_STEP != 0) {
			ui_toast(TR(CHAN_TOAST_TIMEOUT_RANGE, "Not changed: 0 to 495, steps of 15"));
			break;
		}
		c->tot_s = (uint16_t)ced_val;
		break;
	case CE_RX_TONE:
	case CE_TX_TONE:
		if (ced_typed_tone(&ced_tone)) {
			if (ced_tone.type == TONE_DCS ? !tones_standard(&ced_tone) : !tones_valid(&ced_tone)) {
				ui_toast(ced_tone.type == TONE_DCS ? TR(CHAN_TOAST_NOT_DCS_CODE, "Not a DCS code") :
					TR(CHAN_TOAST_CTCSS_RANGE, "CTCSS goes 60.0 to 260.0"));
				return;
			}
			ced_len = 0;
		}
		*(ced_edit == CE_RX_TONE ? &c->rx_tone : &c->tx_tone) = ced_tone;
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
			discard_contact = false;
			discard_kind = false;
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
		if ((ced_edit == CE_RX || ced_edit == CE_TX || ced_edit == CE_ID || ced_edit == CE_RX_TONE ||
		     ced_edit == CE_TX_TONE || (ced_edit == CE_CODE && ced_is_p25(&ced_ch))) && ced_len > 0) {
			ced_digits[--ced_len] = 0;	// takes the last digit back
			if (ced_edit != CE_CODE)
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
			// the send reads the stored channel
			ui_toast(TR(CHAN_TOAST_SAVE_FIRST, "Save first: hold OK"));
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
	case CE_MODE: {
		// The next step this channel can take (ced_step_ok). A stored channel of a kind left for another mode loses
		// its tag when it is saved (channel-modes.md 2.4), after Save asks. In a listen band FM is all there is:
		// AM in the air band, FM on the broadcast receiver
		bool listen = radio_listen_band(c->rx_hz);
		uint8_t s = ced_step_of(c);
		do
			s = (uint8_t)((s + 1) % CED_STEPS);
		while (!ced_step_ok(s, listen));
		if (listen && ced_step_of(c) <= CED_DMR)	// a plain channel: OK can't change it there
			ui_toast(radio_am_tx_refused(c->rx_hz) ? TR(WHY_AIR_BAND_RX_ONLY, "Air band: receive only") :
				TR(WHY_BROADCAST_RX_ONLY, "Broadcast: receive only"));
		ced_step_set(c, s);
		break;
	}
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
			ui_toast(TR(WHY_TONES_NOT_IN_BUILD, "Tones: not in this build"));
			break;
		}
		ced_edit = r;
		ced_tone = r == CE_RX_TONE ? c->rx_tone : c->tx_tone;
		ced_len = 0;
		break;
	case CE_VOX: c->vox = !c->vox; break;
	case CE_BURST: c->tone_burst = !c->tone_burst; break;
	case CE_CC:
	case CE_TOT:
		ced_edit = r;
		ced_val = r == CE_CC ? c->color_code : c->tot_s;
		ced_typed = false;
		break;
#if CONFIG_P25 || CONFIG_NXDN
	case CE_CODE:
		ced_edit = r;
		ced_len = 0;
		ced_typed = false;
#if CONFIG_P25
		if (ced_is_p25(c))
			ced_val = ced_air.p25.rx_nac;
#endif
#if CONFIG_NXDN
		if (ced_is_nxdn(c))
			ced_val = ced_air.nxdn.rx_ran;
#endif
		break;
#if CONFIG_NXDN
	case CE_RATE: ced_air.nxdn.rate9600 = !ced_air.nxdn.rate9600; break;
#endif
#endif
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
	case CE_NUMBER: ui_toast(TR(CHAN_TOAST_NUMBER_IS_FIXED, "Save as new channel to use another number")); break;
#if CONFIG_ZONE_EDIT
	case CE_ZONES: ui_push(SCR_CHZONES, ced_num); break;
#else
	case CE_ZONES: ui_toast(TR(CHAN_TOAST_ZONES_USE_QDMR, "Zones: change them with qdmr for now")); break;
#endif
	case CE_GO:
		if (ced_changed(CE_COUNT))
			ui_toast(TR(CHAN_TOAST_SAVE_FIRST, "Save first: hold OK"));
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
	.name = "channel", .flags = SF_LIST | SF_FIELD, .empty = TRN(CHAN_EMPTY_CHANNEL, "Empty channel"),
#if CONFIG_CHANNEL_EDIT
	.hint = TRN(HINT_OK_CHANGE_HOLD_OK_SAVE, TEXT_ALT_OPEN "OK change  hold OK save  Back"
		TEXT_ALT_NEXT "OK change  hold OK save" TEXT_ALT_CLOSE),
	.key = chinfo_key,
#else
	.hint = TRN(CHAN_HINT_TUNE, "OK tunes to it  Back"),
#endif
	.count = chinfo_count, .item = chinfo_item, .select = chinfo_select, .title = chinfo_title,
	.enter = chinfo_enter,
};

#if CONFIG_CHANNEL_EDIT
// ======================= Leaving the channel editor with changes (defaults to keep editing; P7) ===============

static uint16_t discard_count(void) { return 2; }

static void discard_item(uint16_t i, ui_item_t *it)
{
	str_copy(it->label, sizeof it->label, i == 0 ? TR(DISCARD_ROW_KEEP_EDITING, "No, keep editing") :
		discard_kind ? TR(DISCARD_ROW_SAVE, "Yes, save") : TR(DISCARD_ROW_DISCARD, "Yes, discard"));
	if (i == 1)
		it->flags |= UI_ITEM_WARN;
}

static void discard_select(uint16_t i, bool hold)
{
	(void)hold;
	ui_pop();		// this question
	if (discard_kind) {
		if (i == 1) {	// the save it asked about, on the channel's screen again
			ced_drop_ok = true;
			ced_save(false);
			ced_drop_ok = false;
		}
		return;
	}
	if (i == 1)
		ui_pop();	// and the channel
}

static const char *discard_body(void)
{
	if (discard_kind) {
		// What goes, by its name on the Mode row: "P25 settings will be removed"
		static char ask[40];	// a kind's short name and the longest translation (27 bytes); cut to fit
		sbuf_t b;
		sb_init(&b, ask, sizeof ask);
		sb_str(&b, codeplug_kind_name(ced_orig.kind));
		sb_str(&b, TR(DISCARD_ASK_KIND, " settings will be removed"));
		return ask;
	}
	return discard_contact ? TR(DISCARD_ASK_NEW_CONTACT, "Discard this new contact?") :
		TR(DISCARD_ASK_CHANNEL_CHANGES, "Discard the changes to this channel?");
}

// The save question says what the channel becomes; its body says what goes
static const char *discard_title(void)
{
	if (!discard_kind || ced_ch.kind != CODEPLUG_KIND_NONE)	// another kind: the body says what goes
		return TR(DISCARD_TITLE, "Unsaved Changes");
	return ced_ch.mode == RADIO_MODE_DMR ? TR(DISCARD_TITLE_DMR, "Save as DMR?") :
		TR(DISCARD_TITLE_FM, "Save as FM?");
}

static const ui_scr_def_t discard_screen = {
	.name = "discard", .flags = SF_LIST, .prefix = TR_NONE, .title = discard_title,
	.hint = TRN(DISCARD_HINT, TEXT_ALT_OPEN "OK choose  Back keep editing"
		TEXT_ALT_NEXT "OK choose  Back" TEXT_ALT_CLOSE),
	.count = discard_count, .item = discard_item, .select = discard_select, .body = discard_body,
};
#if CONFIG_CONTACT_EDIT
// ======================= The contact editor (ui.md 2.4) =======================
// A new contact: from Find's "Create contact" on a typed number, or a station's actions (Save as contact). Name (text
// entry), ID (typed), type (talkgroup / private, flips on OK), timeslot (the channel's, 1, 2), Save. Long OK saves too

enum { CT_NAME, CT_ID, CT_TYPE, CT_TS, CT_SAVE, CT_COUNT, CT_NONE = 0xFF };
#define CT_ID_DIGITS	8
#define CT_GROUP_MAX	0xFFFFFEu	// 0xFFFFFF is all call
#define CT_PRIVATE_MAX	((uint32_t)CODEPLUG_DMR_ID_MAX)

static contact_t ct;			// the contact being made
static uint8_t ct_edit = CT_NONE;	// the row whose editor is open (the ID)
// The ID being typed goes in the channel editor's ced_digits, ced_len and ced_val: the two editors are never open at
// once, and RAM is short
_Static_assert(CT_ID_DIGITS <= CE_DIGITS, "the ID's digits fit ced_digits");

void ui_new_contact(const ui_target_t *t)
{
	memset(&ct, 0, sizeof ct);
	ct.id = t->id;
	ct.type = t->group ? CONTACT_GROUP : CONTACT_PRIVATE;
	sbuf_t b;
	sb_init(&b, ct.name, sizeof ct.name);
	if (t->name[0] != 0) {
		sb_str(&b, t->name);
	} else if (t->id != 0) {
		if (t->group)
			sb_str(&b, "TG ");
		sb_u32(&b, t->id, 1);	// "TG 3100", "9991234": a name to save with, or to type over
	}
	ct_edit = CT_NONE;
	ui_push(SCR_CONTACT_EDIT, 0);
}

static uint16_t ct_count(void) { return CT_COUNT; }

static void ct_item(uint16_t i, ui_item_t *it)
{
	static const tr_id_t labels[CT_COUNT] = { TRN(LABEL_NAME, "Name"), TRX(CONTACT_LABEL_ID, "ID"),
		TRN(CONTACT_LABEL_TYPE, "Type"), TRN(LABEL_TIMESLOT, "Timeslot"), TRN(ROW_SAVE, "Save") };
	if (i >= CT_COUNT)
		return;
	str_copy(it->label, sizeof it->label, tr(labels[i]));
	sbuf_t b;
	sb_init(&b, it->value, sizeof it->value);
	switch (i) {
	case CT_NAME:
		sb_str(&b, ct.name);
		break;
	case CT_ID:
		if (ct_edit == CT_ID) {
			it->flags |= UI_ITEM_EDIT;
			sb_str(&b, ced_digits);
			sb_chr(&b, '_');
		} else if (ct.id != 0) {	// a new contact from Contacts starts with none
			sb_u32(&b, ct.id, 1);
		}
		break;
	case CT_TYPE:
		sb_str(&b, ct.type == CONTACT_PRIVATE ? TR(VALUE_PRIVATE, "Private") :
			TR(CONTACT_VALUE_TALKGROUP, "Talkgroup"));
		break;
	case CT_TS:
		if (ct.timeslot == 0) {
			sb_str(&b, TR(CONTACT_VALUE_CHANNEL_DEFAULT, "Channel's"));
		} else {
			sb_str(&b, "TS ");
			sb_u32(&b, ct.timeslot, 1);
		}
		break;
	default:
		break;
	}
}

typedef struct {
	bool group, dup_found;
	uint16_t free;		// the first empty slot, 0 for none
	contact_t *dup;
} ct_scan_t;

static bool ct_scan(uint16_t index, const contact_t *c, void *ctx)
{
	ct_scan_t *sc = ctx;
	if (c == NULL) {
		if (sc->free == 0)
			sc->free = index;
		return true;
	}
	if (c->id == ct.id && (c->type != CONTACT_PRIVATE) == sc->group) {
		*sc->dup = *c;
		sc->dup_found = true;
		return false;
	}
	return true;
}

// The first problem that keeps the contact from being saved, as a toast; NULL when it can be saved. *slot: a free
// contact index. Reads every contact (the duplicate and the free slot)
static const char *ct_problem(uint16_t *slot)
{
	contact_t c;
	bool group = ct.type != CONTACT_PRIVATE;
	if (ct.name[0] == 0)
		return TR(TOAST_NAME_FIRST, "Give it a name first");
	if (ct.id == 0)
		return TR(CONTACT_WHY_ID_FIRST, "Type its ID first");
	if (!group && ct.id > CT_PRIVATE_MAX)
		return TR(CONTACT_WHY_PRIVATE_ID_MAX, "Private IDs go up to 16776415");
	if (group && ct.id > CT_GROUP_MAX)
		return TR(CONTACT_WHY_TALKGROUP_MAX, "Talkgroups go up to 16777214");
	ct_scan_t sc = { .group = group, .dup = &c };
	bool read = codeplug_contacts_each(ct_scan, &sc);
	*slot = sc.free;
	if (sc.dup_found) {
		sbuf_t b;
		sb_init(&b, toast_msg, sizeof toast_msg);
		sb_str(&b, TR(CONTACT_WHY_ALREADY_EXISTS, "Already a contact: "));
		sb_str(&b, c.name);
		return toast_msg;
	}
	if (!read)
		// the duplicate may be in a block not read
		return TR(TOAST_CANT_SAVE_NOW, "Couldn't save now: try again");
	return *slot == 0 ? TR(CONTACT_WHY_NO_ROOM, "No room for more contacts") : NULL;
}

static void ct_save(void)
{
	uint16_t slot = 0;
	const char *why = ct_problem(&slot);
	if (why != NULL) {
		ui_toast(why);
		return;
	}
	if (!ui_svc_write_contact(slot, &ct)) {
		ui_toast(TR(TOAST_CANT_SAVE_NOW, "Couldn't save now: try again"));
		return;
	}
	sbuf_t b;
	sb_init(&b, toast_msg, sizeof toast_msg);
	sb_str(&b, TR(CONTACT_TOAST_SAVED, "Saved contact "));
	sb_str(&b, ct.name);
	ui_pop();
	if (ui_frame()->scr == SCR_ACTIONS && ui_target.id == ct.id && ui_target.group == (ct.type != CONTACT_PRIVATE)) {
		ui_target.saved = true;		// the actions it came from: In contacts, under its new name
		strcpy(ui_target.name, ct.name);
	} else if (ui_frame()->scr != SCR_CONTACTS) {
		ui_go_home();	// Find: its rows are the number's, not the contact (written by the storage job, later)
	}	// Contacts lists it once the job has written it (the codeplug's change rebuilds the list)
	ui_toast(toast_msg);
}

static void ct_select(uint16_t i, bool hold)
{
	if (hold) {
		ct_save();
		return;
	}
	switch (i) {
	case CT_NAME:
		ui_text_open(ct.name, CODEPLUG_NAME_LEN);
		break;
	case CT_ID:
		ct_edit = CT_ID;
		ced_len = 0;
		ced_val = 0;
		ced_digits[0] = 0;
		break;
	case CT_TYPE:
		ct.type = ct.type == CONTACT_PRIVATE ? CONTACT_GROUP : CONTACT_PRIVATE;
		break;
	case CT_TS:
		ct.timeslot = (uint8_t)((ct.timeslot + 1u) % 3u);
		break;
	case CT_SAVE:
		ct_save();
		break;
	default:
		break;
	}
	ui_dirty();
}

static bool ct_key(const input_event_t *ev)
{
	bool press = ev->gesture == GESTURE_PRESS || ev->gesture == GESTURE_LONG;
	if (ct_edit == CT_NONE) {
		if (ev->action == ACT_BACK && ev->gesture == GESTURE_PRESS) {
			discard_contact = true;
			discard_kind = false;
			ui_push(SCR_DISCARD, 0);
			return true;
		}
		return false;
	}
	// The ID being typed: digits append (refused past the largest talkgroup), Back takes one back or closes it, OK
	// keeps it; the knob can't move the focus under it
	if (ev->action >= ACT_DIGIT_0 && ev->action <= ACT_DIGIT_9 && press) {
		uint32_t v = (uint32_t)ced_val * 10u + (uint32_t)(ev->action - ACT_DIGIT_0);
		if (ced_len >= CT_ID_DIGITS || v > CT_GROUP_MAX) {
			ui_toast(TR(CONTACT_TOAST_ID_MAX, "IDs go up to 16777214"));
		} else {
			ced_val = (int32_t)v;
			ced_digits[ced_len++] = (char)('0' + ev->action - ACT_DIGIT_0);
			ced_digits[ced_len] = 0;
		}
		ui_dirty();
		return true;
	}
	if (ev->action == ACT_BACK && ev->gesture == GESTURE_PRESS) {
		if (ced_len > 0) {
			ced_digits[--ced_len] = 0;
			ced_val /= 10;
		} else {
			ct_edit = CT_NONE;
		}
		ui_dirty();
		return true;
	}
	if (ev->action == ACT_SELECT && press) {
		if (ced_val != 0)
			ct.id = (uint32_t)ced_val;		// nothing typed (or 0): unchanged
		ct_edit = CT_NONE;
		ui_dirty();
		return true;
	}
	if (ev->action == ACT_NAV_UP || ev->action == ACT_NAV_DOWN || ev->action == ACT_ENC_STEP)
		return true;
	return false;
}

static const ui_scr_def_t contact_edit_screen = {
	.name = "contactedit", .flags = SF_LIST | SF_FIELD, .prefix = TRN(CONTACT_TITLE_NEW, "New Contact"),
	.hint = TRN(HINT_OK_CHANGE_HOLD_OK_SAVE, TEXT_ALT_OPEN "OK change  hold OK save  Back"
		TEXT_ALT_NEXT "OK change  hold OK save" TEXT_ALT_CLOSE),
	.count = ct_count, .item = ct_item, .select = ct_select, .key = ct_key,
};
#endif

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
	static const tr_id_t names[ZC_COUNT] = {
		[ZC_UP] = TRN(ZONE_ROW_MOVE_UP, "Move up"), [ZC_DOWN] = TRN(ZONE_ROW_MOVE_DOWN, "Move down"),
		[ZC_COPY] = TRN(ZONE_ROW_COPY_HERE, "Copy here"),
		[ZC_EDIT] = TRN(ZONE_ROW_EDIT_CHANNEL, "Edit channel"),
#if CONFIG_ZONE_SHARE
		[ZC_SEND] = TRN(SHARE_ROW_CHANNEL_TO, GLYPH_SHARE " Share channel to..."),
#endif
		[ZC_REMOVE] = TRN(ZONE_ROW_REMOVE_CHANNEL, "Remove from zone") };
	str_copy(it->label, sizeof it->label, tr(names[i]));
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
			ui_toast(dir < 0 ? TR(ZONE_TOAST_ALREADY_FIRST, "Already first") :
				TR(ZONE_TOAST_ALREADY_LAST, "Already last"));
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
		sb_str(&b, TR(ZONE_TOAST_POSITION, "Position "));
		sb_u32(&b, pos + 1u, 1);
		sb_str(&b, TR(PART_OF, " of "));
		sb_u32(&b, ze_z.count, 1);
		ui_toast(ze_msg);
	} else if (i == ZC_COPY) {
		uint16_t free = codeplug_free_channel();
		if (free == 0) {
			ui_toast(TR(TOAST_NO_FREE_CHANNEL, "No free channel"));
		} else if (ze_z.count >= codeplug_zone_capacity()) {
			ui_toast(TR(ZONE_TOAST_FULL, "Zone full"));
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
			ze_toast(TR(ZONE_TOAST_REMOVED_FROM, "Removed from "), ze_z.name);
		}
	}
}

static const char *zchan_title(void) { return zc_name; }

static const ui_scr_def_t zchan_screen = {
	.name = "zchannel", .flags = SF_LIST | SF_NUMBERED, .hint = CONFIG_ZONE_SHARE ?
		TRN(HINT_1_6_PICK_BACK, "1-6 pick  Back") : TRN(HINT_1_5_PICK_BACK, "1-5 pick  Back"),
	.count = zchan_count, .item = zchan_item, .select = zchan_select, .title = zchan_title, .enter = zchan_enter,
};

// --- Add channel: the stored channels the zone doesn't hold (raw r: channel r + 1) ---
static uint16_t zpick_raw_count(void)
{
	ze_read();
	return codeplug_channel_count();
}

static bool zpick_raw_label(uint16_t raw, char *buf, uint8_t len)
{
	uint16_t num = (uint16_t)(raw + 1u);
	char name[CODEPLUG_NAME_LEN + 1];
	if (zone_list_find(&ze_z, num) >= 0 || !codeplug_channel_name(num, name))
		return false;
	ch_label(num, name, buf, len);
	return true;
}

static bool zpick_raw_used(uint16_t raw)
{
	uint16_t num = (uint16_t)(raw + 1u);
	return zone_list_find(&ze_z, num) < 0 && codeplug_channel_used(num);
}

static void zpick_raw_each(ui_raw_fn fn, void *ctx, bool labels)
{
	ze_read();
	ch_rows(0, &ze_z, fn, ctx, labels);
}

static void zpick_item(uint16_t i, ui_item_t *it)
{
	static channel_t ch;
	uint16_t num = (uint16_t)(ui_filter_raw(i) + 1u);
	if (!codeplug_channel(num, &ch))
		return;
	ch_label(num, ch.name, it->label, sizeof it->label);
	sbuf_t b;
	sb_init(&b, it->value, sizeof it->value);
	sb_channel_row(&b, num, &ch);	// adding to a zone: allowed, the row says the system isn't set up
}

static void zpick_select(uint16_t i, bool hold)
{
	(void)hold;
	uint16_t num = (uint16_t)(ui_filter_raw(i) + 1u);
	const radio_view_t *v = ui_view();
	if (ze_z.count >= codeplug_zone_capacity()) {
		ui_toast(TR(ZONE_TOAST_FULL, "Zone full"));
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
	ze_toast(TR(ZONE_TOAST_ADDED_TO, "Added to "), ze_z.name);
}

static const ui_scr_def_t zpick_screen = {
	.name = "zpick", .flags = SF_LIST | SF_FILTER, .prefix = TRN(ZONE_TITLE_ADD_CHANNEL, "Add Channel"),
	.hint = TRN(ZONE_HINT_ADD_CHANNEL, TEXT_ALT_OPEN "OK add  0-9 find  Back"
		TEXT_ALT_NEXT "OK add  Back" TEXT_ALT_CLOSE),
	.empty = TRN(ZONE_EMPTY_ALL_CHANNELS_IN, "Every channel is in it"),
	.count = ui_filter_count, .item = zpick_item, .select = zpick_select,
	.filter = &(const ui_filter_def_t){ .raw_count = zpick_raw_count, .raw_label = zpick_raw_label,
		.raw_each = zpick_raw_each, .raw_used = zpick_raw_used },
};

// --- The channel editor's Zones: every zone, and whether the channel is in it; OK puts it in or takes it out ---
static int chz_pos(uint16_t zi)
{
	if (!codeplug_zone(zi, &chz_z))
		return -2;
	return zone_list_find(&chz_z, ui_frame()->arg);
}

static uint16_t chz_count(void) { return codeplug_zone_count(); }

static void chz_item(uint16_t i, ui_item_t *it)
{
	int at = chz_pos(i);
	if (at == -2)
		return;
	strcpy(it->label, chz_z.name);
	str_copy(it->value, sizeof it->value, at >= 0 ? TR(ZONE_VALUE_IN, "In") : "-");
}

static void chz_select(uint16_t i, bool hold)
{
	(void)hold;
	int at = chz_pos(i);
	if (at == -2)
		return;
	if (at < 0 && chz_z.count >= codeplug_zone_capacity()) {
		ui_toast("Zone full");
		return;
	}
	// The zone screens' op, for this zone (ze_zone stays the open zone's: this editor may sit on its list)
	zone_op_t *op = ze_op_new(at >= 0 ? ZONE_OP_REMOVE : ZONE_OP_INSERT, at >= 0 ? (uint16_t)at : chz_z.count);
	op->zone = i;
	op->channel = ui_frame()->arg;
	if (ze_post())
		ze_toast(at >= 0 ? TR(ZONE_TOAST_REMOVED_FROM, "Removed from ") : TR(ZONE_TOAST_ADDED_TO, "Added to "),
			chz_z.name);
}

static const ui_scr_def_t chz_screen = {
	.name = "chzones", .flags = SF_LIST, .prefix = TRX(ZONE_TITLE_IN_ZONES, "In Zones"),
	.hint = TRX(ZONE_HINT_IN_ZONES, "OK in/out  Back"),
	.empty = TRN(ZONE_EMPTY_NO_ZONES, "No zones"),
	.count = chz_count, .item = chz_item, .select = chz_select,
};

// --- Zone options ---
enum { ZO_RENAME, ZO_COPY,
#if CONFIG_ZONE_SHARE
	ZO_SEND,	// zone sharing (zone-share.md 7)
#endif
	ZO_DELETE, ZO_COUNT };

static uint16_t zopt_count(void) { return ZO_COUNT; }

static void zopt_item(uint16_t i, ui_item_t *it)
{
	static const tr_id_t names[ZO_COUNT] = {
		[ZO_RENAME] = TRN(ZONE_ROW_RENAME, "Rename"), [ZO_COPY] = TRN(ZONE_ROW_COPY_AS_NEW, "Copy as new zone"),
#if CONFIG_ZONE_SHARE
		[ZO_SEND] = TRN(SHARE_ROW_ZONE_TO, GLYPH_SHARE " Share zone to..."),
#endif
		[ZO_DELETE] = TRN(ZONE_ROW_DELETE, "Delete zone") };
	str_copy(it->label, sizeof it->label, tr(names[i]));
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
		ui_toast(TR(TOAST_NAME_FIRST, "Give it a name first"));
		return;
	}
	strcpy(ze_op_new(ZONE_OP_RENAME, 0)->name, ze_name);
	if (ze_post()) {
		strcpy(ze_z.name, ze_name);
		ui_toast(TR(ZONE_TOAST_RENAMED, "Renamed"));
	}
}

// Zones > Create New Zone: named in the text entry, then written empty; Zones focuses it, and its list's first row adds
// channels
static void zones_new_named(void)
{
	if (ze_name[0] == 0) {
		ui_toast("Give it a name first");
		return;
	}
	strcpy(ze_op_new(ZONE_OP_NEW, 0)->name, ze_name);
	if (!ze_post())
		return;
	zones_new_pending = true;
	ze_toast(TR(ZONE_TOAST_ADDED, "Added "), ze_name);
}

static void zones_new(void)
{
	if (codeplug_zone_count() >= CODEPLUG_ZONES_MAX && codeplug_zones_stored()) {
		ui_toast(TR(ZONE_ERR_NO_ROOM, "No room for another zone"));
		return;
	}
	ze_name[0] = 0;
	ui_text_open(ze_name, CODEPLUG_NAME_LEN);
	text_done = zones_new_named;
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
			ui_toast(TR(ZONE_ERR_NO_ROOM, "No room for another zone"));
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
		ze_toast(TR(ZONE_TOAST_COPIED_AS, "Copied as "), name);
#if CONFIG_ZONE_SHARE
	} else if (i == ZO_SEND) {
		if (ze_z.count == 0)
			ui_toast(TR(ZONE_TOAST_EMPTY_NOTHING_TO_SEND, "Empty zone: nothing to send"));
		else
			share_send_to(ze_zone, 0, false, ze_z.name);
#endif
	} else if (!codeplug_zones_stored()) {
		ui_toast(TR(ZONE_TOAST_HOLDS_EVERY_CHANNEL, "It holds every channel: it stays"));
	} else {
		ui_push(SCR_ZONEDEL, 0);
	}
}

static const ui_scr_def_t zopt_screen = {
	.name = "zoneedit", .flags = SF_LIST | SF_NUMBERED, .prefix = TRN(WORD_ZONE, "Zone"),
	.hint = CONFIG_ZONE_SHARE ? TRN(HINT_1_4_PICK_BACK, "1-4 pick  Back") :
		TRN(HINT_1_3_PICK_BACK, "1-3 pick  Back"),
	.count = zopt_count, .item = zopt_item, .select = zopt_select, .title = ze_title, .enter = ze_read,
};

// --- Delete zone (destructive: asks, defaults to No; P7) ---
static uint16_t zdel_count(void) { return 2; }

static void zdel_item(uint16_t i, ui_item_t *it)
{
	str_copy(it->label, sizeof it->label, i == 0 ? TR(ZONE_ROW_KEEP, "No, keep it") :
		TR(ZONE_ROW_CONFIRM_DELETE, "Yes, delete"));
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
	ze_toast(TR(ZONE_TOAST_DELETED, "Deleted "), ze_z.name);
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
	sb_str(&b, TR(ZONE_ASK_DELETE, "Delete "));
	sb_str(&b, ze_z.name);
	sb_str(&b, TR(ZONE_ASK_CHANNELS_STAY, "? Channels stay."));
	return ze_msg;
}

static const ui_scr_def_t zdel_screen = {
	.name = "zonedel", .flags = SF_LIST, .prefix = TRN(ZONE_TITLE_DELETE, "Delete Zone"),
	.hint = TRN(HINT_OK_CHOOSE_BACK_CANCELS, "OK choose  Back cancel"),
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
	const char *name = TR(VALUE_NONE, "None");
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

static void chpick_raw_each(ui_raw_fn fn, void *ctx, bool labels)
{
	(void)labels;
	if (ui_frame()->arg == 0) {
		if (fn(0, TR(VALUE_NONE, "None"), ctx))
			contacts_rows(1, fn, ctx, false);	// raw = contact number
		return;
	}
	char label[40];
	for (uint16_t raw = 0; raw < chpick_raw_count(); raw++) {	// the RX group lists: few, read one by one
		if (!fn(raw, chpick_raw_label(raw, label, sizeof label) ? label : NULL, ctx))
			return;
	}
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

static const char *chpick_title(void) { return ui_frame()->arg == 0 ? TR(CHAN_TITLE_TX_CONTACT, "TX Contact") :
	TR(CHAN_TITLE_RX_GROUP_LIST, "RX Group List"); }

static const ui_scr_def_t chpick_screen = {
	.name = "chpick", .flags = SF_LIST | SF_FILTER, .hint = TRN(HINT_OK_PICK_FIND_BACK, "OK pick  0-9 find  Back"),
	.empty = TRN(CHAN_EMPTY_NONE_STORED, "None stored"),
	.count = ui_filter_count, .item = chpick_item, .select = chpick_select, .title = chpick_title,
	.enter = chpick_enter,
	.filter = &(const ui_filter_def_t){ .raw_count = chpick_raw_count, .raw_label = chpick_raw_label,
		.raw_each = chpick_raw_each },
};
#endif
#endif
#endif

// ======================= Info pages =======================

enum { INFO_POSITION, INFO_HOTSPOT, INFO_BATTERY, INFO_FIRMWARE, INFO_CODEPLUG, INFO_USB_NET,
       INFO_SFR };

static const tr_id_t info_titles[] = { TRN(TITLE_POSITION, "Position"), TRN(TITLE_HOTSPOT, "Hotspot"),
	TRN(TITLE_BATTERY, "Battery"), TRN(TITLE_ABOUT, "About"), TRN(TITLE_CODEPLUG, "Codeplug"),
	TRN(USBNET_TITLE, "USB Network"), TRN(SFR_TITLE, "Single-Freq Repeater") };

// The USB personality can be switched here (the host link's network adapter, CONFIG_USB_NETWORK)
#define USB_NET_UI	(CONFIG_USB_NETWORK && CONFIG_HOSTLINK)

#if CONFIG_SFR && CONFIG_VFO_DMR
// Opened in the VFO, the SFR page sets the VFO's own role (sfr.md 4): a Role row and an Inbound slot row, SELECT steps
// each. The VFO's stored bytes are in the view's channel; the role in force (dmr.sfr_role) is off on a split or in FM
#define SFR_VFO_UI	(ui_frame()->arg == INFO_SFR && ui_view()->vfo)
static const tr_id_t sfr_role_names[SFR_ROLE_NUM_STORED] = { TRN(VALUE_OFF, "Off"), TRN(SFR_VALUE_USER, "User"),
	TRN(SFR_VALUE_USER_ERDM, "User (ERDM)"), TRN(SFR_VALUE_REPEATER, "Repeater"),
	TRN(SFR_VALUE_REPEATER_ERDM, "Repeater (ERDM style)") };
static uint8_t sfr_vfo_a(void)
{
	const channel_t *c = &ui_view()->ch;
	return (c->sfr_flag1 & SFR_CHANNEL_FLAG1_ENABLED) ? c->sfr_a : 0;
}
#else
#define SFR_VFO_UI	false
#endif

static const char *info_title(void)
{
	uint16_t a = ui_frame()->arg;
	return a == INFO_BATTERY && board_volts() == BOARD_VOLTS_SUPPLY ? TR(TITLE_SUPPLY, "Supply") :
		tr(info_titles[a]);
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

static const char *info_body(void)
{
	static char t[200];
	sbuf_t b;
	sb_init(&b, t, sizeof t);
	switch (ui_frame()->arg) {
	case INFO_POSITION: {
		ui_position_t p;
		if (!ui_svc_position(&p)) {
			sb_str(&b, TR(POS_BODY_NO_GPS_OR_OFF, "No GPS, or it is off (Settings > Location)"));
		} else if (!p.fix) {
			// GGA counts only satellites a fix uses
			sb_str(&b, TR(POS_BODY_NO_FIX_GO_OUTSIDE, "No fix yet. Go outside, away from buildings."));
		} else {
			sb_str(&b, TR(POS_BODY_FIX, "Fix, "));
			sb_u32(&b, p.sats, 1);
			sb_str(&b, TR(POS_UNIT_SATELLITES, " satellites"));
			sb_chr(&b, '\n');
			sb_coord(&b, p.lat_e7, 'N', 'S');
			sb_chr(&b, ' ');
			sb_coord(&b, p.lon_e7, 'E', 'W');
			if (p.alt_ok) {
				sb_chr(&b, '\n');
				sb_str(&b, TR(POS_BODY_ALTITUDE, "Altitude "));
				sb_i32(&b, p.alt_m);
				sb_str(&b, " m");
			}
		}
		break;
	}
	case INFO_HOTSPOT:
#if CONFIG_HOTSPOT
		sb_str(&b,
			TR(HOTSPOT_BODY_HOW_TO_START,
				"Start MMDVMHost or Pi-Star on a computer on USB: hotspot mode starts by itself."));
#else
		sb_str(&b, TR(WHY_HOTSPOT_NOT_IN_BUILD, "Hotspot: not in this build"));
#endif
		break;
	case INFO_BATTERY:
		if (board_volts() == BOARD_VOLTS_SUPPLY) {	// a reading only: no charge level or warning
			sb_volts(&b, ui_view()->supply_mv);
			break;
		}
		sb_u32(&b, ui_st.battery_pct, 1);
		sb_str(&b, ui_st.battery_pct <= (ui_st.low_batt_pct ? ui_st.low_batt_pct : 15) ?
			TR(BATT_PART_CHARGE_SOON, "%: charge soon") : "%");
		break;
	case INFO_CODEPLUG: {
		static const tr_id_t src[] = { TRN(CPLUG_VALUE_NONE_BUILT_IN, "None: built-in channels"),
			TRN(CPLUG_VALUE_BUILT_IN, "Built-in channels"),
			TRN(CPLUG_VALUE_WRITTEN_BY_CPS, "Written by the CPS") };
		codeplug_source_t s = codeplug_source();
		sb_str(&b, (unsigned)s < 3 ? tr(src[s]) : "?");
		sb_chr(&b, '\n');
		if (codeplug_foreign()) {	// another firmware's codeplug, left alone until the CPS writes one
			sb_str(&b, TR(CPLUG_BODY_OTHER_FIRMWARE, "Other firmware's codeplug: ignored"));
			sb_chr(&b, '\n');
		}
		sb_str(&b, TR(CPLUG_BODY_ZONES, "Zones: "));
		sb_u32(&b, codeplug_zone_count(), 1);
		sb_chr(&b, '\n');
		sb_str(&b, TR(CPLUG_BODY_CHANNELS_UP_TO, "Channels: up to "));
		sb_u32(&b, codeplug_channel_count(), 1);
		break;
	}
	case INFO_USB_NET:
#if USB_NET_UI
		if (hostlink_network_mode())
			sb_str(&b,
				TR(USBNET_BODY_IS_NETWORK,
					"USB is a network adapter: IP over DMR. qdmr and the hotspot are off until you switch "
				"back."));
		else
			sb_str(&b,
				TR(USBNET_BODY_IS_SERIAL,
					"USB is a serial port: qdmr, hotspot. As a network adapter it carries IP over DMR."));
#else
		sb_str(&b, TR(WHY_USB_NETWORK_NOT_IN_BUILD, "USB network: not in this build"));
#endif
		break;
	case INFO_SFR: {
#if CONFIG_SFR
		// The channel's SFR role and what it is doing now (docs/spec/sfr.md 4)
		const radio_dmr_view_t *d = &ui_view()->dmr;
		char in = d->sfr_inbound ? '2' : '1', out = d->sfr_inbound ? '1' : '2';

#if CONFIG_VFO_DMR
		if (ui_view()->vfo && d->sfr_role == SFR_ROLE_OFF) {
			sb_str(&b, (sfr_vfo_a() & 0x07u) == 0 ?
				TR(SFR_BODY_VFO_NO_ROLE, "The VFO has no SFR role. Pick one below.") :
				TR(SFR_BODY_OFF_NEEDS_DMR_SIMPLEX,
					"Off here: the role needs DMR and one frequency (no split)."));
		} else
#endif
		if (ui_view()->ch.mode != RADIO_MODE_DMR || d->sfr_role == SFR_ROLE_OFF) {
			sb_str(&b,
				TR(SFR_BODY_CHANNEL_NO_ROLE,
					"This channel has no SFR role. Set Repeater or User in the Codeplug Editor, on a channel "
				"with one frequency."));
		} else if (d->sfr_role == SFR_ROLE_RPT_STANDARD || d->sfr_role == SFR_ROLE_RPT_MOTOROLA) {
			sb_str(&b, d->sfr_role == SFR_ROLE_RPT_MOTOROLA ?
				TR(SFR_VALUE_REPEATER_ERDM, "Repeater (ERDM style)") :
				TR(SFR_VALUE_REPEATER, "Repeater"));
			sb_chr(&b, '\n');
			sb_str(&b, TR(SFR_BODY_IN_TS, "In TS"));
			sb_chr(&b, in);
			sb_str(&b, TR(SFR_BODY_OUT_TS, ", out TS"));
			sb_chr(&b, out);
			sb_chr(&b, '\n');
			sb_str(&b, !d->sfr_rpt ? TR(SFR_STATE_OFF_TX_NOT_ALLOWED, "Off: transmit not allowed now") :
				d->sfr_cw ? TR(SFR_STATE_SENDING_CW_ID, "Sending its CW ID") :
				d->sfr_busy ? TR(SFR_STATE_REPEATING, "Repeating a call") : d->sfr_keyed ?
					TR(SFR_STATE_HANG_TIME, "Hang time") : TR(SFR_STATE_LISTENING, "Listening"));
		} else {
			sb_str(&b, d->sfr_role == SFR_ROLE_USER_ERDM ?
				TR(SFR_BODY_USER_OF_ERDM, "User of an ERDM repeater") :
				TR(SFR_BODY_USER_OF_REPEATER, "User of a repeater"));
			sb_chr(&b, '\n');
			sb_str(&b, TR(SFR_BODY_TALK_ON_TS, "Talk on TS"));
			sb_chr(&b, in);
			sb_str(&b, TR(SFR_BODY_HEAR_TS, ", hear TS"));
			sb_chr(&b, in);
			sb_str(&b, TR(SFR_BODY_AND_TS, " and TS"));
			sb_chr(&b, out);
		}
#else
		sb_str(&b, TR(WHY_SFR_NOT_IN_BUILD, "Single-freq repeater: not in this build"));
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
	if (SFR_VFO_UI)
		return 2;
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
#if CONFIG_SFR && CONFIG_VFO_DMR
	if (SFR_VFO_UI) {
		uint8_t a = sfr_vfo_a(), role = a & 0x07u;
		str_copy(it->label, sizeof it->label, i == 0 ? TR(SFR_LABEL_ROLE, "Role") :
			TR(SFR_LABEL_INBOUND_SLOT, "Inbound slot"));
		str_copy(it->value, sizeof it->value, i == 0 ? tr(sfr_role_names[role < SFR_ROLE_NUM_STORED ? role : 0]) :
			 (a & 0x20u) ? "TS2" : "TS1");
		if (i == 1 && role == SFR_ROLE_OFF)
			it->flags |= UI_ITEM_DIM;
		return;
	}
#endif
#if USB_NET_UI
	if (ui_frame()->arg == INFO_USB_NET) {
		str_copy(it->label, sizeof it->label, hostlink_network_mode() ?
			TR(USBNET_ROW_SWITCH_TO_SERIAL, "Switch USB to serial") :
			 TR(USBNET_ROW_SWITCH_TO_NETWORK, "Switch USB to network"));
		return;
	}
#endif
	str_copy(it->label, sizeof it->label, TR(HOTSPOT_ROW_STOP, "Stop hotspot"));
}

static void info_select(uint16_t i, bool hold)
{
	(void)hold;
#if CONFIG_SFR && CONFIG_VFO_DMR
	if (SFR_VFO_UI) {
		uint8_t a = sfr_vfo_a(), role = a & 0x07u;
		if (i == 0) {
			a = (uint8_t)((a & ~0x07u) | (role + 1u) % SFR_ROLE_NUM_STORED);	// the next role, Off after the last
		} else if (role != SFR_ROLE_OFF) {
			a ^= 0x20u;		// the inbound slot: TS1 <-> TS2
		} else {
			ui_toast(TR(SFR_TOAST_PICK_ROLE_FIRST, "Pick a role first"));
			return;
		}
		if (!ui_svc_vfo_sfr(a))
			ui_toast(TR(WHY_SFR_NOT_IN_BUILD, "Single-freq repeater: not in this build"));
		return;
	}
#endif
#if USB_NET_UI
	if (ui_frame()->arg == INFO_USB_NET) {
		bool net = !hostlink_network_mode();
		if (!hostlink_request_usb_network(net)) {
			ui_toast(TR(USBNET_WHY_NOT_ON_RADIO, "No USB network on this radio"));
		} else {
			ui_go_home();
			ui_toast(net ? TR(USBNET_TOAST_NETWORK_NOW, "USB is a network adapter now") :
				TR(USBNET_TOAST_SERIAL_AGAIN, "USB is a serial port again"));
		}
		return;
	}
#endif
#if CONFIG_HOTSPOT
	if (ui_frame()->arg == INFO_HOTSPOT) {
		hostlink_hotspot_exit();
		ui_toast(TR(HOTSPOT_TOAST_STOPPED, "Hotspot stopped"));
	}
#endif
}

static const ui_scr_def_t info_screen = {
	.name = "info", .flags = SF_LIST | SF_FIELD, .hint = TRN(HINT_BACK, "Back"), .count = info_count,
	.item = info_item,
	.select = info_select, .title = info_title, .body = info_body,
};

// ======================= My radio: DMR ID and names (Settings > My radio) =======================
// The radio's own DMR ID (CONFIG_DMR) and the codeplug's name fields: the radio name and the two boot screen lines,
// all saved in the codeplug (a CPS read shows them). On the ID row digits type a new one, OK writes it. Typed as the
// channel editor's Own ID (ui.md 2.1): a digit past CODEPLUG_DMR_ID_MAX is refused, BACK takes the last digit back and
// with none typed leaves the ID unchanged. OK on a name row opens the text entry; keeping it writes the name

enum {
#if CONFIG_DMR
	MY_ID,
#endif
	MY_NAME, MY_BOOT1, MY_BOOT2, MY_COUNT };
static const uint8_t my_names[] = { [MY_NAME] = UI_NAME_RADIO, [MY_BOOT1] = UI_NAME_BOOT1, [MY_BOOT2] = UI_NAME_BOOT2 };
static char my_text[CODEPLUG_NAME_LEN + 1];	// the name being typed, then shown until the codeplug's serial moves on
static uint8_t my_text_row = MY_COUNT;		// the row my_text belongs to (MY_COUNT: none)
static uint32_t my_text_serial;

#if CONFIG_DMR
static uint32_t dmr_id_value;
static uint8_t dmr_id_len;		// digits typed
static bool dmr_id_editing;
static uint32_t dmr_id_saved, dmr_id_serial;	// the ID just saved, shown until the codeplug's serial moves on
#endif

static uint16_t dmr_id_count(void) { return MY_COUNT; }
#if DATA_IDS
static void ds_forget(void);	// Data services, below
#endif

void ui_menus_save_failed(void)
{
#if CONFIG_ZONE_EDIT
	zones_new_pending = false;	// nothing to focus
#endif
	my_text_row = MY_COUNT;		// the stored name shows again
#if CONFIG_DMR
	dmr_id_saved = 0;
#endif
#if DATA_IDS
	ds_forget();
#endif
}

static void dmr_id_item(uint16_t i, ui_item_t *it)
{
	sbuf_t b;
	sb_init(&b, it->value, sizeof it->value);
#if CONFIG_DMR
	if (i == MY_ID) {
		str_copy(it->label, sizeof it->label, TR(MYID_LABEL_DMR_ID, "DMR ID"));
		uint32_t v = dmr_id_editing ? dmr_id_value : dmr_id_saved != 0 && codeplug_serial() == dmr_id_serial ?
			     dmr_id_saved : (uint32_t)ui_svc_setting_get(UI_SET_DMR_ID);
		if (dmr_id_editing) {
			if (dmr_id_len > 0)
				sb_u32(&b, v, 1);
			sb_chr(&b, '_');		// what is typed so far, as the channel editor: "3101_"
			it->flags |= UI_ITEM_EDIT;
		} else if (v != 0) {
			sb_u32(&b, v, 1);
		} else {
			sb_str(&b, TR(VALUE_NOT_SET, "Not set"));	// DMR transmit is refused until there is one
		}
		return;
	}
#endif
	static const tr_id_t labels[] = { [MY_NAME] = TRN(MYID_LABEL_RADIO_NAME, "Radio name"),
		[MY_BOOT1] = TRN(MYID_LABEL_BOOT_1, "Boot 1"), [MY_BOOT2] = TRN(MYID_LABEL_BOOT_2, "Boot 2") };
	str_copy(it->label, sizeof it->label, tr(labels[i]));
	char name[CODEPLUG_NAME_LEN + 1];
	if (my_text_row == i && codeplug_serial() == my_text_serial)
		strcpy(name, my_text);
	else
		ui_svc_name_get(my_names[i], name);
	sb_str(&b, name[0] != 0 ? name : TR(VALUE_NONE, "None"));
}

// My Radio's and Data Services' text above the rows, one buffer (the two screens never show together)
static char id_body[48];

static const char *dmr_id_body(void)
{
	sbuf_t b;
	sb_init(&b, id_body, sizeof id_body);
	sb_str(&b, TR(BODY_SAVED_IN_CODEPLUG, "Saved in the codeplug"));
	sb_chr(&b, '\n');
	sb_str(&b, TR(BODY_RADIO, "Radio: "));
	sb_str(&b, board.name);		// as the info page it replaces
	return id_body;
}

#if CONFIG_CHANNEL_EDIT
static void my_text_kept(void)
{
	my_text_serial = codeplug_serial();	// before the job: the storage task may finish it before we look again
	if (!ui_svc_name_set(my_names[my_text_row], my_text)) {
		my_text_row = MY_COUNT;
		ui_toast("Couldn't save: try again");
		return;
	}
	ui_toast(TR(TOAST_SAVED, "Saved"));
}
#endif

#if CONFIG_DMR
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
		// 0 would leave the codeplug without one
		ui_toast(TR(MYID_TOAST_IDS_START_AT_1, "IDs start at 1"));
		return;
	}
	dmr_id_editing = false;
	uint32_t serial = codeplug_serial();
	if (!ui_svc_setting_set(UI_SET_DMR_ID, (int32_t)dmr_id_value)) {
		ui_toast(TR(TOAST_SAVE_FAILED, "Couldn't save: try again"));
		return;
	}
	dmr_id_saved = dmr_id_value;
	dmr_id_serial = serial;
	ui_toast(TR(MYID_TOAST_SAVED, "DMR ID saved"));
}
#endif

static void dmr_id_select(uint16_t i, bool hold)
{
	(void)hold;
#if CONFIG_DMR
	if (i == MY_ID) {
		if (dmr_id_editing)
			dmr_id_save();
		else
			dmr_id_begin();
		return;
	}
#endif
	if (i >= MY_COUNT)
		return;
#if CONFIG_CHANNEL_EDIT
	ui_svc_name_get(my_names[i], my_text);
	my_text_row = (uint8_t)i;
	my_text_serial = codeplug_serial() - 1u;	// shown from the codeplug until it is kept
	ui_text_open(my_text, my_names[i] == UI_NAME_RADIO ? CODEPLUG_RADIO_NAME_LEN : CODEPLUG_NAME_LEN);
	text_done = my_text_kept;
#else
	ui_toast(TR(WHY_TYPING_NOT_IN_BUILD, "Typing: not in this build"));
#endif
}

static bool dmr_id_key(const input_event_t *ev)
{
#if CONFIG_DMR
	bool press = ev->gesture == GESTURE_PRESS || ev->gesture == GESTURE_LONG;
	if (ev->action >= ACT_DIGIT_0 && ev->action <= ACT_DIGIT_9 && press && ui_frame()->focus == MY_ID) {
		if (!dmr_id_editing)
			dmr_id_begin();
		uint32_t v = dmr_id_value * 10u + (uint32_t)(ev->action - ACT_DIGIT_0);
		if (v > CODEPLUG_DMR_ID_MAX) {
			// the IDs above are the network's
			ui_toast(TR(TOAST_ID_MAX_PRIVATE, "IDs go up to 16776415"));
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
	// Another row while typing: the ID typed so far is dropped, as BACK with none typed
	if (ev->action == ACT_NAV_UP || ev->action == ACT_NAV_DOWN || ev->action == ACT_ENC_STEP)
		dmr_id_editing = false;
#else
	(void)ev;
#endif
	return false;
}

static void dmr_id_enter(void)
{
#if CONFIG_DMR
	dmr_id_editing = false;
	dmr_id_saved = 0;
#endif
}

static const ui_scr_def_t dmr_id_screen = {
	.name = "dmr-id", .flags = SF_LIST | SF_FIELD, .prefix = TRN(SET_TITLE_MY_RADIO, "My Radio"),
#if CONFIG_DMR
	.hint = TRN(HINT_TYPE_OK_EDIT_SAVE_BACK, "0-9 type  OK edit/save  Back"),
#else
	.hint = TRN(HINT_OK_EDIT_BACK, "OK edit  Back"),
#endif
	.count = dmr_id_count, .item = dmr_id_item, .select = dmr_id_select, .key = dmr_id_key, .enter = dmr_id_enter,
	.body = dmr_id_body,
};

#if DATA_IDS
// ======================= Data services (Settings > Connections) =======================
// The ARS presence server and the off-net IP gateway, both DMR IDs saved in the codeplug's settings area
// (docs/spec/codeplug-settings.md), so a CPS read shows them and the Codeplug Editor can set them. Typed as My radio's
// DMR ID: digits type, OK saves, BACK takes the last digit back and with none typed leaves the ID as it was. 0 turns
// the service off. The body shows the ARS registration

enum {
#if CONFIG_DMR_ARS
	DS_ARS,
#endif
#if CONFIG_DMR_IP_GATEWAY
	DS_GATEWAY,
#endif
	DS_COUNT };
static const uint8_t ds_setting[] = {
#if CONFIG_DMR_ARS
	[DS_ARS] = UI_SET_ARS_SERVER,
#endif
#if CONFIG_DMR_IP_GATEWAY
	[DS_GATEWAY] = UI_SET_IP_GATEWAY,
#endif
};
// The typing and the ID just saved share My radio's (dmr_id_*): the two screens are never open together, and RAM is
// short. dmr_id_editing is unused here (ds_row says which row is typed), and dmr_id_serial holds the save's time
static uint8_t ds_row = DS_COUNT;	// the row being typed (DS_COUNT: none)
static uint8_t ds_saved_row = DS_COUNT;	// the row just saved: its ID shows until the radio has taken it

static uint16_t ds_count(void) { return DS_COUNT; }

// A save that failed: the stored ID shows again
static void ds_forget(void)
{
	ds_saved_row = DS_COUNT;
}

static void ds_item(uint16_t i, ui_item_t *it)
{
	static const tr_id_t labels[] = {
#if CONFIG_DMR_ARS
		[DS_ARS] = TRN(DATA_LABEL_ARS_SERVER, "ARS server"),
#endif
#if CONFIG_DMR_IP_GATEWAY
		[DS_GATEWAY] = TRN(DATA_LABEL_IP_GATEWAY, "IP gateway"),
#endif
	};
	sbuf_t b;
	sb_init(&b, it->value, sizeof it->value);
	str_copy(it->label, sizeof it->label, tr(labels[i]));
	if (ds_row == i) {
		if (dmr_id_len > 0)
			sb_u32(&b, dmr_id_value, 1);
		sb_chr(&b, '_');
		it->flags |= UI_ITEM_EDIT;
		return;
	}
	uint32_t v = (uint32_t)ui_svc_setting_get(ds_setting[i]);
	if (ds_saved_row == i && v != dmr_id_saved && hal_millis() - dmr_id_serial < 3000u)
		v = dmr_id_saved;	// the codeplug write and the radio core's reload take a moment
	if (v != 0)
		sb_u32(&b, v, 1);
	else
		sb_str(&b, TR(VALUE_OFF, "Off"));
}

static const char *ds_body(void)
{
	sbuf_t b;
	sb_init(&b, id_body, sizeof id_body);
	sb_str(&b, TR(BODY_SAVED_IN_CODEPLUG, "Saved in the codeplug"));
#if CONFIG_DMR_ARS
	static const tr_id_t states[] = { [UI_ARS_OFF] = TRN(VALUE_OFF, "Off"),
		[UI_ARS_REGISTERING] = TRN(DATA_STATE_REGISTERING, "Registering"),
		[UI_ARS_REGISTERED] = TRN(DATA_STATE_REGISTERED, "Registered"),
		[UI_ARS_REFUSED] = TRN(DATA_STATE_REFUSED, "Refused"),
		[UI_ARS_NO_ANSWER] = TRN(DATA_STATE_NO_ANSWER, "No answer") };
	uint8_t s = ui_svc_ars_state();
	sb_chr(&b, '\n');
	sb_str(&b, TR(DATA_BODY_ARS, "ARS: "));
	sb_str(&b, tr(states[s <= UI_ARS_NO_ANSWER ? s : UI_ARS_OFF]));
#endif
	return id_body;
}

static void ds_begin(uint16_t row)
{
	ds_row = (uint8_t)row;
	dmr_id_len = 0;
	dmr_id_value = 0;
}

static void ds_save(void)
{
	uint8_t row = ds_row;
	ds_row = DS_COUNT;
	if (dmr_id_len == 0)
		return;		// nothing typed: as it was
	if (!ui_svc_setting_set(ds_setting[row], (int32_t)dmr_id_value)) {
		ui_toast(TR(TOAST_SAVE_FAILED, "Couldn't save: try again"));
		return;
	}
	ds_saved_row = row;
	dmr_id_saved = dmr_id_value;
	dmr_id_serial = hal_millis();
	ui_toast(TR(TOAST_SAVED, "Saved"));
}

static void ds_select(uint16_t i, bool hold)
{
	(void)hold;
	if (i >= DS_COUNT)
		return;
	if (ds_row == i)
		ds_save();
	else
		ds_begin(i);
}

static bool ds_key(const input_event_t *ev)
{
	bool press = ev->gesture == GESTURE_PRESS || ev->gesture == GESTURE_LONG;
	uint16_t focus = ui_frame()->focus;
	if (ev->action >= ACT_DIGIT_0 && ev->action <= ACT_DIGIT_9 && press && focus < DS_COUNT) {
		if (ds_row != focus)
			ds_begin(focus);
		uint32_t v = dmr_id_value * 10u + (uint32_t)(ev->action - ACT_DIGIT_0);
		if (v > CODEPLUG_DMR_ID_MAX) {
			ui_toast(TR(TOAST_ID_MAX_PRIVATE, "IDs go up to 16776415"));
			return true;
		}
		dmr_id_value = v;
		dmr_id_len++;
		ui_dirty();
		return true;
	}
	if (ds_row == DS_COUNT)
		return false;
	if (ev->action == ACT_BACK && ev->gesture == GESTURE_PRESS) {
		if (dmr_id_len > 0) {
			dmr_id_len--;
			dmr_id_value /= 10u;
		} else {
			ds_row = DS_COUNT;
		}
		ui_dirty();
		return true;
	}
	if (ev->action == ACT_NAV_UP || ev->action == ACT_NAV_DOWN || ev->action == ACT_ENC_STEP)
		ds_row = DS_COUNT;	// another row: what was typed is dropped
	return false;
}

static void ds_enter(void)
{
	ds_row = DS_COUNT;
	ds_saved_row = DS_COUNT;
}

static const ui_scr_def_t ds_screen = {
	.name = "data-services", .flags = SF_LIST, .prefix = TRN(DATA_TITLE, "Data Services"),
	.hint = TRN(HINT_TYPE_OK_EDIT_SAVE_BACK, "0-9 type  OK edit/save  Back"),
	.count = ds_count, .item = ds_item, .select = ds_select, .key = ds_key, .enter = ds_enter, .body = ds_body,
};
#endif

#if CONFIG_REF_TRIM
// ======================= Freq trim (Settings > Advanced) =======================
// The reference trim (radio/ref_trim.h; radio-features.md 6.1), drawn as a volume-style page: the value in large type
// over a slider from -10 to +10 ppm, then what it moves a carrier by at 440 and 144 MHz. Up / Down step 0.1 ppm (0.5
// held) and are saved at once (a board that retunes for it does so once the trim holds still), so a counter or a
// hotspot shows each step; # flips the sign. Digits type the size in tenths as a calculator does (4 5 = 4.5), keeping
// the sign; OK sets it, OK with nothing typed leaves

static int32_t trim_typed;		// tenths of a ppm typed so far
static bool trim_typing, trim_neg;	// digits typed; the typed value's sign

static int32_t trim_now(void)
{
	return trim_typing ? (trim_neg ? -trim_typed : trim_typed) * 100 : ui_svc_setting_get(UI_SET_REF_TRIM);
}

static void trim_store(int32_t ppb)
{
	ppb = ref_trim_clamp(ppb);
	if (ui_svc_setting_set(UI_SET_REF_TRIM, ppb))
		ui_svc_apply(UI_SET_REF_TRIM, ppb);
	else
		ui_toast(TR(TOAST_SAVE_FAILED, "Couldn't save: try again"));
}

// "-4.5", "+1.2", "0.0": the sign always shown but on 0 (a typed minus with no digits yet shows "-0.0")
static void sb_ppm_num(sbuf_t *b, int32_t ppb, bool minus)
{
	int32_t t = (ppb < 0 ? -ppb : ppb) / 100;
	if (ppb < 0 || minus)
		sb_chr(b, '-');
	else if (ppb > 0)
		sb_chr(b, '+');
	sb_u32(b, (uint32_t)t / 10u, 1);
	sb_chr(b, '.');
	sb_u32(b, (uint32_t)t % 10u, 1);
}

#if CONFIG_UI_SETTINGS
static void sb_ppm(sbuf_t *b, int32_t ppb)
{
	sb_ppm_num(b, ppb, false);
	sb_str(b, " ppm");
}
#endif

// One line: the label at the left (dim), the value at the right
static void trim_line(const font_t *f, int y, const char *label, const char *value)
{
	int w = gfx_width();
	gfx_text(f, 2, y, label, ui_col.dim);
	gfx_text(f, w - 2 - gfx_text_width(f, value), y, value, ui_col.fg);
}

static void trim_draw(void)
{
	const ui_layout_t *L = &ui_layout;
	int w = gfx_width();
	int32_t ppb = trim_now();
	char buf[24];
	sbuf_t b;
	gfx_clear(ui_col.bg);
	int y = ui_draw_top(ui_view(), &ui_st, TR(TRIM_TITLE, "Freq Trim"), NULL) + 2;
	bool roomy = L->hint_y - y >= L->large->h + 3 * L->medium->h + 24;	// room for the scale and the hint line

	// The value, centered: the number large, " ppm" smaller on its baseline; typing shows a cursor
	sb_init(&b, buf, sizeof buf);
	sb_ppm_num(&b, ppb, trim_typing && trim_neg && trim_typed == 0);
	if (trim_typing)
		sb_chr(&b, '_');
	const font_t *big = L->large, *unit = L->medium;
	int nw = gfx_text_width(big, buf), uw = gfx_text_width(unit, " ppm");
	int x = (w - nw - uw) / 2;
	gfx_text(big, x, y, buf, trim_typing ? ui_col.focus : ui_col.fg);
	gfx_text(unit, x + nw, y + big->h - unit->h, " ppm", ui_col.dim);
	y += big->h + (roomy ? 6 : 3);

	// The slider: a track from -max to +max, a tick at 0, the span from 0 to the value filled, a knob at the value
	int x0 = 6, x1 = w - 7, cx = (x0 + x1) / 2, half = (x1 - x0) / 2;
	int th = roomy ? 6 : 4, kh = th + (roomy ? 8 : 4), ty = y + (kh - th) / 2;
	int kx = cx + (int)((int32_t)half * ppb / REF_TRIM_MAX_PPB);
	gfx_rect(x0, ty, x1 - x0 + 1, th, ui_col.line);
	if (kx != cx)
		gfx_fill(kx < cx ? kx : cx, ty, (kx < cx ? cx - kx : kx - cx) + 1, th, ui_col.focus);
	gfx_vline(cx, y, kh, ui_col.dim);
	gfx_vline(x0, y + 1, kh - 2, ui_col.line);
	gfx_vline(x1, y + 1, kh - 2, ui_col.line);
	int kw = roomy ? 6 : 3;
	gfx_round_fill(kx - kw / 2, y, kw, kh, ui_col.fg);
	y += kh + 1;
	if (roomy) {		// the scale under it
		const font_t *t = L->small;
		gfx_text(t, x0, y, "-10", ui_col.dim);
		gfx_text(t, cx - gfx_text_width(t, "0") / 2, y, "0", ui_col.dim);
		gfx_text(t, x1 + 1 - gfx_text_width(t, "+10"), y, "+10", ui_col.dim);
		y += t->h + 4;
	}

	// What it comes to at the two bands' calling frequencies
	const font_t *f = roomy ? L->medium : L->small;
	static const uint16_t mhz[2] = { 440, 144 };
	for (int i = 0; i < 2 && y + f->h <= L->hint_y; i++) {
		int32_t hz = ref_trim_hz_at(ppb, mhz[i]);
		sb_init(&b, buf, sizeof buf);
		if (hz > 0)
			sb_chr(&b, '+');
		sb_i32(&b, hz);
		sb_str(&b, " Hz");
		// "At 440 MHz" wherever it fits beside the widest value (-4400 Hz), "@ 440 MHz" where it doesn't
		char label[20];	// "At 440 MHz", room for a longer translation
		sb_init(&b, label, sizeof label);
		const char *at = TR(TRIM_BODY_AT, "At ");
		sb_str(&b, gfx_text_width(f, at) + gfx_text_width(f, "440 MHz -4400 Hz") + 4 <= w ? at : "@ ");
		sb_u32(&b, mhz[i], 1);
		sb_str(&b, " MHz");
		trim_line(f, y, label, buf);
		y += f->h;
	}
	// which way the sign goes, shortened only where it must be
	const char *how = TR(TRIM_BODY_PLUS_RAISES, "+ to increase frequency");
	if (gfx_text_width(L->small, how) > w)
		how = TR(TRIM_BODY_PLUS_RAISES_SHORT, "+ to increase freq");
	if (y + 2 + L->small->h <= L->hint_y && gfx_text_width(L->small, how) <= w)
		gfx_text(L->small, (w - gfx_text_width(L->small, how)) / 2, L->hint_y - L->small->h - 1, how, ui_col.dim);
	ui_draw_bottom(&ui_st, trim_typing ? TR(TRIM_HINT_TYPING, "0-9 type  # +/-  OK set") :
		TR(TRIM_HINT, "Up Down  # +/-  OK done"));
}

static void trim_set_typed(void)
{
	trim_typing = false;
	trim_store((trim_neg ? -trim_typed : trim_typed) * 100);
}

static bool trim_key(const input_event_t *ev)
{
	bool press = ev->gesture == GESTURE_PRESS || ev->gesture == GESTURE_LONG;
	int dir = ev->action == ACT_NAV_UP || ev->action == ACT_NAV_RIGHT ? 1 :
		ev->action == ACT_NAV_DOWN || ev->action == ACT_NAV_LEFT ? -1 :
		ev->action == ACT_ENC_STEP ? (ev->value > 0 ? 1 : -1) : 0;
	if (dir != 0 && (press || ev->gesture == GESTURE_REPEAT || ev->action == ACT_ENC_STEP)) {
		if (trim_typing)
			trim_set_typed();
		// A held key steps 0.5 ppm; a knob event carries its detents (up to 8, as the other number editors)
		int n = ev->action != ACT_ENC_STEP ? 1 : ev->value < -8 || ev->value > 8 ? 8 : ev->value < 0 ? -ev->value : ev->value;
		int32_t step = ev->gesture == GESTURE_REPEAT ? 5 * REF_TRIM_STEP_PPB : n * REF_TRIM_STEP_PPB;
		trim_store(ui_svc_setting_get(UI_SET_REF_TRIM) + dir * step);
		ui_dirty();
		return true;
	}
	if (ev->action == ACT_CONTEXT && press) {		// #: the other sign
		int32_t v = ui_svc_setting_get(UI_SET_REF_TRIM);
		if (trim_typing) {
			trim_neg = !trim_neg;
		} else if (v == 0) {
			trim_typing = true;		// a minus to type after (# 4 5 OK = -4.5)
			trim_typed = 0;
			trim_neg = true;
		} else {
			trim_store(-v);
		}
		ui_dirty();
		return true;
	}
	if (ev->action >= ACT_DIGIT_0 && ev->action <= ACT_DIGIT_9 && press) {
		if (!trim_typing) {
			trim_typing = true;
			trim_typed = 0;
			trim_neg = ui_svc_setting_get(UI_SET_REF_TRIM) < 0;
		}
		int32_t v = trim_typed * 10 + (ev->action - ACT_DIGIT_0);
		if (v * 100 > REF_TRIM_MAX_PPB) {
			ui_toast(TR(TRIM_TOAST_MAX, "Up to 10.0 ppm"));
			return true;
		}
		trim_typed = v;
		ui_dirty();
		return true;
	}
	if (ev->action == ACT_SELECT && press) {
		if (trim_typing)
			trim_set_typed();
		else
			ui_pop();
		ui_dirty();
		return true;
	}
	if (trim_typing && ev->action == ACT_BACK && ev->gesture == GESTURE_PRESS) {
		if (trim_typed > 0)
			trim_typed /= 10;	// takes the last digit back
		else
			trim_typing = false;
		ui_dirty();
		return true;
	}
	return false;
}

static void trim_enter(void)
{
	trim_typing = false;
}

static const ui_scr_def_t trim_screen = {
	.name = "freq-trim", .flags = SF_FIELD, .key = trim_key, .draw = trim_draw, .enter = trim_enter,
};
#endif

// ======================= About (Settings > Advanced) =======================
// A text page, every word shown (nothing shortened), no status strip while idle: the release and the commit,
// RadioFoundry's license, the maintainers, where the contributors are listed, the third-party code and fonts in this
// image and the codec. From AUTHORS and platform/<platform>/THIRD_PARTY, through the header make generates
// (tools/gen_about.py)

static const char *const about_authors[] = RFOUNDRY_ABOUT_AUTHORS;
static const char *const about_third[][2] = RFOUNDRY_ABOUT_THIRD_PARTY;
static uint16_t about_first;	// first line shown
static uint16_t about_max;	// the largest first line (from the last draw)

// The text's bytes: the two lists make generated, this build's font lines, and 296 for the rest (the version lines,
// the labels, and 35 for a translation's longer ones). A fixed size cut the last lines off where the lists are long
#define ABOUT_TEXT_MAX (296u + RFOUNDRY_ABOUT_LIST_BYTES + (CONFIG_FONT_AA ? 32u : 0u) + (CONFIG_LANG_ZH ? 48u : 0u))

static const char *about_text(void)
{
	static char t[ABOUT_TEXT_MAX];
	sbuf_t b;
	sb_init(&b, t, sizeof t);
	// every line "Label: value" (ui_draw_text_page shows labels dim), each on its own line: "RadioFoundry v26.11" was
	// wider than the display and the release wrapped without its label. A release: its name, then the commit it was
	// built from; any other build: the commit alone
	sb_str(&b, TR(ABOUT_BODY_FIRMWARE, "Firmware: "));
	sb_str(&b, "RadioFoundry\n");
	if (RFOUNDRY_RELEASE[0] != '\0') {
		sb_str(&b, TR(ABOUT_BODY_RELEASE, "Release: "));
		sb_str(&b, RFOUNDRY_RELEASE);
		sb_chr(&b, '\n');
	}
	sb_str(&b, TR(ABOUT_BODY_BUILD, "Build: "));
	sb_str(&b, RFOUNDRY_GIT);
	// the commit's date (UTC; a dirty build's: the build's), which the web editor's update notice compares with a
	// release's
	if (RFOUNDRY_GIT_DATE[0] != '\0') {
		sb_chr(&b, '\n');
		sb_str(&b, TR(ABOUT_BODY_DATE, "Date: "));
		sb_str(&b, RFOUNDRY_GIT_DATE);
	}
	sb_chr(&b, '\n');
	sb_str(&b, TR(BODY_RADIO, "Radio: "));
	sb_str(&b, board.name);
	sb_chr(&b, '\n');
	sb_str(&b, TR(ABOUT_BODY_LICENSE, "License: "));
	sb_str(&b, "MIT, (c) 2026 The RadioFoundry Authors\n");
	// The authors (the copyright line above) are everyone who has contributed: too many to list here, so the page
	// names the maintainers (AUTHORS) and says where the contributors are listed. One maintainer per line, in AUTHORS
	// order: a comma-joined list wraps mid-name on every display
	sb_str(&b, RFOUNDRY_ABOUT_AUTHOR_COUNT > 1 ? TR(ABOUT_BODY_MAINTAINERS, "Maintainers:") :
		TR(ABOUT_BODY_MAINTAINER, "Maintainer:"));
	for (unsigned i = 0; i < RFOUNDRY_ABOUT_AUTHOR_COUNT; i++) {
		sb_chr(&b, RFOUNDRY_ABOUT_AUTHOR_COUNT > 1 ? '\n' : ' ');
		sb_str(&b, about_authors[i]);
	}
	sb_chr(&b, '\n');
	sb_str(&b, TR(ABOUT_BODY_CONTRIBUTORS, "Contributors:"));
	sb_str(&b, "\ngithub.com/radiofoundry");	// on its own line, as a maintainer is: it never wraps after the label
	for (unsigned i = 0; i < RFOUNDRY_ABOUT_THIRD_PARTY_COUNT; i++) {
		sb_chr(&b, '\n');
		sb_str(&b, about_third[i][0]);
		sb_str(&b, ": ");
		sb_str(&b, about_third[i][1]);
	}
	sb_chr(&b, '\n');
	sb_str(&b, TR(ABOUT_BODY_FONT_SPLEEN, "Spleen font: BSD-2-Clause"));	// core/ui/fonts.c
#if CONFIG_FONT_AA
	sb_chr(&b, '\n');
	sb_str(&b, TR(ABOUT_BODY_FONT_JETBRAINS, "JetBrains Mono font: OFL-1.1"));
#endif
#if CONFIG_LANG_ZH
	sb_chr(&b, '\n');
	sb_str(&b, TR(ABOUT_BODY_FONT_NOTO_CJK, "Noto Sans CJK SC font: OFL-1.1"));	// core/ui/cjk_font_data.c
#endif
	sb_chr(&b, '\n');
	// from the radio's own firmware, not part of RadioFoundry
	sb_str(&b, TR(ABOUT_BODY_CODEC_AMBE, "AMBE+2 codec: DVSI (stock)"));
	return t;
}

static void about_enter(void)
{
	about_first = 0;
}

static void about_draw(void)
{
	about_max = (uint16_t)ui_draw_text_page(ui_view(), &ui_st, TR(TITLE_ABOUT, "About"), about_text(), &about_first,
		true);
}

static bool about_key(const input_event_t *ev)
{
	return text_page_key(ev, &about_first, about_max);
}

static const ui_scr_def_t about_screen = {
	.name = "about", .flags = SF_FIELD | SF_NO_STRIP, .key = about_key, .draw = about_draw, .enter = about_enter,
};

// ======================= Backlight tests (Settings > Advanced > Backlight tests) =======================

#if CONFIG_BACKLIGHT_TEST
// A bench tool (docs/testing/backlight-curve.md), so its text is untranslated. OK steps through the pages the board
// has; each draws the boot splash with its reading.
// 1, the PWM compare value (the board's raw hooks): the knob steps 1; Up / Down 1 on a knob with end stops, else 50;
//    digits type a value, applied at as many digits as the maximum has (# applies fewer, BACK takes one back); * the
//    rate; OK held flips to full and back. Shows the duty and the on and off time a period.
// 2, the brightness, 0-100 % in 10 % steps: kept as the setting on leaving it (10 % for 0).
// 3, the PWM rate at that brightness (the board's rates): the standard one again on leaving it.
enum { BT_RAW, BT_LEVEL, BT_RATE, BT_COUNT };
static uint8_t bt_page, bt_pct, bt_rate, bt_ndig;
static bool bt_full;		// page 1: OK held shows full, to compare the value against
static uint32_t bt_hz, bt_typed;
static uint16_t bt_raw;

static const hal_display_t *bt_disp(void) { return board.display; }

static bool bt_has_raw(void)
{
	const hal_display_t *d = bt_disp();
	return d->set_backlight_raw != NULL && d->backlight_raw != NULL && d->backlight_raw_max != 0;
}

static bool bt_has_rate(void)
{
	return bt_has_raw() && bt_disp()->set_backlight_rate != NULL && bt_disp()->backlight_rates > 1;
}

static bool bt_page_ok(uint8_t p)
{
	return p == BT_LEVEL || (p == BT_RAW && bt_has_raw()) || (p == BT_RATE && bt_has_rate());
}

static uint8_t bt_std(void)
{
	return bt_disp()->backlight_rate_std < bt_disp()->backlight_rates ? bt_disp()->backlight_rate_std : 0;
}

static void bt_level(uint8_t pct)
{
	if (bt_disp()->set_backlight != NULL)
		bt_disp()->set_backlight(pct);
}

static void bt_set_rate(uint8_t i)
{
	bt_rate = i;
	bt_hz = bt_has_rate() ? bt_disp()->set_backlight_rate(i) : 0;
}

// The digits the maximum compare value has: a typed value is applied at that many
static uint8_t bt_max_digits(void)
{
	uint8_t n = 1;
	for (uint32_t v = bt_disp()->backlight_raw_max; v >= 10u; v /= 10u)
		n++;
	return n;
}

static void bt_set_raw(uint32_t v)
{
	uint32_t max = bt_disp()->backlight_raw_max;
	bt_raw = (uint16_t)(v > max ? max : v);
	bt_full = false;
	bt_disp()->set_backlight_raw(bt_raw);
}

// Leaving a page (to the next, or off the screen)
static void bt_end(void)
{
	bt_ndig = 0;
	bt_full = false;
	if (bt_page == BT_LEVEL) {
		if (bt_pct == 0)
			bt_pct = 10;
		ui_backlight_hold(bt_pct);
		ui_st.brightness = bt_pct;
		ui_svc_setting_set(UI_SET_BRIGHTNESS, bt_pct);
	} else if (bt_rate != bt_std()) {
		bt_set_rate(bt_std());
	}
	bt_level(bt_pct);	// the level again over a raw value
}

static void bt_begin(void)
{
	if (bt_page == BT_RAW) {
		bt_raw = bt_disp()->backlight_raw();
		if (bt_has_rate())
			bt_set_rate(bt_std());
	} else if (bt_page == BT_RATE) {
		bt_set_rate(bt_std());
	}
}

static void bt_enter(void)
{
	bt_pct = ui_st.brightness;
	ui_backlight_hold(bt_pct);
	bt_rate = bt_has_rate() ? bt_std() : 0;
	bt_hz = 0;
	bt_ndig = 0;
	bt_full = false;
	bt_page = BT_RAW;
	while (!bt_page_ok(bt_page))
		bt_page++;
	bt_begin();
}

static void bt_leave(void)
{
	bt_end();
	ui_backlight_hold(-1);
}

static void bt_draw(void)
{
	char a[24], b[24];
	sbuf_t sa, sb;
	sb_init(&sa, a, sizeof a);
	sb_init(&sb, b, sizeof b);
	if (bt_page == BT_LEVEL) {
		sb_str(&sa, "Brightness");
		sb_u32(&sb, bt_pct, 1);
		sb_chr(&sb, '%');
	} else if (bt_page == BT_RATE) {
		sb_str(&sa, "PWM rate");
		sb_u32(&sb, bt_hz, 1);
		sb_str(&sb, " Hz");
	} else {
		// "7324 Hz CV 2137" / "52.1% 71.2/65.3us" (on / off time a period); "CV 21_" while typing; "full" held
		uint32_t max = bt_disp()->backlight_raw_max;
		uint16_t raw = bt_full ? (uint16_t)max : bt_raw;
		if (bt_hz != 0) {
			sb_u32(&sa, bt_hz, 1);
			sb_str(&sa, " Hz ");
		}
		sb_str(&sa, "CV ");
		if (bt_ndig != 0) {
			sb_u32(&sa, bt_typed, bt_ndig);
			sb_chr(&sa, '_');
		} else {
			sb_u32(&sa, raw, 1);
		}
		if (bt_full) {
			sb_str(&sb, "full");
		} else {
			uint32_t pm = (uint32_t)raw * 1000u / max;	// duty, 0.1 %
			sb_u32(&sb, pm / 10u, 1);
			sb_chr(&sb, '.');
			sb_u32(&sb, pm % 10u, 1);
			sb_chr(&sb, '%');
			if (bt_hz != 0) {
				uint32_t period = 1000000000u / bt_hz;	// ns
				uint32_t on = (uint32_t)((uint64_t)raw * 1000000000u / ((uint64_t)bt_hz * max));
				uint32_t off = period > on ? period - on : 0;
				sb_chr(&sb, ' ');
				sb_u32(&sb, on / 1000u, 1);
				sb_chr(&sb, '.');
				sb_u32(&sb, on % 1000u / 100u, 1);
				sb_chr(&sb, '/');
				sb_u32(&sb, off / 1000u, 1);
				sb_chr(&sb, '.');
				sb_u32(&sb, off % 1000u / 100u, 1);
				sb_str(&sb, "us");
			}
		}
	}
	ui_draw_splash(board.name, RFOUNDRY_RELEASE[0] != '\0' ? RFOUNDRY_RELEASE : RFOUNDRY_GIT, a, b);
}

// A step from the knob (1) or Up / Down: 1 on a knob with end stops, which can't turn far, else 50
static int bt_step(const input_event_t *ev)
{
	if (ev->action == ACT_ENC_STEP)
		return ev->value > 0 ? 1 : ev->value < 0 ? -1 : 0;
	if (ev->gesture != GESTURE_PRESS && ev->gesture != GESTURE_REPEAT)
		return 0;
	int n = bt_page == BT_RAW && !(board.input != NULL && board.input->encoder_limited) ? 50 : 1;
	return ev->action == ACT_NAV_UP ? n : ev->action == ACT_NAV_DOWN ? -n : 0;
}

static bool bt_raw_key(const input_event_t *ev)
{
	bool press = ev->gesture == GESTURE_PRESS;
	if (ev->action >= ACT_DIGIT_0 && ev->action <= ACT_DIGIT_9 && press) {
		bt_typed = (bt_ndig == 0 ? 0 : bt_typed * 10u) + (uint32_t)(ev->action - ACT_DIGIT_0);
		if (++bt_ndig >= bt_max_digits()) {
			bt_ndig = 0;
			bt_set_raw(bt_typed);
		}
		return true;
	}
	if (bt_ndig != 0 && press && ev->action == ACT_CONTEXT) {	// #: apply what is typed
		bt_ndig = 0;
		bt_set_raw(bt_typed);
		return true;
	}
	if (bt_ndig != 0 && press && ev->action == ACT_BACK) {		// a digit back
		bt_typed /= 10u;
		bt_ndig--;
		return true;
	}
	if (ev->action == ACT_SELECT && ev->gesture == GESTURE_LONG) {
		bt_ndig = 0;
		bt_full = !bt_full;
		bt_disp()->set_backlight_raw(bt_full ? bt_disp()->backlight_raw_max : bt_raw);
		return true;
	}
	if (ev->action == ACT_QUICK && press && bt_has_rate()) {
		bt_set_rate((uint8_t)((bt_rate + 1u) % bt_disp()->backlight_rates));
		return true;
	}
	int d = bt_step(ev);
	if (d == 0)
		return false;
	bt_ndig = 0;
	int32_t v = (int32_t)bt_raw + d;
	bt_set_raw(v < 0 ? 0u : (uint32_t)v);
	return true;
}

static bool bt_key(const input_event_t *ev)
{
	bool used;
	if (ev->action == ACT_SELECT && ev->gesture == GESTURE_PRESS) {
		bt_end();
		do {
			bt_page = (uint8_t)((bt_page + 1u) % BT_COUNT);
		} while (!bt_page_ok(bt_page));
		bt_begin();
		used = true;
	} else if (bt_page == BT_RAW) {
		used = bt_raw_key(ev);
	} else {
		int d = bt_step(ev);
		used = d != 0;
		if (used && bt_page == BT_LEVEL) {
			int p = bt_pct + d * 10;
			bt_pct = (uint8_t)(p < 0 ? 0 : p > 100 ? 100 : p);
			ui_backlight_hold(bt_pct);
			bt_level(bt_pct);
		} else if (used) {
			int r = bt_rate + d, k = bt_disp()->backlight_rates;
			bt_set_rate((uint8_t)(r < 0 ? 0 : r >= k ? k - 1 : r));
		}
	}
	if (used)
		ui_dirty();
	return used;
}

static const ui_scr_def_t bl_test_screen = {
	.name = "bltest", .flags = 0, .key = bt_key, .draw = bt_draw, .enter = bt_enter, .leave = bt_leave,
};
#endif

// ======================= Roaming (Tools > Roaming, docs/spec/dmr-roaming.md 6.2) =======================

enum { RM_ZONE, RM_NOW, RM_LOCK, RM_SURVEY, RM_BEACON, RM_LEN, RM_PROBE, RM_COUNT };

#if CONFIG_DMR_ROAMING
// The survey and beacon rows: radio-wide settings, cycled with OK (the values must match the repeaters)
static const int16_t rm_survey_v[] = { 1, 2, 0 };
static const tr_id_t rm_survey_n[] = { TRN(ROAM_VALUE_SYNCED, "Synced"), TRN(ROAM_VALUE_PER_SITE, "Per site"),
	TRN(VALUE_OFF, "Off") };
static const int16_t rm_beacon_v[] = { 60, 120, 300, 600, 0, 10, 15, 20, 30 };
static const tr_id_t rm_beacon_n[] = { TRX(VALUE_60_S, "60 s"), TRN(VALUE_2_MIN, "2 min"), TRN(VALUE_5_MIN, "5 min"),
	TRN(VALUE_10_MIN, "10 min"), TRN(VALUE_NONE, "None"), TRX(VALUE_10_S, "10 s"), TRX(VALUE_15_S, "15 s"),
	TRX(VALUE_20_S, "20 s"), TRX(VALUE_30_S, "30 s") };
static const int16_t rm_len_v[] = { 4, 5, 6, 8, 10, 1, 2, 3 };
static const tr_id_t rm_len_n[] = { TRX(VALUE_4_S, "4 s"), TRX(VALUE_5_S, "5 s"), TRX(VALUE_6_S, "6 s"),
	TRX(VALUE_8_S, "8 s"), TRX(VALUE_10_S, "10 s"), TRX(VALUE_1_S, "1 s"), TRX(VALUE_2_S, "2 s"),
	TRX(VALUE_3_S, "3 s") };

typedef struct {
	uint8_t id;		// ui_setting_t
	const int16_t *v;
	const tr_id_t *n;
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
		return TR(ROAM_WHY_CHANNELS_NOT_VFO, "Roaming works on channels, not the VFO.");
	if (!v->roam.zone_on)
		return TR(ROAM_WHY_OFF_IN_ZONE, "Off in this zone. On, the radio moves between the zone's repeaters.");
	if (roam_active(v))
		return NULL;
	if (v->ch.mode != RADIO_MODE_DMR || v->ch.dmo || v->ch.rx_only || v->ch.tx_hz == v->ch.rx_hz)
		return TR(ROAM_WHY_NOT_A_SITE, "This channel is not a repeater site.");
	return TR(ROAM_WHY_NEEDS_TWO_SITES, "Needs two sites in the zone with the same talkgroup.");
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
		sb_str(&b, TR(ROAM_BODY_SEARCHING, "Searching sites..."));
		return t;
	}
	sb_str(&b, TR(ROAM_BODY_SITE, "Site "));
	sb_u32(&b, v->roam.site + 1u, 1);
	sb_str(&b, TR(PART_OF, " of "));
	sb_u32(&b, v->roam.sites, 1);
	sb_str(&b, ": ");
	sb_str(&b, v->ch.name);
	if (v->roam.rssi_dbm != 0) {
		sb_chr(&b, '\n');
		sb_str(&b, TR(ROAM_BODY_SIGNAL, "Signal "));
		sb_i32(&b, v->roam.rssi_dbm);
		sb_str(&b, v->roam.weak ? TR(ROAM_BODY_DBM_WEAK, " dBm, weak") : " dBm");
	}
	if (v->roam.state == RROAM_STATE_LOCKED) {
		sb_chr(&b, '\n');
		sb_str(&b, TR(ROAM_BODY_LOCKED, "Locked to this site"));
	} else if (ui_svc_setting_get(UI_SET_ROAM_SURVEY) == 1 && ui_svc_setting_get(UI_SET_ROAM_BEACON_S) != 0) {
		sb_chr(&b, '\n');
		sb_str(&b, v->roam.win_conf >= 2 ? TR(ROAM_BODY_BEACONS_SURVEYING, "Beacons: surveying") :
			TR(ROAM_BODY_BEACONS_LEARNING, "Beacons: learning"));
	}
#else
	sb_str(&b, TR(WHY_ROAMING_NOT_IN_BUILD, "Roaming: not in this build"));
#endif
	return t;
}

static uint16_t roam_count(void) { return RM_COUNT; }

static void roam_item(uint16_t i, ui_item_t *it)
{
	static const tr_id_t names[] = { TRN(ROAM_LABEL_IN_THIS_ZONE, "Roam in this zone"),
		TRN(ROAM_ROW_ROAM_NOW, "Roam now"), TRN(ROAM_LABEL_LOCK_TO_SITE, "Lock to this site"),
		TRN(ROAM_LABEL_SITE_SURVEY, "Site survey"), TRN(ROAM_LABEL_BEACON_INTERVAL, "Beacon interval"),
		TRN(ROAM_LABEL_BEACON_LENGTH, "Beacon length"), TRN(ROAM_LABEL_WAKE_PROBE, "Wake probe") };
	str_copy(it->label, sizeof it->label, tr(names[i]));
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
			sb_str(&b, tr(l->n[k]));
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
		str_copy(it->value, sizeof it->value, ui_svc_setting_get(UI_SET_ROAM_PROBE) ? TR(VALUE_ON, "On") :
			TR(VALUE_OFF, "Off"));
		return;
	}
	if (i == RM_ZONE)
		str_copy(it->value, sizeof it->value, v->roam.zone_on ? TR(VALUE_ON, "On") : TR(VALUE_OFF, "Off"));
	else if (i == RM_LOCK)
		str_copy(it->value, sizeof it->value, v->roam.state == RROAM_STATE_LOCKED ? TR(VALUE_ON, "On") :
			TR(VALUE_OFF, "Off"));
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
			ui_toast(TR(ROAM_TOAST_MUST_MATCH_REPEATERS, "Must match the repeaters"));
		return;
	}
	if (i == RM_PROBE) {
		bool on = !ui_svc_setting_get(UI_SET_ROAM_PROBE);
		if (ui_svc_setting_set(UI_SET_ROAM_PROBE, on))
			ui_svc_apply(UI_SET_ROAM_PROBE, on);
		ui_toast(on ? TR(ROAM_TOAST_WAKE_PROBE_ON, "Wake probe on: transmits on silent sites") :
			TR(ROAM_TOAST_WAKE_PROBE_OFF, "Wake probe off: listening only"));
		return;
	}
	if (i == RM_ZONE) {
		if (v->vfo) {
			ui_toast(TR(ROAM_TOAST_CHANNELS_ONLY, "Roaming works on channels"));
			return;
		}
		bool on = !v->roam.zone_on;
		ui_svc_roam(RROAM_ZONE_TOGGLE);
		if (on) {
			sb_str(&b, TR(ROAM_TOAST_ON_FOR, "Roaming on for "));
			sb_str(&b, v->zone_name);
			ui_toast(msg);
		} else {
			ui_toast(TR(ROAM_TOAST_OFF, "Roaming off"));
		}
		return;
	}
	if (why != NULL) {
		ui_toast(!v->roam.zone_on ? TR(ROAM_TOAST_TURN_ON_FIRST, "Turn roaming on first") :
			TR(ROAM_TOAST_CANT_ON_CHANNEL, "Can't roam on this channel"));
		return;
	}
	if (i == RM_NOW) {
		if (v->roam.state == RROAM_STATE_SEARCH) {
			ui_toast(TR(ROAM_TOAST_ALREADY_SEARCHING, "Already searching"));
			return;
		}
		ui_svc_roam(RROAM_NOW);
		ui_go_home();
		ui_toast(TR(ROAM_TOAST_SEARCHING, "Searching sites"));
	} else if (i == RM_LOCK) {
		bool lock = v->roam.state != RROAM_STATE_LOCKED;
		ui_svc_roam(lock ? RROAM_LOCK : RROAM_UNLOCK);
		if (lock) {
			sb_str(&b, TR(ROAM_TOAST_LOCKED_TO, "Locked to "));
			sb_str(&b, v->ch.name);
			ui_toast(msg);
		} else {
			ui_toast(TR(ROAM_TOAST_ROAMING_AGAIN, "Roaming again"));
		}
	}
#else
	(void)i;
	ui_toast(TR(WHY_ROAMING_NOT_IN_BUILD, "Roaming: not in this build"));
#endif
}

static const ui_scr_def_t roam_screen = {
	.name = "roaming", .flags = SF_LIST | SF_NUMBERED | SF_FIELD, .prefix = TRN(TITLE_ROAMING, "Roaming"),
	.hint = TRN(HINT_1_7_PICK_BACK, "1-7 pick  Back"),
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

// UI_CRASH_SEND_*
static const tr_id_t crash_modes[] = { TRN(VALUE_OFF, "Off"), TRN(CRASH_VALUE_ASK, "Ask"), TRN(VALUE_AUTO, "Auto") };

static const char *crash_body(void)
{
	static char t[160];
	sbuf_t b;
	sb_init(&b, t, sizeof t);
	ui_crash_t c;
	if (!ui_svc_crash_get(&c)) {
		sb_str(&b, TR(CRASH_BODY_NONE, "No crash recorded."));
		return t;
	}
	if (crash_prompt()) {
		// The question only: it has to fit above the two rows on the smallest screens
		char to[CODEPLUG_NAME_LEN + 4];
		crash_target(to, sizeof to);
		sb_str(&b, TR(CRASH_BODY_RESTARTED_SEND_TO, "Restarted after a crash. Send the report to "));
		sb_str(&b, to);
		sb_str(&b, "?");
		return t;
	}
	sb_str(&b, c.summary);
	if (c.sent) {
		sb_chr(&b, '\n');
		sb_str(&b, TR(CRASH_BODY_REPORT_SENT, "Report sent"));
	}
	if (c.count > 1) {
		sb_str(&b, "\n");
		sb_u32(&b, c.count, 1);
		sb_str(&b, TR(CRASH_BODY_IN_A_ROW, " crashes in a row"));
	}
	sb_chr(&b, '\n');
	sb_str(&b, TR(CRASH_BODY_RAN_FOR, "Ran for "));
	sb_mmss(&b, c.uptime_ms / 1000u);
	if (c.build != 0) {
		sb_str(&b, TR(CRASH_BODY_BUILD, ", build "));
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
		str_copy(it->label, sizeof it->label, TR(CRASH_ROW_SEND_REPORT, "Send report"));
		if (!have)
			it->flags |= UI_ITEM_DIM;
		break;
	case CR_TO:
		str_copy(it->label, sizeof it->label, TR(CRASH_LABEL_SEND_TO, "Send to"));
		crash_target(it->value, sizeof it->value);
		if (it->value[0] == 0)
			str_copy(it->value, sizeof it->value, TR(VALUE_NOT_SET, "Not set"));
		break;
	case CR_MODE: {
		int32_t m = ui_svc_setting_get(UI_SET_CRASH_MODE);
		str_copy(it->label, sizeof it->label, TR(CRASH_LABEL_AFTER_A_CRASH, "After a crash"));
		str_copy(it->value, sizeof it->value, tr(crash_modes[m >= 0 && m <= UI_CRASH_SEND_AUTO ? m : 0]));
		break;
	}
	case CR_CLEAR:
		str_copy(it->label, sizeof it->label, TR(CRASH_ROW_CLEAR, "Clear"));
		if (!have)
			it->flags |= UI_ITEM_DIM;
		break;
	default:
		str_copy(it->label, sizeof it->label, TR(CRASH_ROW_NOT_NOW, "Not now"));
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
			ui_toast(TR(CRASH_TOAST_NONE, "No crash recorded"));
		} else if (to[0] == 0) {
			ui_toast(TR(CRASH_TOAST_PICK_SEND_TO_FIRST, "Pick Send to first"));
		} else if (ui_svc_crash_send()) {
			crash_watch = true;
			ui_toast(TR(CRASH_TOAST_SENDING, "Sending crash report"));
			if (crash_prompt())
				ui_pop();
		} else {
			ui_toast(TR(TOAST_CANT_SEND_NOW, "Can't send now: try again"));
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
			ui_toast(TR(TOAST_SAVE_FAILED, "Couldn't save: try again"));
		else if (nm != UI_CRASH_SEND_OFF && ui_svc_setting_get(UI_SET_CRASH_CONTACT) <= 0)
			ui_toast(TR(CRASH_TOAST_PICK_SEND_TO_AS_WELL, "Pick Send to as well"));
		break;
	}
	case CR_CLEAR:
		if (have) {
			ui_svc_crash_clear();
			ui_toast(TR(CRASH_TOAST_CLEARED, "Crash record cleared"));
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
	.name = "crash", .flags = SF_LIST | SF_NUMBERED, .prefix = TRN(CRASH_TITLE, "Crash Log"),
	.hint = TRN(HINT_OK_PICK_BACK, "OK pick  Back"),
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
		ui_toast(TR(TOAST_SAVE_FAILED, "Couldn't save: try again"));
		return;
	}
	static char msg[24 + CODEPLUG_NAME_LEN];	// the toast keeps the pointer
	sbuf_t b;
	sb_init(&b, msg, sizeof msg);
	sb_str(&b, TR(CRASH_TOAST_REPORTS_TO, "Crash reports to "));
	sb_str(&b, t.name[0] ? t.name : TR(CRASH_TOAST_THIS_CONTACT, "this contact"));
	ui_pop();
	ui_toast(msg);
}

static void crash_to_raw_each(ui_raw_fn fn, void *ctx, bool labels) { (void)labels; contacts_rows(0, fn, ctx, true); }

static const ui_scr_def_t crash_to_screen = {
	.name = "crash_to", .flags = SF_LIST | SF_FILTER, .prefix = TRN(CRASH_TITLE_SEND_TO, "Send To"),
	.hint = TRN(HINT_FIND_OK_PICK_BACK, "0-9 find  OK pick  Back"),
	.empty = TRN(EMPTY_NO_CONTACTS_USE_QDMR, "No contacts: add them in qdmr"),
	.count = ui_filter_count, .item = contacts_item, .select = crash_to_select,
	.filter = &(const ui_filter_def_t){ .raw_count = contacts_raw_count, .raw_label = contacts_raw_label,
		.raw_each = crash_to_raw_each },
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
			ui_toast(TR(CRASH_TOAST_SENDING, "Sending crash report"));
		}
		return;
	}
	if (!crash_watch)
		return;
	uint8_t st = ui_svc_crash_tx_state();
	if (st == UI_CRASH_TX_OK || st == UI_CRASH_TX_FAILED) {
		crash_watch = false;
		ui_toast(st == UI_CRASH_TX_OK ? TR(CRASH_TOAST_SENT, "Crash report sent") :
			TR(CRASH_TOAST_NOT_SENT, "Crash report not sent"));
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

static uint16_t tools_last;	// the highest tool's number, found on each count (the list counts before the hint)

static uint16_t tools_count(void)
{
	uint16_t n = 0, last = 0;
	for (uint16_t i = 0; i < T_COUNT; i++) {
		if (tool_shown(i)) {
			n++;
			last = (uint16_t)(i + 1u);
		}
	}
	tools_last = last;
	return n;
}

// The hint names the last number ("1-9 open"): the highest tool this radio has, from the keys and the translated rest
static const char *tools_hint(void)
{
	static char hint[24];
	uint16_t last = tools_last;
	sbuf_t b;
	sb_init(&b, hint, sizeof hint);
	sb_str(&b, "1-");
	sb_chr(&b, (char)('0' + (last < 9 ? last : 9)));	// "1-9": the zone offer's 0 isn't named
	sb_str(&b, TR(TOOLS_HINT_OPEN_BACK, " open  Back"));
	return hint;
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
	static const tr_id_t names[] = { TRN(WORD_SCAN, "Scan"), TRN(TITLE_POSITION, "Position"),
		TRN(TITLE_HOTSPOT, "Hotspot"), TRN(TOOLS_ROW_USB_NETWORK, "USB network"),
		TRN(TOOLS_ROW_SFR, "Single-freq repeater"), TRN(TITLE_ROAMING, "Roaming"),
		TRN(TITLE_DIAGNOSTICS, "Diagnostics"), TRX(WORD_APRS, "APRS"), TRN(TITLE_BATTERY, "Battery"),
		TRN(TOOLS_ROW_SHARES_RECEIVED, "Zones/Chs Received") };
	uint16_t i = tools_row(row);
	if (i >= T_COUNT)
		return;
	str_copy(it->label, sizeof it->label, i == T_BATTERY && board_volts() == BOARD_VOLTS_SUPPLY ?
		TR(TITLE_SUPPLY, "Supply") :
		 tr(names[i]));
	item_num(it, (uint16_t)(i + 1u));	// the tool's own number, not its row (see the enum)
	sbuf_t b;
	sb_init(&b, it->value, sizeof it->value);
	switch (i) {
	case T_SCAN:
#if CONFIG_DUAL_WATCH
		if (ui_st.dual.on) {
			sb_str(&b, TR(WORD_DUAL_WATCH, "Dual watch"));
			break;
		}
#endif
		sb_str(&b, ui_st.scanning ? TR(VALUE_ON, "On") : TR(VALUE_OFF, "Off"));
		if (!(feats() & UI_FEAT_SCAN))
			it->flags |= UI_ITEM_DIM;
		break;
	case T_POSITION:
		if (!(feats() & UI_FEAT_GPS))
			it->flags |= UI_ITEM_DIM;
		break;
	case T_HOTSPOT:
#if CONFIG_HOTSPOT
		sb_str(&b, hostlink_mode() == HOSTLINK_HOTSPOT ? TR(VALUE_ON, "On") : TR(VALUE_OFF, "Off"));
#else
		it->flags |= UI_ITEM_DIM;
#endif
		break;
	case T_USB_NET:
#if USB_NET_UI
		sb_str(&b, hostlink_network_mode() ? TR(TOOLS_VALUE_USB_NETWORK, "Network") :
			TR(TOOLS_VALUE_USB_SERIAL, "Serial"));
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
		sb_str(&b, ui_view()->roam.zone_on ? TR(VALUE_ON, "On") : TR(VALUE_OFF, "Off"));
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
			ui_toast(TR(WHY_SCAN_NOT_IN_BUILD, "Scan: not in this build"));
		} else {
			ui_go_home();
			ui_scan_toggle();	// says what scans: the zone, or the band in the VFO (dual watch ends)
		}
		break;
	case T_POSITION: ui_push(CONFIG_GPS_SKY ? SCR_GPS : SCR_INFO, INFO_POSITION); break;
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
	.name = "tools", .flags = SF_LIST | SF_NUMBERED | SF_FIELD | SF_HINT_FN, .prefix = TRN(TITLE_TOOLS, "Tools"),
	.hint_fn = tools_hint,
	.count = tools_count, .item = tools_item, .select = tools_select,
};

// ======================= Settings by intent (information-architecture.md 6) =======================
// CONFIG_UI_SETTINGS=n leaves the whole menu out (the settings keep their stored values; qdmr sets them)
#if CONFIG_UI_SETTINGS

typedef enum { ST_TOGGLE, ST_LIST, ST_NUMBER, ST_ACTION } set_type_t;
enum { SA_DIAG, SA_FIRMWARE, SA_CODEPLUG, SA_RESET, SA_MY_RADIO, SA_CRASH, SA_LANGUAGE, SA_HARDWARE, SA_FIXED_POS, SA_DATA_SERVICES,
       SA_BL_TEST };

typedef struct {
	tr_id_t name;			// TRN; TR_NONE for a key's row (set_name)
	const char *syn;		// another name Find knows it by, in English (never shown)
	uint8_t type;			// set_type_t
	uint8_t id;			// ui_setting_t, or SA_* for an action
	uint8_t n;			// ST_LIST: how many values
	// By type, in the same bytes (24 a row, not 32: there are about 80 rows)
	union {
		struct {			// ST_NUMBER
			int16_t min, max, step;
			const char *unit;	// after the number, or NULL
		};
		struct {			// ST_LIST
			const int16_t *values;
			const tr_id_t *names;	// TRN, or TRX for one never translated
		};
	};
} set_item_t;
_Static_assert(sizeof(void *) != 4 || sizeof(set_item_t) <= 24, "a settings row grew (on the radios; the host's is larger)");

typedef struct {
	tr_id_t name;
	tr_id_t title;		// the group's page title, in Title Case (the name is its row in Settings)
	const set_item_t *items;
	uint8_t count;
} set_group_t;

static const int16_t v_backlight[] = { 5, 10, 30, 60, 120, -10, -30, -60, -120, 0 };	// < 0: dims, never off
// Dim after / off after (four times as long): both times shown, so that the dim step isn't taken for the time-out
static const tr_id_t n_backlight[] = { TRX(SET_VALUE_BACKLIGHT_5S_20S, "5s/20s"),
	TRX(SET_VALUE_BACKLIGHT_10S_40S, "10s/40s"), TRX(SET_VALUE_BACKLIGHT_30S_2M, "30s/2m"),
	TRX(SET_VALUE_BACKLIGHT_1M_4M, "1m/4m"), TRX(SET_VALUE_BACKLIGHT_2M_8M, "2m/8m"),
	TRN(SET_VALUE_BACKLIGHT_10S_DIM, "10s/dim"), TRN(SET_VALUE_BACKLIGHT_30S_DIM, "30s/dim"),
	TRN(SET_VALUE_BACKLIGHT_1M_DIM, "1m/dim"), TRN(SET_VALUE_BACKLIGHT_2M_DIM, "2m/dim"), TRN(VALUE_NEVER, "Never")
};
#if CONFIG_UI_COLOR
static const int16_t v_theme[] = { 0, 1, 2 };
static const tr_id_t n_theme[] = { TRN(SET_VALUE_THEME_DARK, "Dark"), TRN(SET_VALUE_THEME_LIGHT, "Light"),
	TRN(SET_VALUE_THEME_HIGH_CONTRAST, "High contrast") };
static const int16_t v_accent[] = { UI_ACCENT_DEFAULT, UI_ACCENT_RED, UI_ACCENT_ORANGE, UI_ACCENT_YELLOW, UI_ACCENT_TEAL,
				     UI_ACCENT_BLUE, UI_ACCENT_PURPLE, UI_ACCENT_PINK };
static const tr_id_t n_accent[] = { TRN(VALUE_DEFAULT, "Default"), TRN(SET_VALUE_ACCENT_RED, "Red"),
	TRN(SET_VALUE_ACCENT_ORANGE, "Orange"), TRN(SET_VALUE_ACCENT_YELLOW, "Yellow"),
	TRN(SET_VALUE_ACCENT_TEAL, "Teal"), TRN(SET_VALUE_ACCENT_LIGHT_BLUE, "Light blue"),
	TRN(SET_VALUE_ACCENT_PURPLE, "Purple"), TRN(SET_VALUE_ACCENT_PINK, "Pink") };
#endif
static const int16_t v_tot[] = { 0, 30, 60, 120, 180, 300, 600 };
static const tr_id_t n_tot[] = { TRN(VALUE_OFF, "Off"), TRX(VALUE_30_S, "30 s"), TRN(VALUE_1_MIN, "1 min"),
	TRN(VALUE_2_MIN, "2 min"), TRN(VALUE_3_MIN, "3 min"), TRN(VALUE_5_MIN, "5 min"), TRN(VALUE_10_MIN, "10 min") };
static const int16_t v_lock[] = { 0, 30, 60, 300 };
static const tr_id_t n_lock[] = { TRN(VALUE_OFF, "Off"), TRX(VALUE_30_S, "30 s"), TRN(VALUE_1_MIN, "1 min"),
	TRN(VALUE_5_MIN, "5 min") };
static const int16_t v_posfmt[] = { 0, 1, 2 };
static const tr_id_t n_posfmt[] = { TRN(SET_VALUE_POSFMT_DEGREES, "Degrees"),
	TRN(SET_VALUE_POSFMT_DEG_MIN_SEC, "Deg min sec"), TRN(WORD_GRID, "Grid") };
static const int16_t v_possend[] = { 0, 60, 300, 900 };
#if CONFIG_POSITION
// position_pref_t: where our position comes from. A computer's NMEA only where something takes it in
#if CONFIG_POSITION_HOST
static const int16_t v_possrc[] = { 0, 1, 2, 3 };
static const tr_id_t n_possrc[] = { TRN(VALUE_AUTO, "Auto"), TRX(WORD_GPS, "GPS"), TRN(POS_VALUE_COMPUTER, "Computer"),
	TRN(POS_VALUE_FIXED, "Fixed") };
#else
static const int16_t v_possrc[] = { 0, 1, 3 };
static const tr_id_t n_possrc[] = { TRN(VALUE_AUTO, "Auto"), TRX(WORD_GPS, "GPS"), TRN(POS_VALUE_FIXED, "Fixed") };
#endif
#endif
static const tr_id_t n_possend[] = { TRN(VALUE_OFF, "Off"), TRN(VALUE_1_MIN, "1 min"), TRN(VALUE_5_MIN, "5 min"),
	TRN(VALUE_15_MIN, "15 min") };
#if CONFIG_KEY_ASSIGN
// Side keys (core/input/keyassign.h, KEYFN_* values): monitor needs the key held, so it is a press only;
// emergency is hold to activate, so a long press only
static const int16_t v_keyfn_press[] = { KEYFN_DEFAULT, KEYFN_NONE, KEYFN_MONITOR, KEYFN_TALK_TARGET, KEYFN_POWER,
					 KEYFN_ZONE_UP, KEYFN_ZONE_DOWN, KEYFN_SCAN, KEYFN_KEY_LOCK,
					 KEYFN_HOME_ZONE,
#if CONFIG_FM_TX_TONES
					 KEYFN_TONE_BURST,
#endif
#if CONFIG_VFO_FM || CONFIG_VFO_DMR
					 KEYFN_COPY_VFO, KEYFN_VFO_TOGGLE,
#endif
#if CONFIG_DMR
					 KEYFN_CALL_BACK, KEYFN_TALK_LABEL,
#endif
#if CONFIG_DUAL_WATCH
					 KEYFN_AB, KEYFN_DUAL_WATCH,
#endif
};
static const tr_id_t n_keyfn_press[] = { TRN(VALUE_DEFAULT, "Default"), TRN(KEYFN_VALUE_NOTHING, "Nothing"),
	TRN(KEYFN_VALUE_MONITOR, "Monitor"), TRN(KEYFN_VALUE_TALK_TARGET, "Talk target"),
					     TRN(KEYFN_VALUE_POWER_LEVEL, "Power level"),
					TRN(KEYFN_VALUE_NEXT_ZONE, "Next zone"),
					TRN(KEYFN_VALUE_PREV_ZONE, "Prev zone"), TRN(WORD_SCAN, "Scan"),
					     TRN(WORD_KEY_LOCK, "Key lock"), TRN(KEYFN_VALUE_HOME_ZONE, "Home zone"),
#if CONFIG_FM_TX_TONES
					     TRN(WORD_TONE_BURST, "Tone burst"),
#endif
#if CONFIG_VFO_FM || CONFIG_VFO_DMR
					     TRN(KEYFN_VALUE_COPY_TO_VFO, "Copy to VFO"),
					TRN(KEYFN_VALUE_VFO_CHANNEL, "VFO/Channel"),
#endif
#if CONFIG_DMR
					     TRN(KEYFN_VALUE_CALL_BACK, "Call back"),
					TRN(KEYFN_VALUE_TG_NAME_NUMBER, "TG name / number"),
#endif
#if CONFIG_DUAL_WATCH
					     TRX(KEYFN_VALUE_A_B, "A/B"), TRN(WORD_DUAL_WATCH, "Dual watch"),
#endif
};
static const int16_t v_keyfn_hold[] = { KEYFN_DEFAULT, KEYFN_NONE, KEYFN_TALK_TARGET, KEYFN_POWER, KEYFN_ZONE_UP,
					KEYFN_ZONE_DOWN, KEYFN_SCAN,
#if CONFIG_DMR_EMERGENCY
					KEYFN_EMERGENCY,
#endif
					KEYFN_KEY_LOCK, KEYFN_HOME_ZONE,
#if CONFIG_VFO_FM || CONFIG_VFO_DMR
					KEYFN_COPY_VFO, KEYFN_VFO_TOGGLE,
#endif
#if CONFIG_DMR
					KEYFN_CALL_BACK, KEYFN_TALK_LABEL,
#endif
#if CONFIG_DUAL_WATCH
					KEYFN_AB, KEYFN_DUAL_WATCH,
#endif
};
static const tr_id_t n_keyfn_hold[] = { TRN(VALUE_DEFAULT, "Default"), TRN(KEYFN_VALUE_NOTHING, "Nothing"),
	TRN(KEYFN_VALUE_TALK_TARGET, "Talk target"), TRN(KEYFN_VALUE_POWER_LEVEL, "Power level"),
					    TRN(KEYFN_VALUE_NEXT_ZONE, "Next zone"),
					TRN(KEYFN_VALUE_PREV_ZONE, "Prev zone"), TRN(WORD_SCAN, "Scan"),
#if CONFIG_DMR_EMERGENCY
					    TRN(TITLE_EMERGENCY, "Emergency"),
#endif
					    TRN(WORD_KEY_LOCK, "Key lock"), TRN(KEYFN_VALUE_HOME_ZONE, "Home zone"),
#if CONFIG_VFO_FM || CONFIG_VFO_DMR
					    TRN(KEYFN_VALUE_COPY_TO_VFO, "Copy to VFO"),
					TRN(KEYFN_VALUE_VFO_CHANNEL, "VFO/Channel"),
#endif
#if CONFIG_DMR
					    TRN(KEYFN_VALUE_CALL_BACK, "Call back"),
					TRN(KEYFN_VALUE_TG_NAME_NUMBER, "TG name / number"),
#endif
#if CONFIG_DUAL_WATCH
					    TRX(KEYFN_VALUE_A_B, "A/B"), TRN(WORD_DUAL_WATCH, "Dual watch"),
#endif
};
#endif
static const int16_t v_updown[] = { 0, 1, 2 };
static const tr_id_t n_updown[] = { TRN(SET_VALUE_UPDOWN_CHANNEL, "Channel"), TRN(WORD_ZONE, "Zone"),
	TRN(KEYFN_VALUE_TALK_TARGET, "Talk target") };
#if CONFIG_VFO
static const int16_t v_step[] = { 0, 250, 500, 625, 833, 1000, 1250, 2000, 2500, 5000, 10000 };	// 10 Hz units
static const tr_id_t n_step[] = { TRN(VALUE_AUTO, "Auto"), TRX(VALUE_2_5_KHZ, "2.5 kHz"), TRX(VALUE_5_KHZ, "5 kHz"),
	TRX(VALUE_6_25_KHZ, "6.25 kHz"), TRX(VALUE_8_33_KHZ, "8.33 kHz"), TRX(VALUE_10_KHZ, "10 kHz"),
	TRX(VALUE_12_5_KHZ, "12.5 kHz"), TRX(VALUE_20_KHZ, "20 kHz"), TRX(VALUE_25_KHZ, "25 kHz"),
	TRX(VALUE_50_KHZ, "50 kHz"), TRX(VALUE_100_KHZ, "100 kHz") };
#endif
#if CONFIG_VFO && CONFIG_BCAST_FM
static const int16_t v_bcast_step[] = { 200, 100 };	// kHz
static const tr_id_t n_bcast_step[] = { TRX(VALUE_200_KHZ, "200 kHz"), TRX(VALUE_100_KHZ, "100 kHz") };
#endif
#if CONFIG_DUAL_WATCH
static const int16_t v_dual_wait[] = { 500, 1000, 1500, 2000, 3000, 5000 };	// ms
static const tr_id_t n_dual_wait[] = { TRX(VALUE_0_5_S, "0.5 s"), TRX(VALUE_1_S, "1 s"), TRX(VALUE_1_5_S, "1.5 s"),
	TRX(VALUE_2_S, "2 s"), TRX(VALUE_3_S, "3 s"), TRX(VALUE_5_S, "5 s") };
#endif
static const int16_t v_autooff[] = { 0, 30, 60, 120 };
static const tr_id_t n_autooff[] = { TRN(VALUE_OFF, "Off"), TRN(VALUE_30_MIN, "30 min"), TRX(VALUE_1_H, "1 h"),
	TRX(VALUE_2_H, "2 h") };
#if CONFIG_DMR_SMS
static const int16_t v_msgfmt[] = {
	2, 1,	// dmr_sms_format_t: ETSI IP text, ETSI short data, Motorola TMS
#if CONFIG_DMR_TMS
	0,
#endif
};
static const tr_id_t n_msgfmt[] = {
	TRX(SET_VALUE_MSGFMT_ETSI, "ETSI"), TRN(SET_VALUE_MSGFMT_ETSI_SHORT, "ETSI short"),
#if CONFIG_DMR_TMS
	TRX(VALUE_MOTOROLA, "Motorola"),
#endif
};
#endif
#if CONFIG_DMR_CONTROL_STATION
static const int16_t v_netlink[] = { 0, 1 };
static const tr_id_t n_netlink[] = { TRN(SET_VALUE_USB_ADDR_BY_DMR_ID, "By DMR ID"),
	TRX(SET_VALUE_USB_ADDR_192_168_10_X, "192.168.10.x") };
#endif
#if CONFIG_SFR
static const int16_t v_sfrrx[] = { 0, 1,	// dmr_sfr_rx_t
#if CONFIG_SFR_DUALPATH_RX
				   2,
#endif
};
static const tr_id_t n_sfrrx[] = { TRN(SFR_VALUE_REPEATER, "Repeater"), TRN(SET_VALUE_SFR_RX_FIRST_COPY, "First copy"),
	TRN(SET_VALUE_SFR_RX_DUAL_PATH, "Dual-path RX") };
#endif
#if CONFIG_SFR_CWID
static const int16_t v_sfrcwid[] = { 0, 1, 2, 3, 4 };	// radio_cwid_interval_ms codes
static const tr_id_t n_sfrcwid[] = { TRN(VALUE_OFF, "Off"), TRN(VALUE_5_MIN, "5 min"), TRN(VALUE_10_MIN, "10 min"),
	TRN(VALUE_15_MIN, "15 min"), TRN(VALUE_30_MIN, "30 min") };
#endif
static const int16_t v_ptype[] = { 0, 1, 2 };	// dmrPrivacyType_t
static const tr_id_t n_ptype[] = { TRN(VALUE_OFF, "Off"), TRX(VALUE_MOTOROLA, "Motorola"),
	TRX(SET_VALUE_KEY_TYPE_ANYTONE, "Anytone") };

#define TOGGLE(nm, syn_, sid) { nm, syn_, ST_TOGGLE, sid, 0, { { 0, 1, 1, NULL } } }
#define NUMBER(nm, syn_, sid, lo, hi, st, u) { nm, syn_, ST_NUMBER, sid, 0, { { lo, hi, st, u } } }
#define LIST(nm, syn_, sid, v, nms) { nm, syn_, ST_LIST, sid, sizeof v / sizeof v[0], { .values = v, .names = nms } }
#define ACTION(nm, syn_, a) { nm, syn_, ST_ACTION, a, 0, { { 0, 0, 0, NULL } } }

static const set_item_t s_sound[] = {
	NUMBER(TRN(TITLE_VOLUME, "Volume"), "speaker loudness", UI_SET_VOLUME, 0, RADIO_VOLUME_MAX, 1, NULL),
	TOGGLE(TRN(SET_LABEL_KEY_TONES, "Key tones"), "beep", UI_SET_KEY_TONES),
	NUMBER(TRN(SET_LABEL_TONE_VOLUME, "Tone volume"), NULL, UI_SET_TONE_VOLUME, 0, 10, 1, NULL),
	TOGGLE(TRN(SET_LABEL_TALK_PERMIT_TONE, "Talk permit tone"), NULL, UI_SET_TALK_PERMIT),
	TOGGLE(TRN(SET_LABEL_CALL_END_TONE, "Call end tone"), NULL, UI_SET_CALL_END_TONE),
	TOGGLE(TRN(SET_LABEL_TOT_WARNING_TONE, "TOT warning tone"), "time-out", UI_SET_TOT_WARN_TONE),
	TOGGLE(TRN(SET_LABEL_BATTERY_TONE, "Battery tone"), "low battery", UI_SET_BATTERY_TONE),
	TOGGLE(TRN(SET_LABEL_TX_DENIED_TONE, "TX denied tone"), "refused", UI_SET_TX_DENY_TONE),
	TOGGLE(TRN(SET_LABEL_SCAN_STOP_TONE, "Scan stop tone"), NULL, UI_SET_SCAN_TONE),
	TOGGLE(TRN(SET_LABEL_EMERGENCY_TONE, "Emergency tone"), "alarm", UI_SET_EMERGENCY_TONE),
	TOGGLE(TRN(SET_LABEL_VOICE_PROMPTS, "Voice prompts"), "speech", UI_SET_VOICE_PROMPTS),
	TOGGLE(TRN(SET_LABEL_MESSAGE_TONE, "Message tone"), NULL, UI_SET_MESSAGE_TONE),
#if CONFIG_VIBRATE
	TOGGLE(TRN(SET_LABEL_VIBRATE, "Vibrate"), "vibration motor silent", UI_SET_VIBRATE),
#endif
};
static const set_item_t s_display[] = {
	NUMBER(TRN(SET_LABEL_BRIGHTNESS, "Brightness"), "backlight", UI_SET_BRIGHTNESS, 10, 100, 10, "%"),
	LIST(TRN(SET_LABEL_BACKLIGHT_DIM_OFF, "Backlight Dim/Off"), "timeout", UI_SET_BACKLIGHT_S, v_backlight,
		n_backlight),
#if CONFIG_UI_COLOR	// a radio whose only screen is monochrome has no colors to set: no rows for them
	LIST(TRN(SET_LABEL_THEME, "Theme"), "contrast", UI_SET_THEME, v_theme, n_theme),
	LIST(TRN(SET_LABEL_HIGHLIGHT_COLOR, "Highlight color"), "accent", UI_SET_ACCENT, v_accent, n_accent),
	TOGGLE(TRN(SET_LABEL_FILLED_STATUS_BAR, "Filled status bar"), "top line", UI_SET_STRIP),
#endif
	TOGGLE(TRN(SET_LABEL_LARGE_TEXT, "Large text"), "accessibility", UI_SET_LARGE_TEXT),
	TOGGLE(TRN(SET_LABEL_HINT_ROW, "Hint row"), "help", UI_SET_HINTS),
#if CONFIG_DMR
	TOGGLE(TRN(SET_LABEL_TG_NAME_FIRST, "Talkgroup name first"), "talk line label TG number Home",
		UI_SET_LABEL_FIRST),
#endif
	// Last, so the rows above keep their places; its search words are "language" in other languages, for a user who
	// can't read the English
	ACTION(TRN(SET_LABEL_LANGUAGE, "Language"), "idioma langue sprache lingua taal jezyk", SA_LANGUAGE),
};
static const set_item_t s_talk[] = {
	NUMBER(TRN(SET_LABEL_REPLY_WINDOW, "Reply window"), NULL, UI_SET_REPLY_S, 0, 30, 1, " s"),
	LIST(TRN(SET_LABEL_TX_TIMEOUT, "TX timeout"), "TOT", UI_SET_TOT_S, v_tot, n_tot),
	NUMBER(TRN(SET_LABEL_DEFAULT_SQUELCH, "Default squelch"), NULL, UI_SET_SQUELCH, 0, 9, 1, NULL),
#if CONFIG_VFO
	LIST(TRN(SET_LABEL_VFO_STEP, "VFO step"), "frequency tuning channel spacing", UI_SET_VFO_STEP, v_step, n_step),
#endif
#if CONFIG_VFO && CONFIG_BCAST_FM
	LIST(TRN(SET_LABEL_FM_BROADCAST_STEP, "FM broadcast step"), "VFO tuning radio station 200 kHz odd",
		UI_SET_BCAST_STEP, v_bcast_step, n_bcast_step),
#endif
	NUMBER(TRN(SET_LABEL_MIC_GAIN, "Mic gain"), NULL, UI_SET_MIC_GAIN, 0, 10, 1, NULL),
	TOGGLE(TRN(SET_LABEL_MIC_AGC, "Mic AGC"), "automatic gain level", UI_SET_MIC_AGC),
	TOGGLE(TRN(SET_LABEL_MIC_NOISE_REDUCTION, "Mic noise reduction"), "noise suppression background",
		UI_SET_NOISE_REDUCTION),
	TOGGLE(TRN(SET_LABEL_MONITOR_BY_DEFAULT, "Monitor by default"), "digital monitor promiscuous hear all",
		UI_SET_HEAR_ALL_DEFAULT),
	TOGGLE(TRN(SET_LABEL_SHOW_TALKER_ALIAS, "Show talker alias"), NULL, UI_SET_ALIAS_SHOW),
#if CONFIG_DUAL_WATCH
	TOGGLE(TRN(SET_LABEL_DUAL_TALK_BACK, "Dual watch talk back"), "reply other channel A B", UI_SET_DUAL_TALK_BACK),
	LIST(TRN(SET_LABEL_DUAL_DELAY, "Dual watch delay"), "resume wait reply A B", UI_SET_DUAL_DELAY_MS, v_dual_wait,
		n_dual_wait),
#endif
#if CONFIG_SFR
	LIST(TRN(SET_LABEL_SFR_RECEIVE, "SFR receive"), "single frequency repeater both copies dual-path rx",
		UI_SET_SFR_RX, v_sfrrx, n_sfrrx),
#endif
#if CONFIG_SFR_CWID
	LIST(TRN(SET_LABEL_SFR_CW_ID, "SFR CW ID"), "single frequency repeater morse callsign identify",
		UI_SET_SFR_CWID, v_sfrcwid, n_sfrcwid),
#endif
};
static const set_item_t s_privacy[] = {
	LIST(TRN(SET_LABEL_PRIVACY_KEY_TYPE, "Key type"), "privacy encryption", UI_SET_PRIVACY_TYPE, v_ptype, n_ptype),
	// the range follows the type (set_range)
	NUMBER(TRN(SET_LABEL_PRIVACY_KEY, "Key"), "privacy BP", UI_SET_PRIVACY_KEY, 1, 255, 1, NULL),
};
// The key lock's rules: when it locks by itself and what still works while locked
static const set_item_t s_lock[] = {
	LIST(TRN(SET_LABEL_AUTO_LOCK, "Auto lock"), "keylock", UI_SET_AUTO_LOCK_S, v_lock, n_lock),
	TOGGLE(TRN(SET_LABEL_PTT_WHEN_LOCKED, "PTT when locked"), NULL, UI_SET_PTT_LOCKED),
	TOGGLE(TRN(SET_LABEL_KNOB_WHEN_LOCKED, "Knob when locked"), "volume knob key lock", UI_SET_KNOB_WHEN_LOCKED),
};
static const set_item_t s_location[] = {
	TOGGLE(TRX(WORD_GPS, "GPS"), NULL, UI_SET_GPS),
	LIST(TRN(SET_LABEL_POSITION_FORMAT, "Position format"), NULL, UI_SET_POS_FORMAT, v_posfmt, n_posfmt),
	LIST(TRN(SET_LABEL_SEND_POSITION, "Send position"), "APRS", UI_SET_POS_SEND_S, v_possend, n_possend),
	TOGGLE(TRN(SET_LABEL_ANSWER_GPS_REQUESTS, "Answer GPS requests"), "LRRP position location", UI_SET_POS_REPLY),
#if CONFIG_POSITION
	// What every position report sends (docs/spec/position.md): DMR GPS Info, LRRP, APRS. After the rows above, so
	// they keep their places
	LIST(TRN(SET_LABEL_POSITION_SOURCE, "Position source"), "GPS computer fixed auto", UI_SET_POS_SOURCE, v_possrc,
		n_possrc),
	ACTION(TRN(SET_LABEL_FIXED_POSITION, "Fixed position"), "latitude longitude coordinates", SA_FIXED_POS),
	TOGGLE(TRN(SET_LABEL_SEND_IN_DMR_CALLS, "Send in DMR calls"), "GPS info position location talker",
		UI_SET_POS_DMR_CALLS),
#endif
};
static const set_item_t s_connections[] = {
	TOGGLE(TRN(SET_LABEL_HOTSPOT_ALLOWED, "Hotspot allowed"), "MMDVM", UI_SET_HOTSPOT_AUTO),
	TOGGLE(TRN(SET_LABEL_ANSWER_RADIO_CHECKS, "Answer radio checks"), NULL, UI_SET_CHECK_REPLY),
	TOGGLE(TRN(SET_LABEL_ANSWER_CALL_ALERTS, "Answer call alerts"), NULL, UI_SET_ALERT_ANSWER),
#if CONFIG_TFTP
	TOGGLE(TRN(SET_LABEL_TFTP_CODEPLUG_WRITE, "TFTP codeplug write"), "network USB programming", UI_SET_TFTP_WRITE),
#endif
#if CONFIG_DMR_SMS
	LIST(TRN(SET_LABEL_MESSAGE_FORMAT, "Message format"), "SMS text ETSI TMS", UI_SET_MSG_FORMAT, v_msgfmt,
		n_msgfmt),
#endif
#if CONFIG_ZONE_SHARE
	TOGGLE(TRN(SET_LABEL_ACCEPT_ZONE_SHARES, "Accept zone shares"), "zone offers sharing receive import channel",
		UI_SET_ZONE_OFFERS),
#endif
#if CONFIG_USB_NETWORK && CONFIG_HOSTLINK
	TOGGLE(TRN(SET_LABEL_USB_NETWORK_AT_START, "USB network at start"), "dispatch control station",
		UI_SET_USB_BOOT_NET),
#endif
#if CONFIG_DMR_CONTROL_STATION
	TOGGLE(TRN(SET_LABEL_PC_ANSWERS_DATA, "PC answers data"), "dispatch control station passthrough",
		UI_SET_PC_ENDPOINT),
	LIST(TRN(SET_LABEL_USB_ADDRESS, "USB address"), "dispatch IP link", UI_SET_USB_FIXED_LINK, v_netlink,
		n_netlink),
#endif
#if DATA_IDS	// last, so the rows above keep their places
	ACTION(TRN(SET_ROW_DATA_SERVICES, "Data services"), "ARS registration presence server IP gateway off-net",
		SA_DATA_SERVICES),
#endif
};
static const set_item_t s_power[] = {
	TOGGLE(TRN(LABEL_BATTERY_SAVER, "Battery saver"), NULL, UI_SET_BATTERY_SAVER),
	LIST(TRN(SET_LABEL_AUTO_POWER_OFF, "Auto power off"), NULL, UI_SET_AUTO_OFF_MIN, v_autooff, n_autooff),
	NUMBER(TRN(SET_LABEL_LOW_BATTERY_WARNING, "Low battery warning"), NULL, UI_SET_LOW_BATTERY, 5, 30, 5, "%"),
};
#if CONFIG_KEY_ASSIGN
// Settings > Keys: a row pair per key the board lets the user reassign, named as on the radio ("SK1", "SK1 hold")
_Static_assert(UI_KEY_ROLES == 9, "KEY_ROWS below");
static char key_label[2 * UI_KEY_ROLES][20];
static uint8_t key_label_lang = 0xFF;	// the language they were built in: rebuilt when the user picks another

static void key_labels(void)
{
	sbuf_t b;

	if (key_label_lang == lang_current())
		return;
	key_label_lang = lang_current();
	for (uint8_t r = 0; r < UI_KEY_ROLES; r++) {
		for (uint8_t h = 0; h < 2; h++) {
			sb_init(&b, key_label[2 * r + h], sizeof key_label[0]);
			if (board.side_key_names != NULL && r < board.side_key_count) {
				sb_str(&b, board.side_key_names[r]);
			} else if (board.home_key_names != NULL && r >= board.side_key_count &&
				   r < board.side_key_count + board.home_key_count) {
				sb_str(&b, board.home_key_names[r - board.side_key_count]);
			} else {
				sb_str(&b, TR(SET_LABEL_KEY_N, "Key "));
				sb_i32(&b, r + 1);
			}
			if (h)
				sb_str(&b, TR(SET_LABEL_KEY_N_HOLD, " hold"));
		}
	}
}
#endif
static const set_item_t s_keys[] = {
	NUMBER(TRN(SET_LABEL_LONG_PRESS_TIME, "Long press time"), NULL, UI_SET_LONG_PRESS_MS, 400, 1200, 100, " ms"),
	NUMBER(TRN(SET_LABEL_KEY_REPEAT, "Key repeat"), NULL, UI_SET_REPEAT_MS, 60, 300, 20, " ms"),
	LIST(TRN(SET_LABEL_UP_DOWN_ON_HOME, "Up/Down on Home"), NULL, UI_SET_UPDOWN_HOME, v_updown, n_updown),
#if CONFIG_KEY_ASSIGN
#define KEY_ROWS(r) \
	LIST(TR_NONE, "button programmable", UI_SET_KEY_FIRST + 2 * (r), v_keyfn_press, n_keyfn_press), \
	LIST(TR_NONE, "button programmable long press", UI_SET_KEY_FIRST + 2 * (r) + 1, v_keyfn_hold, \
	     n_keyfn_hold)
	KEY_ROWS(0), KEY_ROWS(1), KEY_ROWS(2), KEY_ROWS(3), KEY_ROWS(4), KEY_ROWS(5), KEY_ROWS(6), KEY_ROWS(7),
	KEY_ROWS(8),
#endif
};
static const set_item_t s_my_radio[] = {
	ACTION(TRN(SET_ROW_DMR_ID_AND_NAME, "DMR ID and name"), "callsign", SA_MY_RADIO),
	// Also in Advanced, where users didn't look for it
	ACTION(TRN(TITLE_ABOUT, "About"), "firmware version release build date", SA_FIRMWARE),
};
static const set_item_t s_advanced[] = {
	ACTION(TRN(TITLE_ABOUT, "About"), "firmware version license authors", SA_FIRMWARE),
	ACTION(TRN(TITLE_CODEPLUG, "Codeplug"), NULL, SA_CODEPLUG),
	ACTION(TRN(TITLE_DIAGNOSTICS, "Diagnostics"), "debug", SA_DIAG),
#if CONFIG_BACKLIGHT_TEST
	// Beside Diagnostics, so it moves the rows below it down one: the flows that reach them count the presses
	// (tests/host/uiFlowTest.c ADV_BELOW_DIAG). Not ideal; rows found by name instead of position would end that
	ACTION(TRX(TITLE_BACKLIGHT_TESTS, "Backlight tests"), "brightness PWM duty rate", SA_BL_TEST),
#endif
	ACTION(TRN(SET_ROW_FACTORY_RESET, "Factory reset"), "erase", SA_RESET),
	// After Factory reset, so its place (and its tests' key presses) stays
#if CONFIG_CRASH_SCREEN
	ACTION(TRN(SET_ROW_CRASH_LOG, "Crash log"), "crash report fault debug", SA_CRASH),
#endif
#if CONFIG_RX_EXT
	TOGGLE(TRN(SET_LABEL_RX_PAST_BANDS, "RX past bands"), "extended out of band receive range", UI_SET_RX_EXT),
#endif
#if CONFIG_REF_TRIM
	NUMBER(TRN(SET_ROW_FREQ_TRIM, "Freq trim"), "frequency offset reference oscillator calibration ppm",
		UI_SET_REF_TRIM, -REF_TRIM_MAX_PPB,
	       REF_TRIM_MAX_PPB, REF_TRIM_STEP_PPB, NULL),	// its own page (trim_screen); last: Up from the top
#endif
#if RFOUNDRY_HWOPTS
	// Only on radios with hardware options (hal_hwopt.h): last, so the rows above keep their places
	ACTION(TRX(HW_TITLE, "Hardware"), "board revision", SA_HARDWARE),
#endif
};

#define GROUP(nm, a)		{ nm, nm, a, sizeof a / sizeof a[0] }
#define GROUP_T(nm, t, a)	{ nm, t, a, sizeof a / sizeof a[0] }
static const set_group_t groups[] = {
	GROUP(TRN(SET_ROW_SOUND, "Sound"), s_sound), GROUP(TRN(SET_ROW_DISPLAY, "Display"), s_display),
	GROUP_T(TRN(SET_ROW_TALK_LISTEN, "Talk & listen"), TRN(SET_TITLE_TALK_LISTEN, "Talk & Listen"), s_talk),
		GROUP(TRN(SET_ROW_PRIVACY, "Privacy"), s_privacy),
	GROUP(TRN(SET_ROW_LOCATION, "Location"), s_location), GROUP(TRN(SET_ROW_CONNECTIONS, "Connections"),
		s_connections),
	GROUP_T(TRN(SET_ROW_POWER_SAVING, "Power saving"), TRN(SET_TITLE_POWER_SAVING, "Power Saving"), s_power),
	GROUP(TRN(SET_ROW_KEYS, "Keys"), s_keys), GROUP(TRN(WORD_KEY_LOCK, "Key lock"), s_lock),
	GROUP_T(TRN(SET_ROW_MY_RADIO, "My radio"), TRN(SET_TITLE_MY_RADIO, "My Radio"), s_my_radio),
		GROUP(TRN(SET_ROW_ADVANCED, "Advanced"), s_advanced),
};
#define GROUPS	(sizeof groups / sizeof groups[0])

uint8_t ui_set_group_count(void) { return GROUPS; }
tr_id_t ui_set_group_name(uint8_t g) { return g < GROUPS ? groups[g].name : TR_NONE; }

#if RFOUNDRY_HWOPTS
#define HWOPT_ROWS_MAX		15

// The hardware options Settings > Advanced > Hardware lists (hal_hwopt.h): those the board didn't detect
static uint16_t hwopt_visible(void)
{
	uint16_t n = 0;
	for (uint8_t k = 0; k < board_hwopt_count() && n < HWOPT_ROWS_MAX; k++) {
		n += hwopt_state(board_hwopt(k)) != HWOPT_ST_HIDDEN;
	}
	return n;
}
#endif

// Settings a radio without a battery (or a broadcast receiver, or the key) doesn't have are left out of the list, not
// shown disabled
static bool set_hidden(const set_item_t *s)
{
#if RFOUNDRY_HWOPTS
	if (s->type == ST_ACTION && s->id == SA_HARDWARE)
		return hwopt_visible() == 0;	// none, or every one detected
#endif
#if CONFIG_KEY_ASSIGN
	if (s->type != ST_ACTION && s->id >= UI_SET_KEY_FIRST && s->id <= UI_SET_KEY_LAST) {
		key_labels();
		// a key this radio doesn't have (side keys, then the keys programmable on Home)
		return (s->id - UI_SET_KEY_FIRST) / 2 >= board.side_key_count + board.home_key_count;
	}
#endif
	if (s->type != ST_ACTION && s->id == UI_SET_VOLUME)
		return !(feats() & UI_FEAT_VOLUME);	// a knob sets the volume on the other radios
	if (s->type != ST_ACTION && s->id == UI_SET_KNOB_WHEN_LOCKED)
		return !(feats() & UI_FEAT_VOLUME_KNOB);	// only where the MCU reads the knob
	if (s->type != ST_ACTION && s->id == UI_SET_BCAST_STEP)
		return !(feats() & UI_FEAT_BCAST);
#if CONFIG_RX_EXT
	if (s->type != ST_ACTION && s->id == UI_SET_RX_EXT)
		return board.rx_ext_count == 0;	// a board with no measured range past its bands
#endif
	if (s->type != ST_ACTION && s->id == UI_SET_REF_TRIM)
		return !(feats() & UI_FEAT_REF_TRIM);	// only a radio whose board has the trim
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

// A row's name in the language in use, or in English (Find matches both). Only a key's row has no key: it is named
// after the key, built in the language in use (by set_hidden, which set_nth asked), the same text either way
static const char *set_name(const set_item_t *s, bool english)
{
#if CONFIG_KEY_ASSIGN
	if (s->name == TR_NONE)
		return key_label[s->id - UI_SET_KEY_FIRST];
#endif
	return english ? tr_en(s->name) : tr(s->name);
}

const char *ui_set_item_name(uint8_t g, uint8_t i, bool english, const char **synonyms)
{
	const set_item_t *s = set_nth(g, i);
	*synonyms = s != NULL ? s->syn : NULL;
	return s != NULL ? set_name(s, english) : "";
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
	str_copy(it->label, sizeof it->label, tr(groups[i].name));
	item_num(it, (uint16_t)(i + 1u));
}

static void settings_select(uint16_t i, bool hold)
{
	(void)hold;
	ui_push(SCR_GROUP, i);
}

static const ui_scr_def_t settings_screen = {
	.name = "settings", .flags = SF_LIST | SF_NUMBERED, .prefix = TRN(TITLE_SETTINGS, "Settings"),
	.hint = TRN(HINT_0_9_OPEN_BACK, "0-9 open  Back"),
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

#if CONFIG_KEY_ASSIGN
// A home key left at Default (P1, P2: board.home_keys) does on Home what its arrow does there: the zone. Say that
// instead of "Default". NULL for any other key or value
static const char *home_key_default(uint8_t id, int32_t v)
{
	uint8_t k = (uint8_t)(id - UI_SET_KEY_FIRST);
	if (v != KEYFN_DEFAULT || id < UI_SET_KEY_FIRST || id > UI_SET_KEY_LAST || k / 2u < board.side_key_count ||
	    k / 2u >= board.side_key_count + board.home_key_count)
		return NULL;
	uint8_t bit = board.home_keys[k / 2u - board.side_key_count];
	for (uint8_t i = 0; i < board.keymap_len; i++) {
		if (board.keymap[i].bit != bit)
			continue;
		uint8_t a = (k & 1u) ? board.keymap[i].long_action : board.keymap[i].short_action;
		if (a == ACT_NAV_LEFT || a == ACT_USER_1 || a == ACT_ZONE_DOWN)	// ui_home_key
			return TR(KEYFN_VALUE_PREV_ZONE, "Prev zone");
		if (a == ACT_NAV_RIGHT || a == ACT_USER_4 || a == ACT_ZONE_UP)
			return TR(KEYFN_VALUE_NEXT_ZONE, "Next zone");
	}
	return NULL;
}
#endif

static void set_value_text(const set_item_t *s, int32_t v, char *out, size_t len)
{
	sbuf_t b;
	sb_init(&b, out, len);
	switch (s->type) {
	case ST_TOGGLE:
		sb_str(&b, v ? TR(VALUE_ON, "On") : TR(VALUE_OFF, "Off"));
		break;
	case ST_LIST:
#if CONFIG_KEY_ASSIGN
		{
			const char *home = home_key_default(s->id, v);
			if (home != NULL) {
				sb_str(&b, home);
				break;
			}
		}
#endif
		for (uint8_t k = 0; k < s->n; k++) {
			if (s->values[k] == v) {
				sb_str(&b, tr(s->names[k]));
				return;
			}
		}
		sb_i32(&b, v);
		break;
	case ST_NUMBER:
#if CONFIG_REF_TRIM
		if (s->id == UI_SET_REF_TRIM) {
			sb_ppm(&b, v);
			break;
		}
#endif
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
		return TR(WHY_VOICE_PROMPTS_NOT_IN_BUILD, "Voice prompts: not in this build");
	case UI_SET_TALK_PERMIT:
		return CONFIG_DMR && CONFIG_BEEP ? NULL :
			TR(WHY_PERMIT_TONE_NOT_IN_BUILD, "Talk permit tone: not in this build");
	case UI_SET_CALL_END_TONE:
		return CONFIG_DMR && CONFIG_BEEP ? NULL : CONFIG_BEEP ?
			TR(WHY_DMR_NOT_IN_BUILD, "DMR: not in this build") :
			TR(WHY_TONES_NOT_IN_BUILD, "Tones: not in this build");
	case UI_SET_TOT_WARN_TONE:
	case UI_SET_BATTERY_TONE:
	case UI_SET_TX_DENY_TONE:
		return CONFIG_BEEP ? NULL : TR(WHY_TONES_NOT_IN_BUILD, "Tones: not in this build");
	case UI_SET_SCAN_TONE:
		return !CONFIG_BEEP ? TR(WHY_TONES_NOT_IN_BUILD, "Tones: not in this build") : feats() & UI_FEAT_SCAN ?
			NULL : TR(WHY_SCAN_NOT_IN_BUILD, "Scan: not in this build");
	case UI_SET_EMERGENCY_TONE:
		return !CONFIG_BEEP ? TR(WHY_TONES_NOT_IN_BUILD, "Tones: not in this build") :
			feats() & UI_FEAT_EMERGENCY ? NULL :
				TR(WHY_EMERGENCY_NOT_IN_BUILD, "Emergency: not in this build");
	case UI_SET_MESSAGE_TONE:
		return (feats() & UI_FEAT_SMS) || CONFIG_APRS_AFSK ? NULL :
			TR(WHY_MESSAGES_NOT_IN_BUILD, "Messages: not in this build");
	case UI_SET_VIBRATE:
		return feats() & UI_FEAT_VIBRATE ? NULL :
			TR(SET_WHY_NO_VIBRATION_MOTOR, "This radio has no vibration motor");
	case UI_SET_THEME:
	case UI_SET_ACCENT:
	case UI_SET_STRIP:
		return board.display != NULL && board.display->bpp == 1 ?
			TR(SET_WHY_COLOR_SCREENS_ONLY, "Colors are for color screens") : NULL;
	case UI_SET_MIC_GAIN:
		if (!mic_gain_available())
			return TR(WHY_MIC_GAIN_NOT_IN_BUILD, "Mic gain: not in this build");
		return mic_agc_on() ? TR(SET_WHY_MIC_AGC_SETS_LEVEL, "Mic AGC sets the level") : NULL;
	case UI_SET_NOISE_REDUCTION:
		if (!CONFIG_DMR)
			return TR(WHY_NOISE_REDUCTION_NOT_IN_BUILD, "Noise reduction: not in this build");
		return vocoder_has(VOCODER_OPT_NOISE_REDUCTION) ? NULL : TR(WHY_NOT_ON_THIS_RADIO, "Not on this radio");
	case UI_SET_MIC_AGC:
		if (!CONFIG_DMR)
			return TR(WHY_MIC_AGC_NOT_IN_BUILD, "Mic AGC: not in this build");
		return vocoder_has(VOCODER_OPT_AGC) ? NULL : TR(WHY_NOT_ON_THIS_RADIO, "Not on this radio");
	case UI_SET_HEAR_ALL_DEFAULT:
		return feats() & UI_FEAT_HEAR_ALL ? NULL :
			TR(WHY_DMON_NOT_IN_BUILD, "Digital monitor: not in this build");
	case UI_SET_ALIAS_SHOW:
		return CONFIG_DMR_TALKER_ALIAS ? NULL :
			TR(WHY_TALKER_ALIAS_NOT_IN_BUILD, "Talker alias: not in this build");
	case UI_SET_PRIVACY_TYPE:
	case UI_SET_PRIVACY_KEY:
		if (!(feats() & UI_FEAT_PRIVACY))
			return TR(WHY_PRIVACY_NOT_IN_BUILD, "Privacy: not in this build");
		return s->id == UI_SET_PRIVACY_KEY && ui_svc_setting_get(UI_SET_PRIVACY_TYPE) == 0 ?
			TR(SET_WHY_PICK_KEY_TYPE_FIRST, "Pick a key type first") : NULL;
	case UI_SET_GPS:
	case UI_SET_POS_FORMAT:
	case UI_SET_POS_SEND_S:
		return board_has_gps() ? NULL : TR(SET_WHY_NO_GPS_ON_RADIO, "No GPS on this radio");
	case UI_SET_POS_DMR_CALLS:
		return CONFIG_DMR_GPS ? NULL : TR(WHY_DMR_POSITION_NOT_IN_BUILD, "DMR position: not in this build");
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
	str_copy(it->label, sizeof it->label, set_name(s, false));
	bool edit = set_editing && i == ui_frame()->focus;
	if (s->type != ST_ACTION)
		set_value_text(s, edit ? set_value : ui_svc_setting_get(s->id), it->value, sizeof it->value);
	else if (s->id == SA_LANGUAGE)
		str_copy(it->value, sizeof it->value, lang_name(lang_current()));
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
		ui_toast(TR(TOAST_SAVE_FAILED, "Couldn't save: try again"));
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
#if CONFIG_REF_TRIM
	if (s->id == UI_SET_REF_TRIM && s->type == ST_NUMBER) {
		ui_push(SCR_REF_TRIM, 0);
		return;
	}
#endif
	int32_t v = ui_svc_setting_get(s->id);
	switch (s->type) {
	case ST_TOGGLE:
		set_store(s, !v);
		break;
	case ST_LIST: {
		uint8_t k = 0;
		while (k < s->n && s->values[k] != v)
			k++;
		uint8_t next = (uint8_t)((k + 1u) % s->n);
#if CONFIG_KEY_ASSIGN
		// A key's slot skips what it can't hold (a home key: Monitor, Tone burst)
		if (s->id >= UI_SET_KEY_FIRST && s->id <= UI_SET_KEY_LAST) {
			for (uint8_t t = 0; t < s->n && !ui_svc_key_offered(s->id, s->values[next]); t++)
				next = (uint8_t)((next + 1u) % s->n);
		}
#endif
		set_store(s, s->values[next]);
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
				ui_toast(TR(WHY_DIAGNOSTICS_NOT_IN_BUILD, "Diagnostics: not in this build"));
			break;
		case SA_FIRMWARE: ui_push(SCR_ABOUT, 0); break;
		case SA_CODEPLUG: ui_push(SCR_INFO, INFO_CODEPLUG); break;
		case SA_MY_RADIO: ui_push(SCR_DMR_ID, 0); break;
		case SA_RESET: ui_push(SCR_CONFIRM, 0); break;
		case SA_LANGUAGE: ui_push(SCR_LANGUAGE, 0); break;
#if DATA_IDS
		case SA_DATA_SERVICES: ui_push(SCR_DATA_SERVICES, 0); break;
#if CONFIG_BACKLIGHT_TEST
		case SA_BL_TEST: ui_push(SCR_BL_TEST, 0); break;
#endif
#endif
#if CONFIG_CRASH_SCREEN
		case SA_CRASH: ui_push(SCR_CRASH, 0); break;
#endif
#if RFOUNDRY_HWOPTS
		case SA_HARDWARE: ui_push(SCR_HWOPT, 0); break;
#endif
#if CONFIG_POSITION
		case SA_FIXED_POS: ui_push(SCR_FIXED_POS, 0); break;
#endif
		default: break;
		}
		break;
	default:
		break;
	}
	if (hold && !set_editing && s->type != ST_ACTION)
		ui_toast(TR(SET_TOAST_SAVED_AS_YOU_CHANGE, "Settings are saved as you change them"));
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
			sb_str(&b, TR(SET_TOAST_NOT_CHANGED, "Not changed: "));
			sb_i32(&b, lo);
			sb_str(&b, TR(SET_TOAST_RANGE_TO, " to "));
			sb_i32(&b, hi);
			if (s->step > 1) {
				sb_str(&b, TR(SET_TOAST_STEPS_OF, ", steps of "));
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
	return g < GROUPS ? tr(groups[g].title) : "";
}

static const ui_scr_def_t group_screen = {
	.name = "group", .flags = SF_LIST, .hint = TRN(HINT_OK_CHANGE_BACK, "OK change  Back"),
	.count = group_count, .item = group_item, .select = group_select, .key = group_key, .enter = group_enter,
	.title = group_title,
};

#if RFOUNDRY_HWOPTS
// ======================= Hardware options (hal_hwopt.h) =======================
// OK steps through an option's choices without storing; holding OK stores the one shown. A choice the board marks
// "restart" applies from the next start-up. One row at a time shows an unstored choice, kept in the screen's frame
// argument (no static: RAM is tight on some radios): the row + 1 in the high byte, 0 = none, the choice in the low one.
// Stepping another row drops it

static void hwopt_enter(void)
{
	ui_frame()->arg = 0;
}

// The rows: the board's options less those it detected (HWOPT_ST_HIDDEN)
static uint16_t hwopt_count(void)
{
	return hwopt_visible();
}

// Row i's option
static const hal_hwopt_t *hwopt_row(uint16_t i)
{
	for (uint8_t k = 0; k < board_hwopt_count(); k++) {
		if (hwopt_state(board_hwopt(k)) != HWOPT_ST_HIDDEN && i-- == 0) {
			return board_hwopt(k);
		}
	}
	return board_hwopt(0);	// not reached: i < hwopt_count()
}

static int8_t hwopt_shown(uint16_t i, int8_t stored)
{
	uint16_t p = ui_frame()->arg;
	return (p >> 8) == i + 1u ? (int8_t)(p & 0xFFu) : stored;
}

static void hwopt_item(uint16_t i, ui_item_t *it)
{
	const hal_hwopt_t *o = hwopt_row(i);
	int8_t stored = o->get();
	int8_t c = hwopt_shown(i, stored);
	sbuf_t b;
	sb_init(&b, it->label, sizeof it->label);
	sb_str(&b, o->name);
	sb_init(&b, it->value, sizeof it->value);
	sb_str(&b, c >= 0 && c < o->count ? o->choices[c] : "Not set");
	if (c != stored)
		sb_chr(&b, '*');	// shown, not stored yet
	if (hwopt_state(o) == HWOPT_ST_ASK)
		it->flags |= UI_ITEM_WARN;
}

static void hwopt_select(uint16_t i, bool hold)
{
	const hal_hwopt_t *o = hwopt_row(i);
	int8_t stored = o->get();
	int8_t c = hwopt_shown(i, stored);
	if (!hold) {
		ui_frame()->arg = (uint16_t)(((i + 1u) << 8) | (uint8_t)(c < 0 ? 0 : (c + 1) % o->count));
		return;
	}
	if (c < 0) {
		ui_toast("Press OK to choose first");
	} else if (c == stored) {
		ui_toast("Already stored");
	} else if (!o->set((uint8_t)c)) {
		ui_toast("Not stored");
	} else {
		ui_frame()->arg = 0;
		// A board that reports the state says whether a restart is still due (none when the choice is what runs)
		bool restart = o->state != NULL ? hwopt_state(o) == HWOPT_ST_RESTART : o->restart;
		ui_toast(restart ? "Stored: restart to apply" : "Stored");
	}
}

static const ui_scr_def_t hwopt_screen = {
	.name = "hwopt", .flags = SF_LIST, .prefix = TRX(HW_TITLE, "Hardware"),
	.hint = TRX(HW_HINT, "OK next  Hold OK store"),
	.count = hwopt_count, .item = hwopt_item, .select = hwopt_select, .enter = hwopt_enter,
};
#endif

// ======================= Confirmation (destructive only, defaults to No; P7) =======================

static uint16_t confirm_count(void) { return 2; }

static void confirm_item(uint16_t i, ui_item_t *it)
{
	str_copy(it->label, sizeof it->label, i == 0 ? TR(RESET_ROW_KEEP, "No, keep settings") :
		TR(RESET_ROW_CONFIRM, "Yes, reset"));
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
		ui_toast(TR(RESET_TOAST_DONE, "Factory settings restored"));
	} else {
		ui_toast(TR(WHY_RESET_NOT_IN_BUILD, "Reset: not in this build"));
	}
}

static const char *confirm_body(void)
{
	return TR(RESET_ASK, "Reset all settings to factory defaults? Channels stay. This can't be undone.");
}

static const ui_scr_def_t confirm_screen = {
	.name = "confirm", .flags = SF_LIST, .prefix = TRN(RESET_TITLE, "Factory Reset"),
	.hint = TRN(HINT_OK_CHOOSE_BACK_CANCELS, "OK choose  Back cancel"),
	.count = confirm_count, .item = confirm_item, .select = confirm_select, .body = confirm_body,
};
#else
uint8_t ui_set_group_count(void) { return 0; }
tr_id_t ui_set_group_name(uint8_t g) { (void)g; return TR_NONE; }
uint8_t ui_set_item_count(uint8_t g) { (void)g; return 0; }
const char *ui_set_item_name(uint8_t g, uint8_t i, bool english, const char **synonyms)
{
	(void)g; (void)i; (void)english;
	*synonyms = NULL;
	return "";
}
void ui_open_setting(uint8_t g, uint8_t i) { (void)g; (void)i; }

static uint16_t settings_count(void) { return 1; }
static void settings_item(uint16_t i, ui_item_t *it)
{
	(void)i;
	str_copy(it->label, sizeof it->label, TR(SET_EMPTY_USE_QDMR, "Set them with qdmr"));
	it->flags |= UI_ITEM_INFO;
}

static const ui_scr_def_t settings_screen = {
	.name = "settings", .flags = SF_LIST, .prefix = TRN(TITLE_SETTINGS, "Settings"), .hint = TRN(HINT_BACK, "Back"),
	.count = settings_count, .item = settings_item,
};
#define group_screen	settings_screen
#define confirm_screen	settings_screen
#endif

// ======================= Talk target picker (SK2, quick 8) =======================

#define PICK_MAX	12
typedef struct {
	ui_target_t t;
	tr_id_t tag;
} pick_t;
static pick_t picks[PICK_MAX];
static uint8_t n_picks;

#if CONFIG_DMR
static void pick_add(const ui_target_t *t, tr_id_t tag)
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
	uint8_t reply = PICK_MAX;	// the reply target's row
	// The VFO's own target (it has no channel to take one from)
	if (v->vfo && v->dmr.target_id != 0) {
		memset(&t, 0, sizeof t);
		t.id = v->dmr.target_id;
		t.group = !v->dmr.target_private;
		strcpy(t.name, v->dmr.target_name);
		pick_add(&t, TRN(PICKER_CHIP_CURRENT, "Current"));
	}
	// The channel's default
	uint16_t def = codeplug_channel_talk_contact(&v->ch);
	if (def && codeplug_contact(def, &c)) {
		memset(&t, 0, sizeof t);
		t.id = c.id;
		t.group = c.type != CONTACT_PRIVATE;
		strcpy(t.name, c.name);
		pick_add(&t, TRN(VALUE_DEFAULT, "Default"));
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
		reply = n_picks;
		pick_add(&t, TRN(PICKER_CHIP_REPLY, "Reply"));
		if (reply >= n_picks)
			reply = PICK_MAX;	// already there under another tag
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
				pick_add(&t, TRN(PICKER_CHIP_LISTEN, "Listen"));
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
			pick_add(&t, TRN(TITLE_RECENT, "Recent"));
		}
	}
	// Focus the reply target when there is one (task-flows.md 3: "TG 91 Reply" second)
	if (reply < n_picks)
		ui_frame()->focus = reply;
#endif
}

// After the targets: Type a number..., then Select contact... (every contact, as Contacts lists them)
#define PICK_CONTACTS	(CONFIG_DMR ? 1u : 0u)

static uint16_t picker_count(void) { return (uint16_t)(n_picks + 1u + PICK_CONTACTS); }

static void picker_item(uint16_t i, ui_item_t *it)
{
	if (i >= n_picks) {
		str_copy(it->label, sizeof it->label, i == n_picks ? TR(PICKER_ROW_TYPE_NUMBER, "Type a number...") :
			TR(PICKER_ROW_SELECT_CONTACT, "Select contact..."));
		return;
	}
	ui_sb_target(it->label, sizeof it->label, &picks[i].t);
	sbuf_t b;	// the tag, left out where the target needs the room
	sb_init(&b, it->value, sizeof it->value);
	sb_str(&b, TEXT_ALT_OPEN);
	sb_str(&b, tr(picks[i].tag));
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
	if (i > n_picks) {
		ui_push(SCR_CONTACTS, 0);	// PTT, OK, hold OK and # work there as here
		return;
	}
	if (i == n_picks) {
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
	.name = "picker", .flags = SF_LIST | SF_FIELD, .prefix = TRN(PICKER_TITLE, "Talk to"),
	.hint = TRN(PICKER_HINT, TEXT_ALT_OPEN "PTT once  OK use  Hold OK set"
		TEXT_ALT_NEXT "OK use  Hold OK set" TEXT_ALT_CLOSE),
	.count = picker_count, .item = picker_item, .select = picker_select, .context = picker_context,
	.target = picker_target, .enter = picker_enter, .title = picker_title,
};

// ======================= Scan: what to do with the channel that stopped it =======================

static uint16_t scan_count(void) { return 3; }

static void scan_item(uint16_t i, ui_item_t *it)
{
	static const tr_id_t names[] = { TRN(SCAN_ROW_SKIP_FOR_NOW, "Skip channel for now"),
		TRN(SCAN_ROW_STAY_HERE, "Stay here"), TRN(SCAN_ROW_ALWAYS_SKIP, "Always skip") };
	str_copy(it->label, sizeof it->label, tr(names[i]));
	item_num(it, (uint16_t)(i + 1u));
	if (i == 2)
		it->flags |= UI_ITEM_DIM;
}

static void scan_select(uint16_t i, bool hold)
{
	(void)hold;
	if (i == 0) {
		ui_go_home();
		ui_toast(ui_svc_scan_skip() ? TR(SCAN_TOAST_SKIPPED_UNTIL_STOP, "Skipped until the scan stops") :
			TR(SCAN_TOAST_NOTHING_TO_SKIP, "Nothing to skip yet"));
	} else if (i == 1) {
		ui_svc_scan(false);
		ui_st.scanning = false;
		ui_go_home();
		ui_toast(TR(SCAN_TOAST_STOPPED_HERE, "Scan stopped here"));
	} else {
		ui_toast(TR(SCAN_TOAST_LISTS_USE_QDMR, "Edit scan lists in qdmr"));
	}
}

static const ui_scr_def_t scan_screen = {
	.name = "scan", .flags = SF_LIST | SF_NUMBERED | SF_FIELD, .prefix = TRN(WORD_SCAN, "Scan"),
	.hint = TRN(HINT_1_3_PICK_BACK, "1-3 pick  Back"),
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

// ======================= Settings > Display > Language =======================
// Every language a catalog exists for, each in its own name; one this image was built without is dimmed and says so
// (the flasher builds it in, docs/user)

static uint16_t lang_list_count(void) { return lang_count(); }

static void lang_list_item(uint16_t i, ui_item_t *it)
{
	str_copy(it->label, sizeof it->label, lang_name((uint8_t)i));
	if (i == lang_current())
		str_copy(it->value, sizeof it->value, TR(LANG_VALUE_IN_USE, "In use"));
	if (!lang_built((uint8_t)i))
		it->flags |= UI_ITEM_DIM;
}

static void lang_list_select(uint16_t i, bool hold)
{
	(void)hold;
	if (!lang_built((uint8_t)i)) {
		// a fixed text: no RAM for a buffer naming the language
		ui_toast(TR(WHY_NOT_IN_BUILD, "Not in this build"));
		return;
	}
	if (!ui_svc_setting_set(UI_SET_LANGUAGE, lang_value((uint8_t)i))) {
		ui_toast(TR(TOAST_SAVE_FAILED, "Couldn't save: try again"));
		return;
	}
	ui_apply_setting(UI_SET_LANGUAGE, lang_value((uint8_t)i));
	ui_pop();
}

static void lang_list_enter(void)
{
	ui_frame()->focus = lang_current();
}

static const ui_scr_def_t lang_screen = {
	.name = "language", .flags = SF_LIST, .prefix = TRN(SET_LABEL_LANGUAGE, "Language"),
	.hint = TRN(HINT_OK_PICK_BACK, "OK pick  Back"),
	.count = lang_list_count, .item = lang_list_item, .select = lang_list_select, .enter = lang_list_enter,
};

// ======================= Another firmware's codeplug: the first edit asks (defaults to Cancel; P7) ===============
// ui_svc_codeplug_answer: the rows are UI_CP_CANCEL, UI_CP_NEW, UI_CP_USE. BACK is Cancel (ui_pop drops the write)

static uint16_t cpf_count(void) { return 3; }

static void cpf_item(uint16_t i, ui_item_t *it)
{
	str_copy(it->label, sizeof it->label, i == UI_CP_CANCEL ? TR(CPLUG_ROW_CANCEL, "Cancel") : i == UI_CP_NEW ?
		TR(CPLUG_ROW_START_NEW, "Start new") :
		 TR(CPLUG_ROW_USE_ANYWAY, "Use anyway"));
	if (i == UI_CP_NEW)
		it->flags |= UI_ITEM_WARN;
}

static void cpf_select(uint16_t i, bool hold)
{
	(void)hold;
	if (i != UI_CP_CANCEL && !ui_svc_codeplug_answer((uint8_t)i)) {
		ui_toast(TR(TOAST_BUSY_SAVING, "Busy saving: try again"));
		return;
	}
	ui_pop();	// Cancel: the pop drops the write and says so
	if (i != UI_CP_CANCEL)
		ui_toast(i == UI_CP_NEW ? TR(CPLUG_TOAST_NEW_STARTED, "New codeplug started") :
			TR(CPLUG_TOAST_OLD_KEPT, "Old codeplug kept: edit again"));
}

static const char *cpf_body(void)
{
	return TR(CPLUG_ASK_OTHER_FIRMWARE,
		"Another firmware's codeplug? Start new erases contacts, callsign and DMR ID. Back up first.");
}

static const ui_scr_def_t cpf_screen = {
	.name = "codeplug-foreign", .flags = SF_LIST, .prefix = TRN(CPLUG_TITLE_OLD, "Old codeplug"),
	.hint = TRN(HINT_OK_CHOOSE_BACK_CANCELS, "OK choose  Back cancel"),
	.count = cpf_count, .item = cpf_item, .select = cpf_select, .body = cpf_body,
};

const ui_scr_def_t *const ui_scr[SCR_COUNT] = {
	[SCR_HOME] = &home_screen, [SCR_QUICK] = &quick_screen, [SCR_HUB] = &hub_screen,
	[SCR_FIND] = &ui_find_screen, [SCR_ACTIONS] = &actions_screen, [SCR_CONTACTS] = &contacts_screen,
	[SCR_MESSAGES] = &messages_screen, [SCR_MESSAGE] = &message_screen, [SCR_COMPOSE] = &compose_screen,
	[SCR_MESSAGE_READ] = &message_read_screen,
	[SCR_RECENT] = &recent_screen, [SCR_ZONES] = &channels_screen, [SCR_ZONE] = &zone_screen,
	[SCR_TOOLS] = &tools_screen, [SCR_SETTINGS] = &settings_screen, [SCR_GROUP] = &group_screen,
	[SCR_PICKER] = &picker_screen, [SCR_INFO] = &info_screen, [SCR_CONFIRM] = &confirm_screen,
	[SCR_DIAG] = &diag_screen, [SCR_EMERGENCY] = &emergency_screen, [SCR_SCAN] = &scan_screen,
	[SCR_ROAM] = &roam_screen, [SCR_APRS] = &ui_aprs_screen, [SCR_APRS_HEARD] = &ui_aprs_heard_screen,
	[SCR_APRS_STATION] = &ui_aprs_station_screen,
	[SCR_ABOUT] = &about_screen,
#if CONFIG_POSITION
	[SCR_FIXED_POS] = &ui_fixed_pos_screen,
#endif
	[SCR_VOLUME] = &volume_screen, [SCR_LANGUAGE] = &lang_screen,
	[SCR_DMR_ID] = &dmr_id_screen,
	[SCR_CP_FOREIGN] = &cpf_screen,
#if DATA_IDS
	[SCR_DATA_SERVICES] = &ds_screen,
#endif
#if CONFIG_BACKLIGHT_TEST
	[SCR_BL_TEST] = &bl_test_screen,
#endif
#if CONFIG_REF_TRIM
	[SCR_REF_TRIM] = &trim_screen,
#endif
#if CONFIG_APRS_AFSK
	[SCR_APRS_TO] = &aprs_to_screen,
#endif
#if CONFIG_DMR_SMS || CONFIG_APRS_AFSK
	[SCR_MESSAGE_TEXT] = &message_text_screen,
#endif
#if CONFIG_CHANNEL_LIST
	[SCR_CHANNELS] = &chlist_screen, [SCR_CHANNEL] = &chinfo_screen,
#if CONFIG_CHANNEL_EDIT
	[SCR_TEXT] = &text_screen, [SCR_DISCARD] = &discard_screen,
#if CONFIG_CONTACT_EDIT
	[SCR_CONTACT_EDIT] = &contact_edit_screen,
#endif
#if CONFIG_ZONE_EDIT
	[SCR_ZCHANNEL] = &zchan_screen, [SCR_ZPICK] = &zpick_screen, [SCR_ZONEEDIT] = &zopt_screen,
	[SCR_ZONEDEL] = &zdel_screen, [SCR_CHZONES] = &chz_screen,
#endif
#if CONFIG_DMR
	[SCR_CHPICK] = &chpick_screen,
#endif
#endif
#endif
#if CONFIG_GPS_SKY
	[SCR_GPS] = &ui_gps_screen,
#endif
#if CONFIG_CRASH_SCREEN
	[SCR_CRASH] = &crash_screen,
#endif
#if CONFIG_UI_SETTINGS && RFOUNDRY_HWOPTS
	[SCR_HWOPT] = &hwopt_screen,
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
