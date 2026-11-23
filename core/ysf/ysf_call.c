// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A YSF DN voice call (ysf.h, docs/spec/ysf.md 4): on receive, the frames of ysf_rx.c become a call with
// its fields and its voice frames in order; on transmit, the frames are built and released symbol by symbol.
#include "ysf.h"

#include <string.h>

// ── Fields ──
#define CM_RADIO_ID	1	// call mode a radio sends with a destination of "*****" and its radio ID (ysf.md 4.2)
#define CS_RADIO	2	// callsign information every radio heard sends

static void field_put(uint8_t out[YSF_CS_LEN], const char *s)
{
	uint8_t i = 0;

	for (; i < YSF_CS_LEN && s[i]; i++)
		out[i] = (uint8_t)((s[i] >= 'a' && s[i] <= 'z') ? s[i] - 'a' + 'A' : s[i]);
	for (; i < YSF_CS_LEN; i++)
		out[i] = ' ';
}

// A received field as text: printable characters only, trailing spaces dropped
static bool field_get(char out[YSF_CS_LEN + 1], const uint8_t in[YSF_CS_LEN])
{
	char t[YSF_CS_LEN + 1];
	int8_t n = YSF_CS_LEN;

	for (uint8_t i = 0; i < YSF_CS_LEN; i++)
		t[i] = (in[i] >= 0x20 && in[i] < 0x7F) ? (char)in[i] : ' ';
	while (n > 0 && t[n - 1] == ' ')
		n--;
	t[n] = '\0';
	if (!strcmp(t, out))
		return false;
	memcpy(out, t, (size_t)n + 1);
	return true;
}

static bool id_char(uint8_t c)
{
	return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z');
}

// A radio ID in the last five characters of a field: false when they aren't one, or it is the one we have
static bool id_get(char out[YSF_ID_LEN + 1], const uint8_t in[YSF_CS_LEN])
{
	for (uint8_t i = 0; i < YSF_ID_LEN; i++)
		if (!id_char(in[5 + i]))
			return false;
	if (!memcmp(out, in + 5, YSF_ID_LEN) && out[YSF_ID_LEN] == '\0')
		return false;
	memcpy(out, in + 5, YSF_ID_LEN);
	out[YSF_ID_LEN] = '\0';
	return true;
}

static void dst_field(const ysf_tx_id_t *id, uint8_t out[YSF_CS_LEN])
{
	memset(out, '*', 5);
	for (uint8_t i = 0; i < YSF_ID_LEN; i++)
		out[5 + i] = id_char((uint8_t)id->radio_id[i]) ? (uint8_t)id->radio_id[i] : '0';
}

void ysf_dch_content(const ysf_tx_id_t *id, uint8_t fn, uint8_t out[YSF_DCH_BYTES])
{
	// FN 6: what one maker's radio sends there (recorded on air; its meaning is unknown, ysf.md 4.2)
	static const uint8_t fn6[YSF_DCH_BYTES] = { 0x01, 0x22, 0x61, 0x5F, 0x2B, 0x03, 0x11, 0x00, 0x00, 0x00 };

	memset(out, ' ', YSF_DCH_BYTES);
	switch (fn) {
	case 0:
		dst_field(id, out);
		break;
	case 1:
		field_put(out, id->callsign);
		break;
	case 5:
		dst_field(id, out);
		memset(out, ' ', 5);
		break;
	case 6:
		memcpy(out, fn6, sizeof fn6);
		break;
	default:
		break;		// downlink, uplink, remarks 1-2: spaces
	}
}

void ysf_head_csd(const ysf_tx_id_t *id, uint8_t csd1[YSF_CSD_BYTES], uint8_t csd2[YSF_CSD_BYTES])
{
	dst_field(id, csd1);
	field_put(csd1 + YSF_CS_LEN, id->callsign);
	memset(csd2, ' ', YSF_CSD_BYTES);
}

