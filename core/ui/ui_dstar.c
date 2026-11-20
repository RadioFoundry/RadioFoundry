// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// D-STAR (docs/spec/ui.md 2.6, docs/spec/dstar-codeplug.md): what the next over sends and the shortcuts that change it,
// and the station the radio sends as. CONTEXT on Home (or the talk target key) opens it on a D-STAR channel; Find
// ("dstar", "echo", "reflector") and Settings > My radio reach it too. The routing rows only set UR, as the radio's
// owner would by hand: Call CQ and the caller's callsign for now (until the channel changes or BACK on Home), the
// reflector commands (link, unlink, echo, gateway information) for one over. Each sends the user Home, where the talk
// line says what PTT does now. The station rows (MY, suffix, message) are kept in the radio's settings, over the
// codeplug's D-STAR record.
#include <string.h>
#include "ui_nav.h"
#include "fmt.h"
#include "i18n.h"
#include "config.h"
#include "radio/radio.h"

#if CONFIG_DSTAR

// Weak stand-ins for the services (core/ui/ui_svc_dstar.c has the radio's, the UI simulator its own).
// The station set on the radio (settings_t dstar_*), field 0 MY, 1 suffix, 2 message; text "" = the codeplug's
#define WEAK __attribute__((weak))
WEAK bool ui_svc_dstar_station(uint8_t field, char *out, uint8_t cap)
{
	(void)field;
	if (cap > 0) {
		out[0] = '\0';
	}
	return false;
}
WEAK bool ui_svc_dstar_station_set(uint8_t field, const char *text) { (void)field; (void)text; return false; }
WEAK void ui_svc_dstar_ur(uint8_t mode, const char *ref) { (void)mode; (void)ref; }

enum { DS_SEND_TO, DS_CQ, DS_REPLY, DS_ECHO, DS_LINK, DS_UNLINK, DS_INFO, DS_MY, DS_SUFFIX, DS_MSG, DS_COUNT };

// The routing rows act only on a D-STAR channel; the station rows anywhere
static bool on_dstar(void)
{
	return ui_view()->dstar.on;
}

// The text editor's buffer and which row it edits
static char edit_text[24];
static uint8_t edit_row = DS_COUNT;


static uint16_t ds_count(void) { return DS_COUNT; }

static void ds_item(uint16_t i, ui_item_t *it)
{
	static const tr_id_t names[DS_COUNT] = { TRN(DSTAR_ROW_SEND_TO, "Send to"), TRN(DSTAR_ROW_CALL_CQ, "Call CQ"),
		TRN(DSTAR_ROW_CALL_CALLER, "Call last caller"), TRN(DSTAR_ROW_ECHO, "Echo test"),
		TRN(DSTAR_ROW_LINK, "Link reflector"), TRN(DSTAR_ROW_UNLINK, "Unlink"),
		TRN(DSTAR_ROW_INFO, "Gateway info"), TRN(DSTAR_ROW_MY_CALL, "My call"),
		TRN(DSTAR_ROW_SUFFIX, "Suffix"), TRN(DSTAR_ROW_MESSAGE, "Message") };
	if (i >= DS_COUNT) {
		return;
	}
	item_num_set(it, (uint16_t)(i + 1u));
	str_copy(it->label, sizeof it->label, tr(names[i]));
	sbuf_t b;
	sb_init(&b, it->value, sizeof it->value);
	bool here = on_dstar();
	const radio_dstar_view_t *d = &ui_view()->dstar;
	switch (i) {
	case DS_SEND_TO:
		if (here) {
			ui_dstar_ur_text(d, &b, false);
			if (d->ur_mode != RDSTAR_UR_CHANNEL) {
				it->flags |= UI_ITEM_TEMP;
			}
		}
		if (!here) {
			it->flags |= UI_ITEM_DIM;
		}
		break;
	case DS_REPLY:
		if (here && d->heard && d->my[0] != '\0') {
			sb_str(&b, d->my);
			break;
		}
		it->flags |= UI_ITEM_DIM;
		break;
	case DS_CQ: case DS_ECHO: case DS_LINK: case DS_UNLINK: case DS_INFO:
		if (!here) {
			it->flags |= UI_ITEM_DIM;
		}
		break;
	case DS_MY: case DS_SUFFIX: case DS_MSG: {
		char t[24];
		uint8_t field = (uint8_t)(i - DS_MY);
		if (!ui_svc_dstar_station(field, t, sizeof t)) {
			it->flags |= UI_ITEM_DIM;
			break;
		}
		// Nothing set on the radio: what goes out (the codeplug's record or the radio name)
		if (t[0] == '\0' && here) {
			str_copy(t, sizeof t, field == 0 ? d->tx_my : field == 1 ? d->tx_suffix : "");
		}
		sb_str(&b, t[0] != '\0' ? t : TR(VALUE_NONE, "None"));
		break;
	}
	default:
		break;
	}
}

