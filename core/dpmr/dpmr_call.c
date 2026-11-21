// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A dPMR voice call (dpmr.h): on receive, the units of dpmr_rx.c become a call with its IDs and its voice frames in
// order; on transmit, the frames are built and released symbol by symbol. Section numbers are ETSI TS 102 490's.
#include "dpmr.h"

#include <string.h>

// A header older than this when the first payload frame comes belongs to something else (a header and end pair, a
// call whose voice was lost): nine extended wake-up headers and their normal one are 800 ms, the voice follows at once
#define HDR_HOLD_US	200000u

// ── Receive ──
static void update(dpmr_call_rx_t *c)
{
	if (c->sink->update)
		c->sink->update(&c->call, c->ctx);
}

static void call_end(dpmr_call_rx_t *c, bool ended)
{
	c->pending = false;
	if (!c->active)
		return;
	c->active = false;
	c->call.ended = ended;
	if (c->sink->end)
		c->sink->end(&c->call, c->ctx);
}

// The call starts at its first payload frame: a header alone carries no voice (a connection request, or a call
// whose voice never came), and a run of wake-up headers is one call, not several
static void call_start(dpmr_call_rx_t *c, const dpmr_rx_unit_t *u)
{
	memset(&c->call, 0, sizeof c->call);
	c->call.rssi_dbm = u->rssi_dbm;
	if (c->pending && (uint32_t)(u->t_us - c->hdr_us) <= HDR_HOLD_US) {
		c->call.called = c->hdr.called;
		c->call.own = c->hdr.own;
		c->call.mode = c->hdr.mode;
		c->call.format = c->hdr.format;
		c->call.have = DPMR_HAVE_CALLED | DPMR_HAVE_OWN | DPMR_HAVE_HEADER;
		c->call.quarters = 0x0F;
		if (c->hdr_cc != 0xFFFFu) {
			c->call.cc = c->hdr_cc;
			c->call.have |= DPMR_HAVE_CC;
		}
	}
	c->pending = false;
	c->active = true;
	c->stats.calls++;
	if (c->sink->start)
		c->sink->start(&c->call, c->ctx);
}

static void on_header(dpmr_call_rx_t *c, const dpmr_rx_unit_t *u)
{
	c->stats.headers++;
	if (!u->ok) {
		c->stats.hi_fail++;
		return;
	}
	// A header inside a call from someone else: the old call's end was lost
	if (c->active && (u->u.hi.called != c->call.called || u->u.hi.own != c->call.own))
		call_end(c, false);
	c->pending = true;
	c->hdr = u->u.hi;
	c->hdr_cc = u->cc_bad == 0 ? u->cc : 0xFFFFu;
	c->hdr_us = u->t_us;
}

// A CCH's quarter of an ID (9.1): the called ID in frames 1 and 2, our caller's in 3 and 4
static bool learn_quarter(dpmr_call_t *call, const dpmr_cch_t *cch)
{
	uint32_t *id = cch->fn < 2u ? &call->called : &call->own;
	uint32_t shift = (cch->fn & 1u) ? 0u : 12u;
	uint32_t was = *id;
	uint8_t have = call->have;

	*id = (*id & ~(0xFFFu << shift)) | ((uint32_t)cch->id12 << shift);
	call->quarters |= (uint8_t)(1u << cch->fn);
	if ((call->quarters & 0x3u) == 0x3u)
		call->have |= DPMR_HAVE_CALLED;
	if ((call->quarters & 0xCu) == 0xCu)
		call->have |= DPMR_HAVE_OWN;
	return *id != was || call->have != have || call->mode != cch->mode || call->format != cch->format;
}

static bool voice_mode(uint8_t mode)
{
	return mode == DPMR_M_VOICE || mode == DPMR_M_VOICE_SLD;
}

static void on_payload(dpmr_call_rx_t *c, const dpmr_rx_unit_t *u)
{
	bool changed = false;

	c->stats.frames++;
	if (!u->ok)
		c->stats.cch_fail++;
	if (!c->active) {
		if (!u->ok) {
			c->stats.voice_held += DPMR_TCH_FRAMES;	// nothing yet says this is a call, or whose
			return;
		}
		call_start(c, u);
	}
	if (u->rssi_dbm)
		c->call.rssi_dbm = u->rssi_dbm;
	if (u->ok) {
		changed = learn_quarter(&c->call, &u->u.cch);
		c->call.mode = u->u.cch.mode;
		c->call.format = u->u.cch.format;
	}
	if (u->second && u->cc_bad == 0 && (!(c->call.have & DPMR_HAVE_CC) || c->call.cc != u->cc)) {
		c->call.cc = u->cc;
		c->call.have |= DPMR_HAVE_CC;
		changed = true;
	}
	if (changed)
		update(c);
	for (uint8_t n = 0; n < DPMR_TCH_FRAMES; n++) {
		uint8_t ambe[DPMR_AMBE_BYTES];

		if (!voice_mode(c->call.mode)) {
			c->stats.voice_held++;
			continue;
		}
		dpmr_tch_get(u->hard, n, ambe);
		c->stats.voice++;
		if (c->sink->voice)
			c->sink->voice(ambe, c->ctx);
	}
}

static void on_unit(const dpmr_rx_unit_t *u, void *ctx)
{
	dpmr_call_rx_t *c = ctx;

	c->stats.ham_fixed += u->res.fixed;
	c->stats.ham_bad += u->res.bad;
	switch (u->kind) {
	case DPMR_UNIT_HEADER:
		on_header(c, u);
		break;
	case DPMR_UNIT_END:
		c->stats.ends++;
		if (!u->ok)
			c->stats.end_fail++;	// the FS3 alone says the call is over
		call_end(c, true);
		break;
	default:
		on_payload(c, u);
		break;
	}
}

