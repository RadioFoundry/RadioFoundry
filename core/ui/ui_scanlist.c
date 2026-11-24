// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Scan lists (Tools > Scan with CONFIG_SCAN_LIST; docs/spec/ui.md 2.7, docs/spec/scan-list.md): the lists, one list's
// channels, adding channels from any zone, and a list's options (rename, delete). The lists live in the scan list area;
// every write goes through ui_svc_scanlist_put / _delete (a job for the housekeeping task), so what is open here is a
// copy that the screens change first and hand over whole
#include "config.h"
#if CONFIG_SCAN_LIST
#include <string.h>
#include "ui_nav.h"
#include "fmt.h"
#include "i18n.h"
#include "codeplug/codeplug.h"

// ======================= The lists (Tools > Scan) =======================
// Row 0 scans the zone (the band in the VFO) as Tools > Scan did before; then the stored lists; then "New scan list"

typedef struct {
	uint8_t slot;
	uint8_t count;
	char name[CODEPLUG_NAME_LEN + 1];
} row_t;

static row_t rows[SCAN_LIST_MAX];
static uint8_t nrows;
static uint32_t seen_gen = 0xFFFFFFFFu;
static char focus_name[CODEPLUG_NAME_LEN + 1];	// the list to focus once its write has landed ("" = none)

static scan_list_t cur;				// the list the editor and the options work on
static uint8_t cur_slot;
static char new_name[CODEPLUG_NAME_LEN + 1];	// a name being typed
static char msg[CODEPLUG_NAME_LEN + 24];		// the toasts
static uint16_t pick_zone;			// the zone the channel picker lists

// The stored lists, read again whenever the area changed (ui_svc_scanlist_generation)
static void refresh(void)
{
	static scan_list_t l;
	uint32_t g = ui_svc_scanlist_generation();
	if (g == seen_gen)
		return;
	seen_gen = g;
	nrows = 0;
	uint16_t slots = ui_svc_scanlist_slots();
	for (uint16_t s = 0; s < slots && nrows < SCAN_LIST_MAX; s++) {
		if (!ui_svc_scanlist_get(s, &l))
			continue;
		row_t *r = &rows[nrows++];
		r->slot = (uint8_t)s;
		r->count = l.count;
		str_copy(r->name, sizeof r->name, l.name);
	}
	if (focus_name[0] && ui_frame()->scr == SCR_SLISTS) {
		for (uint8_t i = 0; i < nrows; i++)
			if (strcmp(rows[i].name, focus_name) == 0)
				ui_frame()->focus = (uint16_t)(1u + i);
		focus_name[0] = 0;
	}
}

static bool room(void)
{
	return nrows < ui_svc_scanlist_slots();
}

static uint16_t lists_count(void)
{
	refresh();
	return (uint16_t)(1u + nrows + (room() ? 1u : 0u));
}

static void toast2(const char *text, const char *name)
{
	sbuf_t b;
	sb_init(&b, msg, sizeof msg);
	sb_str(&b, text);
	sb_str(&b, name);
	ui_toast(msg);
}

static void lists_item(uint16_t i, ui_item_t *it)
{
	refresh();
	sbuf_t b;
	if (i == 0) {
		str_copy(it->label, sizeof it->label, ui_view()->vfo ? TR(SCANLIST_ROW_SCAN_BAND, "Scan band") : TR(SCANLIST_ROW_SCAN_ZONE, "Scan zone"));
		sb_init(&b, it->value, sizeof it->value);
		sb_str(&b, ui_st.scanning ? TR(VALUE_ON, "On") : TR(VALUE_OFF, "Off"));
		return;
	}
	if (i > nrows) {
		str_copy(it->label, sizeof it->label, TR(SCANLIST_ROW_NEW, "New scan list"));
		return;
	}
	const row_t *r = &rows[i - 1u];
	str_copy(it->label, sizeof it->label, r->name);
	sb_init(&b, it->value, sizeof it->value);
	sb_u32(&b, r->count, 1);
	sb_chr(&b, ' ');
	sb_str(&b, TR(SCANLIST_UNIT_CH, "ch"));
}

static void new_named(void)
{
	if (new_name[0] == 0)
		return;
	memset(&cur, 0, sizeof cur);
	str_copy(cur.name, sizeof cur.name, new_name);
	if (!ui_svc_scanlist_put(-1, &cur)) {
		ui_toast(TR(TOAST_BUSY_SAVING, "Busy saving: try again"));
		return;
	}
	str_copy(focus_name, sizeof focus_name, new_name);
	toast2(TR(ZONE_TOAST_ADDED, "Added "), new_name);
}