// ── Receive ──
static void call_start(ysf_call_rx_t *c, const ysf_rx_frame_t *f, bool late)
{
	memset(&c->call, 0, sizeof c->call);
	c->call.late = late;
	c->call.dgid = f->fich.dgid;
	c->call.dt = f->fich.dt;
	c->call.mr = f->fich.mr;
	c->call.rssi_dbm = f->rssi_dbm;
	c->active = true;
	c->stats.calls++;
	if (c->sink->start)
		c->sink->start(&c->call, c->ctx);
}

static void call_end(ysf_call_rx_t *c)
{
	if (!c->active)
		return;
	c->active = false;
	if (c->sink->end)
		c->sink->end(c->ctx);
}

static void changed(ysf_call_rx_t *c)
{
	if (c->sink->update)
		c->sink->update(&c->call, c->ctx);
}

// Destination and source, from the header's CSD1 or DCH FN 0 / 1. The destination of "*****" and a radio ID
// carries the caller's radio ID
static bool take_dst(ysf_call_rx_t *c, const uint8_t f[YSF_CS_LEN])
{
	bool ch = field_get(c->call.dst, f);

	if (!memcmp(f, "*****", 5))
		ch |= id_get(c->call.src_id, f);
	return ch;
}

static void on_dch(ysf_call_rx_t *c, uint8_t fn, const uint8_t d[YSF_DCH_BYTES])
{
	bool ch = false;

	switch (fn) {
	case 0:
		ch = take_dst(c, d);
		break;
	case 1:
		ch = field_get(c->call.src, d);
		break;
	case 5:
		ch = id_get(c->call.src_id, d);	// remarks 3-4: the radio ID last
		break;
	default:
		break;
	}
	if (ch)
		changed(c);
}

static void voice(ysf_call_rx_t *c, const ysf_rx_frame_t *f)
{
	for (uint8_t n = 0; n < YSF_VCH_PER_FRAME; n++) {
		uint8_t bits[AMBE_PARAM_BITS], v[YSF_VOICE_BYTES];

		if (c->call.dt != YSF_DT_VD2) {
			c->stats.voice_held++;
			continue;
		}
		c->stats.vch_fixed += (uint32_t)ysf_vch_get(f->hard, f->soft, n, bits);
		ambeFecEncode(bits, v);
		c->stats.voice++;
		if (c->sink->voice)
			c->sink->voice(v, c->ctx);
	}
}

static void on_data(ysf_call_rx_t *c, const ysf_rx_frame_t *f)
{
	uint8_t c1[YSF_CSD_BYTES], c2[YSF_CSD_BYTES], ok = 0;
	bool whole = false;

	c->stats.data_frames++;
	if (c->active && (c->call.dt != YSF_DT_DATA || f->fich.fi == YSF_FI_HEADER))
		call_end(c);		// a voice call's terminator was lost, or a new transmission's header came
	// A data transmission is a call without voice: the header's CSD1 names its source and destination as a voice
	// call's does, so it shows (and goes into the list of calls heard) like one
	if (!c->active && f->fich.fi != YSF_FI_TERM) {
		call_start(c, f, f->fich.fi != YSF_FI_HEADER);
		if (f->fich.fi == YSF_FI_HEADER) {
			int errs = ysf_csd_get(f->hard, f->soft, 0, &c->rx.work, c1);

			if (errs < 0) {
				c->stats.csd_fail++;
			} else {
				c->stats.fec_bits += (uint32_t)errs;
				bool dst = take_dst(c, c1), src = field_get(c->call.src, c1 + YSF_CS_LEN);

				if (dst || src)
					changed(c);
			}
		}
	}
	switch (f->fich.fi) {
	case YSF_FI_HEADER:
		ysf_wx_rx_begin(&c->wx);
		return;
	case YSF_FI_COMMS:
		if (!c->wx.on)
			return;		// entered after the header: its first bytes are lost
		ok = (uint8_t)((ysf_csd_get(f->hard, f->soft, 0, &c->rx.work, c1) >= 0) |
			       (ysf_csd_get(f->hard, f->soft, 1, &c->rx.work, c2) >= 0) << 1);
		whole = ysf_wx_rx_frame(&c->wx, f->fich.fn, f->fich.bn, f->fich.ft, c1, c2, ok);
		if (!whole)
			return;
		break;
	case YSF_FI_TERM:
		break;
	default:
		return;
	}
	if (ysf_wx_rx_end(&c->wx)) {
		c->stats.wx_replies++;
		if (c->sink->wx)
			c->sink->wx(&c->wx.out, c->ctx);
	}
	if (f->fich.fi == YSF_FI_TERM)
		call_end(c);
}

