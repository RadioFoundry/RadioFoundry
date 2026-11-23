// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Wires-X (docs/spec/ysf.md 9): the node this YSF channel hears, the room it is in, connect and disconnect,
// the room list a page at a time, a search. Every command is a short transmission the radio core keys (refused as
// PTT would be); the node's reply comes in its own time, so the screens show what was asked until it answers.
// Opened from Tools > Wires-X, quick settings tile 3 on a YSF channel, or a key set to Wires-X
#include "config.h"
#if CONFIG_YSF	// the whole file
#include <string.h>
#include "ui_nav.h"
#include "fmt.h"
#include "i18n.h"

#define WX_WAIT_MS	8000u	// no reply this long after a command: "No answer"
#define WX_ID_DIGITS	5
#define WEAK		__attribute__((weak))

WEAK bool ui_svc_ysf_wx(uint8_t cmd, uint32_t arg, const char *text) { (void)cmd; (void)arg; (void)text; return false; }
WEAK void ui_svc_ysf_wx_state(radio_ysf_wx_t *out) { memset(out, 0, sizeof *out); out->asked_cmd = YSF_WX_NONE; }
WEAK bool ui_svc_ysf_wx_room(uint8_t i, ysf_wx_room_t *out) { (void)i; (void)out; return false; }

static radio_ysf_wx_t wx;		// the radio's state, read at each draw
static uint32_t asked_ms;		// when we last sent a command, 0 none
static uint32_t asked_serial;		// the reply serial then
static uint32_t room_id;		// typed (0 none), kept while the radio runs
static uint8_t id_len;			// digits typed in the current entry
static bool id_editing;
#if CONFIG_CHANNEL_EDIT
static char search_text[YSF_WX_TEXT_LEN + 1];
#endif

static void wx_read(void)
{
	ui_svc_ysf_wx_state(&wx);
}

// Waiting for the node's answer to what we last asked, and whether it has given up
static bool waiting(void)
{
	return asked_ms != 0 && wx.serial == asked_serial && ui_st.now_ms - asked_ms < WX_WAIT_MS;
}

static bool no_answer(void)
{
	return asked_ms != 0 && wx.serial == asked_serial && ui_st.now_ms - asked_ms >= WX_WAIT_MS;
}

// Send cmd; a toast says what was sent, or why not (not on a YSF channel). The radio core may still refuse it as it
// refuses PTT: Home's refusal line and the TX deny beep say so
static bool send(uint8_t cmd, uint32_t arg, const char *text)
{
	wx_read();
	if (ui_view()->ch.mode != RADIO_MODE_YSF || !ui_svc_ysf_wx(cmd, arg, text)) {
		ui_toast(TR(WX_TOAST_YSF_ONLY, "Wires-X: on a YSF channel"));
		return false;
	}
	asked_ms = ui_st.now_ms != 0 ? ui_st.now_ms : 1;
	asked_serial = wx.serial;
	ui_dirty();
	return true;
}

// ── The Wires-X page ──
enum { WX_NODE, WX_ROOM, WX_ROOM_ID, WX_CONNECT, WX_DISCONNECT, WX_ROOMS,
#if CONFIG_CHANNEL_EDIT
	WX_SEARCH,
#endif
	WX_COUNT };

static uint16_t wx_count(void)
{
	wx_read();
	return WX_COUNT;
}