static void list_options(uint16_t i)
{
	if (i == 0 || i > nrows)
		return;
	ui_push(SCR_SLOPT, rows[i - 1u].slot);
}

static void lists_select(uint16_t i, bool hold)
{
	refresh();
	if (i == 0) {
		ui_go_home();
		ui_scan_toggle();	// says what scans; dual watch ends
		return;
	}
	if (i > nrows) {
		new_name[0] = 0;
		ui_text_open_done(new_name, CODEPLUG_NAME_LEN, new_named);
		return;
	}
	if (hold && !ui_has_action(ACT_CONTEXT)) {
		list_options(i);	// few-key boards: long OK opens what CONTEXT would
		return;
	}
	const row_t *r = &rows[i - 1u];
	if (ui_view()->vfo) {
		ui_toast(TR(SCANLIST_WHY_LEAVE_VFO, "Leave the VFO first"));
		return;
	}
	if (r->count < 2) {
		ui_toast(TR(SCANLIST_WHY_NEEDS_TWO_CHANNELS, "Add two channels first"));
		return;
	}
	ui_scan_list(r->slot, r->name);
}

static void lists_enter(void)
{
	seen_gen = 0xFFFFFFFFu;
	refresh();
	ui_frame()->focus = 0;
}

const ui_scr_def_t ui_scanlists_screen = {
	.name = "scan-lists", .flags = SF_LIST | SF_FIELD, .prefix = TRN(WORD_SCAN, "Scan"),
	.hint = TRN(SCANLIST_HINT_LISTS, "OK scan  # options  Back"),
	.count = lists_count, .item = lists_item, .select = lists_select, .context = list_options, .enter = lists_enter,
};

// ======================= A list's options (arg: its slot) =======================

enum { LO_EDIT, LO_RENAME, LO_DELETE, LO_COUNT };

static void opt_enter(void)
{
	cur_slot = (uint8_t)ui_frame()->arg;
	if (!ui_svc_scanlist_get(cur_slot, &cur))
		ui_pop();	// gone (deleted elsewhere)
}

static uint16_t opt_count(void) { return LO_COUNT; }

static void opt_item(uint16_t i, ui_item_t *it)
{
	static const tr_id_t names[LO_COUNT] = { TRN(SCANLIST_ROW_EDIT_CHANNELS, "Edit channels"), TRN(ZONE_ROW_RENAME, "Rename"), TRN(MSG_ROW_DELETE, "Delete") };
	str_copy(it->label, sizeof it->label, tr(names[i]));
	item_num_set(it, (uint16_t)(i + 1u));
	if (i == LO_DELETE)
		it->flags |= UI_ITEM_WARN;
}

static void renamed(void)
{
	if (new_name[0] == 0 || strcmp(new_name, cur.name) == 0)
		return;
	str_copy(cur.name, sizeof cur.name, new_name);
	if (!ui_svc_scanlist_put(cur_slot, &cur)) {
		ui_toast(TR(TOAST_BUSY_SAVING, "Busy saving: try again"));
		return;
	}
	toast2(TR(SCANLIST_TOAST_RENAMED, "Renamed "), cur.name);
}

static void opt_select(uint16_t i, bool hold)
{
	(void)hold;
	if (i == LO_EDIT) {
		ui_push(SCR_SLEDIT, cur_slot);
	} else if (i == LO_RENAME) {
		str_copy(new_name, sizeof new_name, cur.name);
		ui_text_open_done(new_name, CODEPLUG_NAME_LEN, renamed);
	} else {
		ui_push(SCR_SLDEL, cur_slot);
	}
}

static const char *opt_title(void) { return cur.name; }

const ui_scr_def_t ui_scanlist_options_screen = {
	.name = "scan-list", .flags = SF_LIST | SF_NUMBERED, .prefix = TRN(SCANLIST_TITLE, "Scan list"), .hint = TRN(HINT_1_3_PICK_BACK, "1-3 pick  Back"),
	.count = opt_count, .item = opt_item, .select = opt_select, .title = opt_title, .enter = opt_enter,
};

// ======================= Delete a list (arg: its slot) =======================

static uint16_t del_count(void) { return 2; }

static void del_item(uint16_t i, ui_item_t *it)
{
	str_copy(it->label, sizeof it->label, i == 0 ? TR(ZONE_ROW_KEEP, "No, keep it") : TR(ZONE_ROW_CONFIRM_DELETE, "Yes, delete"));
	if (i == 1)
		it->flags |= UI_ITEM_WARN;
}