static void on_lost(void *ctx)
{
	dpmr_call_rx_t *c = ctx;

	// A header kept for voice that has not come survives a lock lost between headers (a preamble longer than the
	// standard's least); HDR_HOLD_US decides whether it still counts
	if (c->active)
		call_end(c, false);
}

void dpmr_call_rx_init(dpmr_call_rx_t *c, const dpmr_call_sink_t *sink, void *ctx)
{
	memset(c, 0, sizeof *c);
	c->sink = sink;
	c->ctx = ctx;
	// (on_unit's context is c; the framer's is c->rx, the same address)
	dpmr_rx_init(&c->rx, on_unit, on_lost, c);
}

// ── Transmit (4.2.3) ──
enum { TX_HEADER, TX_PAYLOAD, TX_END, TX_DONE };

static void tx_frame(dpmr_call_tx_t *t, uint8_t state)
{
	t->state = state;
	t->sent = 0;
	t->nvoice = 0;
	switch (state) {
	case TX_HEADER: {
		// A run of headers is extended wake-up headers counting down to a normal one (5.10.1), as a dPMR446 radio
		// sends them: CI type 111 and the headers still to come
		dpmr_hi_t h = t->hi;
		uint8_t left = (uint8_t)(t->headers - 1u - t->headers_sent);

		if (left != 0)
			h.ci = (uint16_t)(DPMR_CI_EXT_WAKEUP | (left > 15u ? 15u : left));
		dpmr_header_put(t->frame, &h, t->cc);
		t->nsyms = t->ready = DPMR_FRAME_SYMS;
		break;
	}
	case TX_PAYLOAD: {
		dpmr_cch_t c = {
			.fn = t->fn, .id12 = dpmr_id_quarter(&t->hi, t->fn), .mode = t->hi.mode,
			.version = t->hi.version, .format = t->hi.format, .sld = 0,
		};

		// Nothing of a payload frame goes out before its first TCH is in: its sync and CCH wait for the voice
		dpmr_payload_put(t->frame, &c, t->cc);
		t->nsyms = DPMR_FRAME_SYMS;
		t->ready = 0;
		break;
	}
	case TX_END: {
		static const dpmr_end_info_t normal = { 0, 0, 0, 0 };	// a normal end, no acknowledgement asked, no wait

		dpmr_end_put(t->frame, &normal);
		t->nsyms = t->ready = DPMR_END_SYMS;
		break;
	}
	default:
		t->nsyms = t->ready = 0;
		break;
	}
}

static void fill_silence(dpmr_call_tx_t *t)
{
	while (t->silence && dpmr_call_tx_voice(t, t->silence))
		;
}

void dpmr_call_tx_begin(dpmr_call_tx_t *t, const dpmr_hi_t *hi, uint16_t cc, uint8_t headers)
{
	memset(t, 0, sizeof *t);
	t->hi = *hi;
	t->cc = cc;
	t->headers = headers ? headers : 1u;
	tx_frame(t, TX_HEADER);
}

uint16_t dpmr_call_tx_ready(const dpmr_call_tx_t *t, uint16_t *first)
{
	*first = t->sent;
	return (uint16_t)(t->ready - t->sent);
}

const uint8_t *dpmr_call_tx_syms(const dpmr_call_tx_t *t)
{
	return t->frame;
}

void dpmr_call_tx_sent(dpmr_call_tx_t *t, uint16_t n)
{
	t->sent = (uint16_t)(t->sent + n);
	if (t->sent < t->nsyms || t->state == TX_DONE)
		return;
	switch (t->state) {
	case TX_HEADER:
		if (++t->headers_sent < t->headers) {
			tx_frame(t, TX_HEADER);
			break;
		}
		t->fn = 0;
		tx_frame(t, TX_PAYLOAD);
		fill_silence(t);	// (an end before the voice: a call is at least one superframe, 4.2.3)
		break;
	case TX_PAYLOAD:
		// A call is whole superframes: the end frame follows the fourth payload frame only
		t->fn = (uint8_t)((t->fn + 1u) & 3u);
		tx_frame(t, t->ending && t->fn == 0 ? TX_END : TX_PAYLOAD);
		if (t->state == TX_PAYLOAD && t->ending)
			fill_silence(t);
		break;
	default:
		tx_frame(t, TX_DONE);
		break;
	}
}

bool dpmr_call_tx_wants_voice(const dpmr_call_tx_t *t)
{
	return t->state == TX_PAYLOAD && t->nvoice < DPMR_TCH_FRAMES;
}

bool dpmr_call_tx_voice(dpmr_call_tx_t *t, const uint8_t ambe[DPMR_AMBE_BYTES])
{
	if (!dpmr_call_tx_wants_voice(t))
		return false;
	dpmr_tch_put(t->frame, t->nvoice, ambe);
	t->nvoice++;
	t->ready = (uint16_t)DPMR_TCH_SYM(t->nvoice);
	return true;
}

void dpmr_call_tx_end(dpmr_call_tx_t *t, const uint8_t silence[DPMR_AMBE_BYTES])
{
	t->ending = true;
	t->silence = silence;
	fill_silence(t);
}

bool dpmr_call_tx_done(const dpmr_call_tx_t *t)
{
	return t->state == TX_DONE;
}