static void on_frame(const ysf_rx_frame_t *f, void *ctx)
{
	ysf_call_rx_t *c = ctx;
	ysf_fec_work_t *w = &c->rx.work;
	uint8_t buf[YSF_CSD_BYTES];
	int errs;

	c->stats.frames++;
	if (f->rssi_dbm)
		c->call.rssi_dbm = f->rssi_dbm;
	if (f->fich_errs < 0) {
		// A frame placed by the call's order: a communications frame, its FN the next (in a data transmission the
		// reply lost a frame)
		c->stats.fich_fail++;
		c->wx.broken = true;
		if (!c->active)
			return;
		errs = ysf_dch_get(f->hard, f->soft, w, buf);
		if (errs >= 0 && c->call.dt == YSF_DT_VD2)
			on_dch(c, c->next_fn, buf);
		c->next_fn = (uint8_t)(c->next_fn >= YSF_FT_DN ? 0 : c->next_fn + 1);
		voice(c, f);
		return;
	}
	c->stats.fec_bits += (uint32_t)f->fich_errs;
	// A data transmission (room data, ysf.md 9.3): no call, its frames to the reply being parsed
	if (f->fich.dt == YSF_DT_DATA) {
		on_data(c, f);
		return;
	}
	switch (f->fich.fi) {
	case YSF_FI_HEADER:
		// A header inside a call: the old one's terminator was lost
		if (c->active)
			call_end(c);
		call_start(c, f, false);
		errs = ysf_csd_get(f->hard, f->soft, 0, w, buf);
		if (errs < 0) {
			c->stats.csd_fail++;
			break;
		}
		c->stats.fec_bits += (uint32_t)errs;
		bool dst = take_dst(c, buf), src = field_get(c->call.src, buf + YSF_CS_LEN);

		if (dst || src)
			changed(c);
		c->next_fn = 0;
		break;
	case YSF_FI_COMMS:
		if (!c->active)
			call_start(c, f, true);
		if (f->fich.dgid != c->call.dgid || f->fich.dt != c->call.dt || f->fich.mr != c->call.mr) {
			c->call.dgid = f->fich.dgid;
			c->call.dt = f->fich.dt;
			c->call.mr = f->fich.mr;
			changed(c);
		}
		c->next_fn = (uint8_t)(f->fich.fn >= f->fich.ft ? 0 : f->fich.fn + 1);
		if (f->fich.dt == YSF_DT_VD2) {
			errs = ysf_dch_get(f->hard, f->soft, w, buf);
			if (errs < 0) {
				c->stats.dch_fail++;
			} else {
				c->stats.fec_bits += (uint32_t)errs;
				on_dch(c, f->fich.fn, buf);
			}
		}
		voice(c, f);
		break;
	case YSF_FI_TERM:
		call_end(c);
		break;
	default:
		break;
	}
}

static void on_lost(void *ctx)
{
	ysf_call_rx_t *c = ctx;

	c->wx.on = false;
	call_end(c);
}

void ysf_call_rx_init(ysf_call_rx_t *c, const ysf_call_sink_t *sink, void *ctx)
{
	memset(c, 0, sizeof *c);
	c->sink = sink;
	c->ctx = ctx;
	// (on_frame's context is c; the framer's is c->rx, the same address)
	ysf_rx_init(&c->rx, on_frame, on_lost, c);
}