static void wx_item(uint16_t i, ui_item_t *it)
{
	static const tr_id_t labels[WX_COUNT] = { TRN(WX_LABEL_NODE, "Node"), TRN(WX_LABEL_ROOM, "Room"),
		TRN(WX_LABEL_ROOM_ID, "Room ID"), TRN(WX_ROW_CONNECT, "Connect"), TRN(WX_ROW_DISCONNECT, "Disconnect"),
		TRN(WX_ROW_ROOMS, "Rooms"),
#if CONFIG_CHANNEL_EDIT
		TRN(WX_ROW_SEARCH, "Search"),
#endif
	};
	sbuf_t b;

	str_copy(it->label, sizeof it->label, tr(labels[i]));
	sb_init(&b, it->value, sizeof it->value);
	switch (i) {
	case WX_NODE:
		if (waiting() && (wx.asked_cmd == YSF_WX_DX || wx.asked_cmd == YSF_WX_CONNECT ||
				  wx.asked_cmd == YSF_WX_DISCONNECT))
			sb_str(&b, TR(WX_VALUE_WAITING, "Waiting..."));
		else if (no_answer())
			sb_str(&b, TR(WX_VALUE_NO_ANSWER, "No answer"));
		else if (wx.serial == 0)
			sb_str(&b, "-");	// not asked yet
		else
			sb_str(&b, wx.node_name[0] ? wx.node_name : wx.node_call);
		break;
	case WX_ROOM:
		if (wx.serial == 0)
			sb_str(&b, "-");
		else if (!wx.connected)
			sb_str(&b, TR(VALUE_NONE, "None"));
		else
			sb_str(&b, wx.room_name[0] ? wx.room_name : wx.room_id);
		break;
	case WX_ROOM_ID:
		if (id_editing) {
			if (id_len)
				sb_u32(&b, room_id, id_len);
			sb_chr(&b, '_');
			it->flags |= UI_ITEM_EDIT;
		} else if (room_id) {
			sb_u32(&b, room_id, WX_ID_DIGITS);
		} else {
			sb_str(&b, TR(VALUE_NOT_SET, "Not set"));
		}
		break;
	case WX_CONNECT:
		if (room_id)
			sb_u32(&b, room_id, WX_ID_DIGITS);
		break;
	case WX_ROOMS:
		if (wx.rooms_total && !wx.search)
			sb_u32(&b, wx.rooms_total, 1);
		break;
	default:
		break;
	}
}

#if CONFIG_CHANNEL_EDIT
static void search_kept(void)
{
	if (search_text[0] != '\0' && send(YSF_WX_SEARCH, 0, search_text))
		ui_push(SCR_WIRESX_ROOMS, 0);
}
#endif

static void wx_select(uint16_t i, bool hold)
{
	(void)hold;
	if (id_editing && i != WX_ROOM_ID)
		id_editing = false;
	switch (i) {
	case WX_NODE:
	case WX_ROOM:
		send(YSF_WX_DX, 0, NULL);
		break;
	case WX_ROOM_ID:
		id_editing = !id_editing;
		if (id_editing) {
			room_id = 0;
			id_len = 0;
		}
		ui_dirty();
		break;
	case WX_CONNECT:
		if (!room_id)
			ui_toast(TR(WX_TOAST_ROOM_ID_FIRST, "Type a room ID first"));
		else
			send(YSF_WX_CONNECT, room_id, NULL);
		break;
	case WX_DISCONNECT:
		send(YSF_WX_DISCONNECT, 0, NULL);
		break;
	case WX_ROOMS:
		if (send(YSF_WX_ALL, 1, NULL))
			ui_push(SCR_WIRESX_ROOMS, 0);
		break;
#if CONFIG_CHANNEL_EDIT
	case WX_SEARCH:
		search_text[0] = '\0';
		ui_menus_text(search_text, YSF_WX_TEXT_LEN, search_kept);
		break;
#endif
	default:
		break;
	}
}

// Digits type the room ID wherever the list is (as My radio's DMR ID): five at most, BACK takes one back
static bool wx_key(const input_event_t *ev)
{
	bool press = ev->gesture == GESTURE_PRESS || ev->gesture == GESTURE_LONG;

	if (ev->action >= ACT_DIGIT_0 && ev->action <= ACT_DIGIT_9 && press) {
		if (!id_editing || id_len >= WX_ID_DIGITS) {
			id_editing = true;
			room_id = 0;
			id_len = 0;
		}
		room_id = room_id * 10u + (uint32_t)(ev->action - ACT_DIGIT_0);
		id_len++;
		ui_frame()->focus = WX_ROOM_ID;
		ui_dirty();
		return true;
	}
	if (id_editing && ev->action == ACT_BACK && ev->gesture == GESTURE_PRESS) {
		if (id_len) {
			id_len--;
			room_id /= 10u;
		} else {
			id_editing = false;
		}
		ui_dirty();
		return true;
	}
	return false;
}

static const char *wx_body(void)
{
	static char body[48];
	sbuf_t b;

	if (!wx.serial || !wx.node_call[0])
		return NULL;	// the node's callsign once it answered, else no line
	sb_init(&b, body, sizeof body);
	sb_str(&b, TR(WX_BODY_NODE, "Node: "));
	sb_str(&b, wx.node_call);
	return body;
}

