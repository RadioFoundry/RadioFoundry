// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// room commands and replies (ysf_wx.h, docs/spec/ysf.md 9)
#include "ysf/ysf_wx.h"

#include <stddef.h>
#include <string.h>

// ── Commands (9.2): a sequence byte, three bytes of command, 0x27, the parameters in ASCII, space padded as a
// commercial portable sends them, 0x03 and the byte sum of everything before the sum ──
#define WX_SEP		0x27		// a node's replies have 0x26 there
#define WX_END		0x03

static const uint8_t code[4][3] = {
	{ 0x5D, 0x71, 0x5F },	// DX: the node's status
	{ 0x5D, 0x23, 0x5F },	// CONNECT
	{ 0x5D, 0x2A, 0x5F },	// DISCONNECT
	{ 0x5D, 0x66, 0x5F },	// ALL: the room list (SEARCH too, with other parameters)
};

static void digits(uint8_t *out, uint32_t v, uint8_t n)
{
	while (n--) {
		out[n] = (uint8_t)('0' + v % 10u);
		v /= 10u;
	}
}

static uint8_t sum8(const uint8_t *d, uint16_t n)
{
	uint8_t s = 0;

	while (n--)
		s = (uint8_t)(s + *d++);
	return s;
}

uint8_t ysf_wx_command(uint8_t cmd, uint32_t arg, const char *text, uint8_t out[YSF_WX_CMD_MAX])
{
	uint8_t n = 0;

	if (cmd > YSF_WX_SEARCH)
		return 0;
	out[n++] = 0x00;
	memcpy(out + n, code[cmd == YSF_WX_SEARCH ? YSF_WX_ALL : cmd], 3);
	n += 3;
	out[n++] = WX_SEP;
	switch (cmd) {
	case YSF_WX_DX:
	case YSF_WX_DISCONNECT:
		memset(out + n, ' ', 10);		// ten spaces
		n += 10;
		break;
	case YSF_WX_CONNECT:
		digits(out + n, arg % 100000u, 5);	// the room ID in a field of ten
		n += 5;
		memset(out + n, ' ', 5);
		n += 5;
		break;
	case YSF_WX_ALL:
		out[n++] = '0';				// "01": the list; the first entry in three digits
		out[n++] = '1';
		digits(out + n, arg >= 1 && arg <= 999 ? arg : 1, 3);
		n += 3;
		break;
	case YSF_WX_SEARCH:
		out[n++] = '1';				// "11": a search; the first entry in three digits (spaces for the
		out[n++] = '1';				// first page, as a portable sends it); the text in 16, upper case
		if (arg > 1 && arg <= 999)
			digits(out + n, arg, 3);
		else
			memset(out + n, ' ', 3);
		n += 3;
		for (uint8_t i = 0; i < YSF_WX_TEXT_LEN; i++) {
			char c = text && *text ? *text++ : ' ';

			out[n++] = (uint8_t)(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
		}
		break;
	default:
		break;
	}
	out[n++] = WX_END;
	out[n] = sum8(out, n);
	return (uint8_t)(n + 1);
}

// ── Replies (9.3), parsed as their bytes come ──
#define ROOMS_AT	29		// a list's first room entry
#define ROOM_LEN	50

static void text_of(const uint8_t *src, uint8_t n, char *out)
{
	int8_t k = (int8_t)n;

	for (uint8_t i = 0; i < n; i++)
		out[i] = (src[i] >= 0x20 && src[i] < 0x7F) ? (char)src[i] : ' ';
	while (k > 0 && out[k - 1] == ' ')
		k--;
	out[k] = '\0';
}

static unsigned num_of(const uint8_t *src, uint8_t n)
{
	unsigned v = 0;

	while (n--) {
		if (*src >= '0' && *src <= '9')
			v = v * 10u + (unsigned)(*src - '0');
		src++;
	}
	return v;
}

// out keeps the last reply taken: the room list stays for the screens while other replies come and go (a reply
// that isn't a list never writes its room entries)
void ysf_wx_rx_begin(ysf_wx_rx_t *r)
{
	memset(r, 0, offsetof(ysf_wx_rx_t, out));
	r->on = true;
}

static bool is_list(const uint8_t *h)
{
	return h[1] == 0x5D && h[2] == 0x46 && h[3] == 0x5F;
}

static void take(ysf_wx_rx_t *r, uint8_t b)
{
	uint16_t p = r->pos;

	if (p < sizeof r->hdr)
		r->hdr[p] = b;
	if (p >= ROOMS_AT && is_list(r->hdr)) {
		unsigned j = (unsigned)(p - ROOMS_AT) / ROOM_LEN, i = (unsigned)(p - ROOMS_AT) % ROOM_LEN;

		r->rec[i] = b;
		if (i == ROOM_LEN - 1 && j < YSF_WX_ROOMS) {
			// An entry: '5', its ID, its name in 16, its users in 3, a description, CR
			ysf_wx_room_t *room = &r->out.room[j];
			unsigned u = num_of(r->rec + 22, 3);

			text_of(r->rec + 1, 5, room->id);
			text_of(r->rec + 6, 16, room->name);
			room->users = (uint8_t)(u > 255 ? 255 : u);
		}
	}
	// The end marker: a 0x03 followed by the sum of every byte before that sum (the last such pair counts: the
	// sum may itself be 0x03, and the frames are padded past the end)
	if (p >= 1 && r->prev == WX_END && b == r->sum)
		r->end = p;		// the 0x03 is at p - 1
	r->sum = (uint8_t)(r->sum + b);
	r->prev = b;
	r->pos = (uint16_t)(p + 1);
}

bool ysf_wx_rx_frame(ysf_wx_rx_t *r, uint8_t fn, uint8_t bn, uint8_t ft, const uint8_t c1[20], const uint8_t c2[20],
		     uint8_t ok)
{
	uint16_t at;

	if (!r->on || fn == 0)
		return false;		// FN 0 carries the callsigns again
	// Blocks counted as BN moves on: a reply of five blocks or more wraps it to 0
	if (bn != r->bn) {
		r->blk = (uint8_t)(r->blk + ((bn - r->bn) & 3u));
		r->bn = bn;
	}
	// Block 0: FN 1 holds data bytes 0-19 behind the remarks, FN 2 on 40 each (260 in all); each later block starts
	// its FN 1 with a 0x00 and 19 bytes, then 40 a frame (259)
	at = r->blk == 0 ? 0 : (uint16_t)(260u + (r->blk - 1u) * 259u);
	if (fn == 1) {
		if (!(ok & 2) || r->pos != at) {
			r->broken = true;
		} else {
			for (uint8_t i = r->blk ? 1 : 0; i < 20; i++)
				take(r, c2[i]);
		}
	} else {
		at = (uint16_t)(at + (r->blk ? 19u : 20u) + (fn - 2u) * 40u);
		if ((ok & 3) != 3 || r->pos != at) {
			r->broken = true;
		} else {
			for (uint8_t i = 0; i < 20; i++)
				take(r, c1[i]);
			for (uint8_t i = 0; i < 20; i++)
				take(r, c2[i]);
		}
	}
	return fn == ft && ft < 7;
}

bool ysf_wx_rx_end(ysf_wx_rx_t *r)
{
	const uint8_t *h = r->hdr;
	ysf_wx_t *w = &r->out;
	unsigned end, o;

	if (!r->on)
		return false;
	r->on = false;
	if (r->broken || !r->end)
		return false;
	end = r->end - 1u;		// the 0x03
	if (is_list(h) && end >= ROOMS_AT) {
		// The list and a search's result alike: [7..11] the node's ID, [12..21] its callsign, [22] '1' for a
		// search's result, [23..24] rooms on the page, [25..27] in all. A node may fill the page up to 20 with
		// blank entries (a search with one match came as 20 rows): the first entry without an ID ends the page
		unsigned n, k = 0;
		char call[sizeof w->node_call];

		// A list names its node by callsign only: another node's name and room are not this one's
		text_of(h + 12, 10, call);
		if (strcmp(call, w->node_call) != 0) {
			w->node_name[0] = w->room_id[0] = w->room_name[0] = '\0';
			w->connected = false;
		}
		w->search = h[5] == '0';
		n = num_of(h + 23, 2);
		while (k < n && k < YSF_WX_ROOMS && ROOMS_AT + (k + 1u) * ROOM_LEN <= end && w->room[k].id[0] != '\0')
			k++;
		w->rooms = (uint8_t)k;
		w->rooms_total = (uint16_t)num_of(h + 25, 3);
		w->kind = YSF_WX_ALL;
		text_of(h + 7, 5, w->node_id);
		text_of(h + 12, 10, w->node_call);
		return true;
	}
	if (h[1] != 0x5D || h[3] != 0x5F)
		return false;
	// Status, connect and disconnect, in two layouts: a commercial repeater's node answers 0x52 (status) and 0x42
	// (connect, disconnect) with 18 bytes before the node's ID; 0x51 and 0x41 have the fields from [5]. Then the
	// node's ID (5), its callsign (10), its name (14), its link state, the room's ID (5), its name (16). The state is
	// "12" with no room; a commercial node says "15" in a room, a hotspot gateway "13" linked to a reflector and
	// "10" when a connect failed (no such room; it is then out of any room)
	switch (h[2]) {
	case 0x51: case 0x41: o = 5; break;
	case 0x52: case 0x42: o = 23; break;
	default: return false;
	}
	if (end < o + 52u)
		return false;
	w->connected = h[o + 29] == '1' && (h[o + 30] == '5' || h[o + 30] == '3');
	w->kind = (h[2] & 0xF0) == 0x50 ? YSF_WX_DX : w->connected ? YSF_WX_CONNECT : YSF_WX_DISCONNECT;
	text_of(h + o, 5, w->node_id);
	text_of(h + o + 5, 10, w->node_call);
	text_of(h + o + 15, 14, w->node_name);
	if (w->connected) {
		text_of(h + o + 31, 5, w->room_id);
		text_of(h + o + 36, 16, w->room_name);
	} else {
		w->room_id[0] = w->room_name[0] = '\0';
	}
	return true;
}