// ── Transmit ──
enum { TX_PRE, TX_HEAD, TX_COMMS, TX_TERM, TX_DONE };

static void fich_for(const ysf_call_tx_t *t, uint8_t fi, ysf_fich_t *f)
{
	memset(f, 0, sizeof *f);
	f->fi = fi;
	f->cs = CS_RADIO;
	f->cm = CM_RADIO_ID;
	f->fn = fi == YSF_FI_COMMS ? t->fn : 0;
	f->ft = YSF_FT_DN;
	f->mr = t->id.mr;
	f->dt = t->data ? YSF_DT_DATA : YSF_DT_VD2;
	f->dgid = t->id.dgid;
	if (t->data)
		f->ft = t->data_ft;
}

// A data frame's two 20-byte channels: header, FN 0 and terminator carry the callsigns as a DN header does; FN 1 the
// remarks (our radio ID last) and data bytes 0-19; each later FN 40 bytes, space padded (ysf.md 9.1)
static void data_csd(const ysf_call_tx_t *t, uint8_t fi, uint8_t fn, uint8_t c1[YSF_CSD_BYTES], uint8_t c2[YSF_CSD_BYTES])
{
	if (fi != YSF_FI_COMMS || fn == 0) {
		ysf_head_csd(&t->id, c1, c2);
		return;
	}
	memset(c1, ' ', YSF_CSD_BYTES);
	memset(c2, ' ', YSF_CSD_BYTES);
	if (fn == 1) {
		dst_field(&t->id, c1 + YSF_CS_LEN);
		memset(c1 + YSF_CS_LEN, ' ', 5);
		memcpy(c2, t->data_buf, t->data_len < YSF_CSD_BYTES ? t->data_len : YSF_CSD_BYTES);
		return;
	}
	for (uint8_t i = 0; i < YSF_CSD_BYTES; i++) {
		unsigned at = 20u + (fn - 2u) * 40u + i;

		if (at < t->data_len && at < YSF_WX_CMD_MAX)
			c1[i] = t->data_buf[at];
		if (at + YSF_CSD_BYTES < t->data_len && at + YSF_CSD_BYTES < YSF_WX_CMD_MAX)
			c2[i] = t->data_buf[at + YSF_CSD_BYTES];
	}
}

static void tx_frame(ysf_call_tx_t *t, uint8_t state)
{
	ysf_fich_t f;
	uint8_t a[YSF_CSD_BYTES], b[YSF_CSD_BYTES];

	t->state = state;
	t->sent = 0;
	t->nvoice = 0;
	t->nsyms = YSF_FRAME_SYMS;
	switch (state) {
	case TX_PRE:
		memset(t->frame, 0x77, sizeof t->frame);	// +3 -3: the receivers' level and timing reference
		t->ready = YSF_FRAME_SYMS;
		break;
	case TX_HEAD:
	case TX_TERM:
		ysf_frame_begin(t->frame);
		fich_for(t, state == TX_HEAD ? YSF_FI_HEADER : YSF_FI_TERM, &f);
		ysf_fich_put(t->frame, &f);
		if (t->data)
			data_csd(t, f.fi, 0, a, b);
		else
			ysf_head_csd(&t->id, a, b);
		ysf_csd_put(t->frame, a, b);
		t->ready = YSF_FRAME_SYMS;
		break;
	case TX_COMMS:
		if (t->data) {
			ysf_frame_begin(t->frame);
			fich_for(t, YSF_FI_COMMS, &f);
			ysf_fich_put(t->frame, &f);
			data_csd(t, YSF_FI_COMMS, t->fn, a, b);
			ysf_csd_put(t->frame, a, b);
			t->ready = YSF_FRAME_SYMS;
			break;
		}
		// The DCH spreads over all five blocks, so it is written first; each VCH releases its block. Nothing goes
		// out before the first VCH: an end that comes first sends the terminator right after the frame before
		ysf_frame_begin(t->frame);
		fich_for(t, YSF_FI_COMMS, &f);
		ysf_fich_put(t->frame, &f);
		ysf_dch_content(&t->id, t->fn, a);
		ysf_dch_put(t->frame, a);
		t->ready = 0;
		break;
	default:
		t->nsyms = t->ready = 0;
		break;
	}
}