static void wx_enter(void)
{
	id_editing = false;
	wx_read();
}

const ui_scr_def_t ui_wiresx_screen = {
	.name = "wiresx", .flags = SF_LIST | SF_FIELD, .prefix = TRX(WX_TITLE, "Wires-X"),
	.hint = TRN(HINT_TYPE_OK_SEND_BACK, "0-9 room  OK send  Back"),
	.count = wx_count, .item = wx_item, .select = wx_select, .key = wx_key, .enter = wx_enter, .body = wx_body,
};

// ── The room list: a page of the node's list or a search's result; OK connects; the last row asks for the next page ──
static bool more(void)
{
	return !wx.search && wx.rooms && wx.rooms_first + wx.rooms - 1u < wx.rooms_total;
}

static uint16_t rooms_count(void)
{
	wx_read();
	if (waiting() && (wx.asked_cmd == YSF_WX_ALL || wx.asked_cmd == YSF_WX_SEARCH))
		return 0;	// the body says so
	return (uint16_t)(wx.rooms + (more() ? 1 : 0));
}

static void rooms_item(uint16_t i, ui_item_t *it)
{
	ysf_wx_room_t r;
	sbuf_t b;

	if (i >= wx.rooms) {
		str_copy(it->label, sizeof it->label, TR(WX_ROW_NEXT_PAGE, "Next page"));
		return;
	}
	if (!ui_svc_ysf_wx_room((uint8_t)i, &r))
		return;
	sb_init(&b, it->label, sizeof it->label);
	sb_str(&b, r.name[0] ? r.name : r.id);
	sb_init(&b, it->value, sizeof it->value);
	sb_str(&b, r.id);
	sb_chr(&b, ' ');
	sb_u32(&b, r.users, 1);
}

static void rooms_select(uint16_t i, bool hold)
{
	ysf_wx_room_t r;

	(void)hold;
	if (i >= wx.rooms) {
		send(YSF_WX_ALL, (uint32_t)wx.rooms_first + wx.rooms, NULL);
		return;
	}
	if (!ui_svc_ysf_wx_room((uint8_t)i, &r))
		return;
	room_id = 0;
	for (const char *p = r.id; *p >= '0' && *p <= '9'; p++)
		room_id = room_id * 10u + (uint32_t)(*p - '0');
	if (room_id && send(YSF_WX_CONNECT, room_id, NULL))
		ui_pop();	// back on the page: Node and Room show the answer
}

static const char *rooms_body(void)
{
	static char body[40];
	sbuf_t b;

	sb_init(&b, body, sizeof body);
	if (waiting())
		sb_str(&b, TR(WX_BODY_WAITING, "Waiting for the node..."));
	else if (no_answer())
		sb_str(&b, TR(WX_VALUE_NO_ANSWER, "No answer"));
	else if (!wx.rooms)
		sb_str(&b, wx.search ? TR(WX_BODY_NOTHING_FOUND, "Nothing found") : TR(WX_BODY_NO_LIST, "No list yet"));
	else if (!wx.search) {
		sb_u32(&b, wx.rooms_first, 1);
		sb_chr(&b, '-');
		sb_u32(&b, (uint32_t)wx.rooms_first + wx.rooms - 1u, 1);
		sb_str(&b, TR(WX_BODY_OF, " of "));
		sb_u32(&b, wx.rooms_total, 1);
	}
	return body;
}

const ui_scr_def_t ui_wiresx_rooms_screen = {
	.name = "wiresx-rooms", .flags = SF_LIST | SF_FIELD, .prefix = TRN(WX_TITLE_ROOMS, "Rooms"),
	.hint = TRN(HINT_OK_CONNECT_BACK, "OK connect  Back"),
	.count = rooms_count, .item = rooms_item, .select = rooms_select, .body = rooms_body,
};

// Tools > Wires-X, quick tile 3 on YSF, a key set to Wires-X
void ui_wiresx_open(void)
{
	if (ui_view()->ch.mode != RADIO_MODE_YSF) {
		ui_toast(TR(WX_TOAST_YSF_ONLY, "Wires-X: on a YSF channel"));
		return;
	}
	ui_push(SCR_WIRESX, 0);
}
#endif	// CONFIG_YSF