static void del_select(uint16_t i, bool hold)
{
	(void)hold;
	ui_pop();
	if (i == 0)
		return;
	if (!ui_svc_scanlist_delete(cur_slot)) {
		ui_toast(TR(TOAST_BUSY_SAVING, "Busy saving: try again"));
		return;
	}
	ui_pop();	// the options are of nothing now
	toast2(TR(ZONE_TOAST_DELETED, "Deleted "), cur.name);
}

static const char *del_body(void)
{
	return TR(SCANLIST_BODY_DELETE_ASK, "Delete this scan list? Its channels stay in their zones.");
}

const ui_scr_def_t ui_scanlist_delete_screen = {
	.name = "scan-list-delete", .flags = SF_LIST, .prefix = TRN(MSG_ROW_DELETE, "Delete"), .hint = TRN(HINT_OK_CHOOSE_BACK_CANCELS, "OK choose  Back cancel"),
	.count = del_count, .item = del_item, .select = del_select, .title = opt_title, .body = del_body,
};

// ======================= A list's channels (arg: its slot) =======================
// Raw row 0 adds a channel; then the list's channels, OK removes one

static void edit_enter(void)
{
	cur_slot = (uint8_t)ui_frame()->arg;
	if (!ui_svc_scanlist_get(cur_slot, &cur))
		ui_pop();
	ui_frame()->focus = 0;
}

static uint16_t edit_count(void) { return (uint16_t)(1u + cur.count); }

static void edit_item(uint16_t i, ui_item_t *it)
{
	static channel_t ch;
	sbuf_t b;
	if (i == 0) {
		str_copy(it->label, sizeof it->label, TR(ZONE_ROW_ADD_CHANNEL, "Add channel"));
		return;
	}
	bool known = codeplug_channel(cur.channels[i - 1u], &ch);	// a channel deleted from the codeplug since
	sb_init(&b, it->label, sizeof it->label);
	sb_u32(&b, i, 2);
	sb_chr(&b, ' ');
	sb_str(&b, known ? ch.name : "?");
	sb_init(&b, it->value, sizeof it->value);
	if (known) {
		if (!sb_channel_row(&b, cur.channels[i - 1u], &ch))
			it->flags |= UI_ITEM_DIM;	// a trunked channel whose system isn't set up
	} else {
		sb_str(&b, TR(SCANLIST_VALUE_MISSING, "Missing"));
		it->flags |= UI_ITEM_DIM;
	}
}

static void edit_select(uint16_t i, bool hold)
{
	(void)hold;
	if (i == 0) {
		if (cur.count >= SCAN_LIST_MAX) {
			ui_toast(TR(SCANLIST_WHY_FULL, "The list is full"));
			return;
		}
		pick_zone = ui_view()->zone_index;
		ui_push(SCR_SLPICK, cur_slot);
		return;
	}
	uint16_t pos = (uint16_t)(i - 1u);
	scan_list_t next = cur;
	memmove(&next.channels[pos], &next.channels[pos + 1u], (size_t)(next.count - pos - 1u) * sizeof next.channels[0]);
	next.count--;
	if (!ui_svc_scanlist_put(cur_slot, &next)) {
		ui_toast(TR(TOAST_BUSY_SAVING, "Busy saving: try again"));
		return;
	}
	cur = next;
	if (ui_frame()->focus >= edit_count())
		ui_frame()->focus = (uint16_t)(edit_count() - 1u);
	ui_toast(TR(SCANLIST_TOAST_REMOVED, "Removed"));
}

const ui_scr_def_t ui_scanlist_edit_screen = {
	.name = "scan-list-edit", .flags = SF_LIST, .prefix = TRN(TITLE_CHANNELS, "Channels"), .hint = TRN(SCANLIST_HINT_CHANNELS, "OK remove  Back"),
	.count = edit_count, .item = edit_item, .select = edit_select, .title = opt_title, .enter = edit_enter,
};

// ======================= Add channels from any zone =======================
// Raw row 0 is the zone (OK: another zone), then the zone's channels; OK adds one to the list or takes it out again, and
// the screen stays, so a list takes channels from several zones one after the other. Digits filter by name

static int in_list(uint16_t num)
{
	for (uint8_t k = 0; k < cur.count; k++)
		if (cur.channels[k] == num)
			return k;
	return -1;
}

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
		sb_str(&b, TR(WORD_ZONE, "Zone"));
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
	ui_filter_reset();
	ui_frame()->focus = ui_filter_count() > 1 ? 1 : 0;
}