void ysf_call_tx_begin(ysf_call_tx_t *t, const ysf_tx_id_t *id)
{
	memset(t, 0, sizeof *t);
	t->id = *id;
	tx_frame(t, TX_PRE);
}

uint16_t ysf_call_tx_ready(const ysf_call_tx_t *t, uint16_t *first)
{
	*first = t->sent;
	return (uint16_t)(t->ready - t->sent);
}

const uint8_t *ysf_call_tx_syms(const ysf_call_tx_t *t)
{
	return t->frame;
}

void ysf_call_tx_sent(ysf_call_tx_t *t, uint16_t n)
{
	t->sent = (uint16_t)(t->sent + n);
	if (t->sent < t->nsyms || t->state == TX_DONE)
		return;
	switch (t->state) {
	case TX_PRE:
		tx_frame(t, TX_HEAD);
		break;
	case TX_HEAD:
		t->fn = 0;
		tx_frame(t, t->ending ? TX_TERM : TX_COMMS);
		break;
	case TX_COMMS:
		if (t->data) {
			// The command's frames are FN 0 to its last, then the terminator
			if (t->fn >= t->data_ft) {
				tx_frame(t, TX_TERM);
			} else {
				t->fn++;
				tx_frame(t, TX_COMMS);
			}
			break;
		}
		t->fn = (uint8_t)(t->fn >= YSF_FT_DN ? 0 : t->fn + 1);
		tx_frame(t, t->ending ? TX_TERM : TX_COMMS);
		break;
	default:
		tx_frame(t, TX_DONE);
		break;
	}
}

void ysf_call_tx_begin_data(ysf_call_tx_t *t, const ysf_tx_id_t *id, const uint8_t *data, uint8_t len)
{
	memset(t, 0, sizeof *t);
	t->id = *id;
	t->data = true;
	t->data_len = len < YSF_WX_CMD_MAX ? len : YSF_WX_CMD_MAX;
	memcpy(t->data_buf, data, t->data_len);
	// FN 1 holds 20 bytes, each later FN 40
	t->data_ft = 1;
	while (20u + (t->data_ft - 1u) * 40u < t->data_len && t->data_ft < 7)
		t->data_ft++;
	tx_frame(t, TX_PRE);
}

bool ysf_call_tx_wants_voice(const ysf_call_tx_t *t)
{
	return !t->data && t->state == TX_COMMS && t->nvoice < YSF_VCH_PER_FRAME;
}

bool ysf_call_tx_voice(ysf_call_tx_t *t, const uint8_t v[YSF_VOICE_BYTES])
{
	uint8_t bits[AMBE_PARAM_BITS];

	if (!ysf_call_tx_wants_voice(t))
		return false;
	ambeFecDecode(v, bits, NULL);
	ysf_vch_put(t->frame, t->nvoice, bits);
	t->nvoice++;
	// Block n is whole with its VCH; the next block's DCH part goes with it
	t->ready = t->nvoice < YSF_VCH_PER_FRAME ? (uint16_t)(YSF_BLOCK_SYM(t->nvoice) + 20) : YSF_FRAME_SYMS;
	return true;
}

void ysf_call_tx_end(ysf_call_tx_t *t, const uint8_t silence[YSF_VOICE_BYTES])
{
	if (t->data)
		return;		// a data transmission ends by itself
	t->ending = true;
	// A communications frame with nothing on air yet is not sent at all
	if (t->state == TX_COMMS && t->nvoice == 0) {
		tx_frame(t, TX_TERM);
		return;
	}
	while (ysf_call_tx_voice(t, silence))
		;
}

bool ysf_call_tx_done(const ysf_call_tx_t *t)
{
	return t->state == TX_DONE;
}