static void ur_done(uint8_t mode, const char *ref, const char *toast)
{
	ui_svc_dstar_ur(mode, ref);
	ui_go_home();
	ui_toast(toast);
}

static void link_kept(void)
{
	size_t n = strlen(edit_text);
	if (n < 2 || n > 7) {
		ui_toast(TR(DSTAR_TOAST_LINK_FORMAT, "Reflector and module: REF030C"));
		return;
	}
	ur_done(RDSTAR_UR_LINK, edit_text, TR(DSTAR_TOAST_LINK, "Link: press PTT once"));
}

static void station_kept(void)
{
	if (edit_row < DS_MY || edit_row > DS_MSG) {
		return;
	}
	if (!ui_svc_dstar_station_set((uint8_t)(edit_row - DS_MY), edit_text)) {
		ui_toast(TR(TOAST_SAVE_FAILED, "Couldn't save: try again"));
		return;
	}
	ui_toast(TR(TOAST_SAVED, "Saved"));
	ui_dirty();
}

static void ds_select(uint16_t i, bool hold)
{
	(void)hold;
	if (i < DS_MY && !on_dstar()) {
		ui_toast(TR(DSTAR_WHY_CHANNELS_ONLY, "D-STAR channels only"));
		return;
	}
	switch (i) {
	case DS_SEND_TO: ur_done(RDSTAR_UR_CHANNEL, NULL, TR(DSTAR_TOAST_CHANNEL_UR, "Back to the channel's UR")); break;
	case DS_CQ: ur_done(RDSTAR_UR_CQ, NULL, TR(DSTAR_TOAST_CQ, "Calling CQ")); break;
	case DS_REPLY:
		if (!ui_view()->dstar.heard || ui_view()->dstar.my[0] == '\0') {
			ui_toast(TR(DSTAR_TOAST_NO_CALLER, "No caller heard yet"));
			return;
		}
		ur_done(RDSTAR_UR_REPLY, NULL, TR(DSTAR_TOAST_CALLER, "Calling the last caller"));
		break;
	case DS_ECHO: ur_done(RDSTAR_UR_ECHO, NULL, TR(DSTAR_TOAST_ECHO, "Echo: talk, then listen")); break;
	case DS_UNLINK: ur_done(RDSTAR_UR_UNLINK, NULL, TR(DSTAR_TOAST_UNLINK, "Unlink: press PTT once")); break;
	case DS_INFO: ur_done(RDSTAR_UR_INFO, NULL, TR(DSTAR_TOAST_INFO, "Info: press PTT once")); break;
	case DS_LINK:
		edit_text[0] = '\0';
		edit_row = DS_LINK;
		ui_text_field(edit_text, 7, link_kept);
		break;
	case DS_MY: case DS_SUFFIX: case DS_MSG: {
		uint8_t field = (uint8_t)(i - DS_MY);
		if (!ui_svc_dstar_station(field, edit_text, sizeof edit_text)) {
			ui_toast(TR(WHY_DSTAR_NOT_IN_BUILD, "D-STAR: not in this build"));
			return;
		}
		edit_row = (uint8_t)i;
		ui_text_field(edit_text, i == DS_MY ? 8 : i == DS_SUFFIX ? 4 : 20, station_kept);
		break;
	}
	default:
		break;
	}
}

// Above the rows: the last transmission heard, its caller and their message
static const char *ds_body(void)
{
	static char t[96];
	sbuf_t b;
	sb_init(&b, t, sizeof t);
	const radio_dstar_view_t *d = &ui_view()->dstar;
	if (!d->on) {
		sb_str(&b, TR(DSTAR_BODY_NOT_HERE, "Not a D-STAR channel"));
		return t;
	}
	if (!d->heard) {
		sb_str(&b, TR(EMPTY_NO_CALLS_HEARD, "No calls heard yet"));
		return t;
	}
	sb_str(&b, TR(DSTAR_BODY_HEARD, "Heard "));
	sb_str(&b, d->my);
	if (d->suffix[0] != '\0') {
		sb_chr(&b, '/');
		sb_str(&b, d->suffix);
	}
	if (d->msg[0] != '\0') {
		sb_str(&b, " \"");
		sb_str(&b, d->msg);
		sb_chr(&b, '"');
	}
	return t;
}

const ui_scr_def_t ui_dstar_screen = {
	.name = "dstar", .flags = SF_LIST | SF_NUMBERED | SF_FIELD, .prefix = TRX(WORD_DSTAR, "D-STAR"),
	.hint = TRN(HINT_0_9_PICK_BACK, "0-9 pick  Back"),
	.count = ds_count, .item = ds_item, .select = ds_select, .body = ds_body,
};
#endif