static void pick_item(uint16_t i, ui_item_t *it)
{
	static zone_t z;
	uint16_t raw = ui_filter_raw(i);
	sbuf_t b;
	if (!pick_raw_label(raw, it->label, sizeof it->label))
		return;
	sb_init(&b, it->value, sizeof it->value);
	if (raw == 0) {
		if (codeplug_zone(pick_zone, &z))
			sb_str(&b, z.name);
		return;
	}
	if (codeplug_zone(pick_zone, &z) && in_list(z.channels[raw - 1u]) >= 0) {
		sb_str(&b, TR(SCANLIST_VALUE_IN_LIST, "In list"));
		it->flags |= UI_ITEM_TEMP;
	}
}

static void pick_select(uint16_t i, bool hold)
{
	static zone_t z;
	(void)hold;
	uint16_t raw = ui_filter_raw(i);
	if (raw == 0) {
		ui_push(SCR_SLZONE, 0);
		return;
	}
	if (!codeplug_zone(pick_zone, &z) || raw - 1u >= z.count)
		return;
	uint16_t num = z.channels[raw - 1u];
	scan_list_t next = cur;
	int at = in_list(num);
	if (at >= 0) {
		memmove(&next.channels[at], &next.channels[at + 1], (size_t)(next.count - (uint8_t)at - 1u) * sizeof next.channels[0]);
		next.count--;
	} else if (next.count >= SCAN_LIST_MAX) {
		ui_toast(TR(SCANLIST_WHY_FULL, "The list is full"));
		return;
	} else {
		next.channels[next.count++] = num;
	}
	if (!ui_svc_scanlist_put(cur_slot, &next)) {
		ui_toast(TR(TOAST_BUSY_SAVING, "Busy saving: try again"));
		return;
	}
	cur = next;
}

static const char *pick_title(void) { return cur.name; }

const ui_scr_def_t ui_scanlist_pick_screen = {
	.name = "scan-list-add", .flags = SF_LIST | SF_FILTER, .prefix = TRN(SCANLIST_TITLE_ADD_TO, "Add to"),
	.hint = TRN(ZONE_HINT_ADD_CHANNEL, TEXT_ALT_OPEN "OK add  0-9 find  Back" TEXT_ALT_NEXT "OK add  Back" TEXT_ALT_CLOSE),
	.count = ui_filter_count, .item = pick_item, .select = pick_select, .title = pick_title, .enter = pick_enter,
	.filter = &(const ui_filter_def_t){ .raw_count = pick_raw_count, .raw_label = pick_raw_label },
};

// ======================= The zone for the picker =======================

static uint16_t zone_count(void) { return codeplug_zone_count(); }

static void zone_item(uint16_t i, ui_item_t *it)
{
	static zone_t z;
	if (!codeplug_zone(i, &z))
		return;
	str_copy(it->label, sizeof it->label, z.name);
	if (i == pick_zone)
		it->flags |= UI_ITEM_TEMP;
}

static void zone_enter(void)
{
	ui_frame()->focus = pick_zone;
}

static void zone_select(uint16_t i, bool hold)
{
	(void)hold;
	pick_zone = i;
	ui_pop();	// back to the picker, now on that zone
	ui_filter_reset();
	ui_frame()->focus = ui_filter_count() > 1 ? 1 : 0;
	ui_frame()->top = 0;
}

const ui_scr_def_t ui_scanlist_zone_screen = {
	.name = "scan-list-zone", .flags = SF_LIST | SF_FIELD, .prefix = TRN(WORD_ZONE, "Zone"), .hint = TRN(HINT_OK_PICK_BACK, "OK pick  Back"),
	.count = zone_count, .item = zone_item, .select = zone_select, .enter = zone_enter,
};

// Start the list in slot: the zone scan's place on the Home screen, with the name in the toast
void ui_scan_list(uint8_t slot, const char *name)
{
#if CONFIG_DUAL_WATCH
	if (ui_st.dual.on)
		ui_dual_stop();
#endif
	if (!ui_svc_scanlist_start(slot)) {
		ui_toast(TR(WHY_SCAN_NOT_IN_BUILD, "Scan: not in this build"));
		return;
	}
	ui_st.scanning = true;
	ui_go_home();
	toast2(TR(SCANLIST_TOAST_SCANNING, "Scanning "), name);
}
#endif
