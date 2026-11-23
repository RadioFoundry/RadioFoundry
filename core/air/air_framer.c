// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The framer (air_framer.h, docs/spec/air-interface.md 5).
//
// Receive, one symbol at a time: the symbol is sliced (from the soft value with the measured levels, else as the
// provider sliced it) into a 24-symbol shift register. Searching, every sync pattern is compared with the register;
// a match is checked against the soft values (a straight-line fit of what came in to what the pattern says gives the
// outer level and the zero offset, and must leave the pattern readable), then the frame starts where the pattern's
// frame_at says: its first symbols come out of the register, or out of a ring of history kept in the frame buffer
// when a pattern sits further into its frame than the register reaches. Locked, symbols go into the frame buffer;
// each pattern is looked for where it belongs and nowhere else, unless the last one was missed.

#include "air_framer.h"

#include <string.h>

#include "air_syms.h"

enum { ST_SEARCH, ST_LOCKED };

#define REG_MASK	0xFFFFFFFFFFFFull	// 24 dibits
#define REG_SIGN	0xAAAAAAAAAAAAull	// each dibit's first bit: the symbol's sign
#define REG_LOW		0x555555555555ull	// each dibit's second bit: set on an outer symbol

// What a sync's soft values may say and still be a sync: the outer level against the nominal AIR_SOFT_OUTER, the zero
// offset (a tuning error of half an outer symbol), and how far the values lie from the fitted line on average. The
// floor on the level is above half the nominal on purpose: a run of inner symbols with the right signs fits an outer
// level of a third, and reads as a sync of outer symbols at any floor up to a half
#define LVL_MIN		(AIR_SOFT_OUTER * 7 / 12)
#define LVL_MAX		(AIR_SOFT_OUTER * 3 / 2)
#define OFF_MAX		(AIR_SOFT_OUTER / 2)
#define RESID_DIV	6	// mean distance from the line under a sixth of the outer level: half a decision margin

static uint8_t pop32(uint32_t v)
{
	v = v - ((v >> 1) & 0x55555555u);
	v = (v & 0x33333333u) + ((v >> 2) & 0x33333333u);
	return (uint8_t)((((v + (v >> 4)) & 0x0F0F0F0Fu) * 0x01010101u) >> 24);
}

static uint8_t pop64(uint64_t v)
{
	return (uint8_t)(pop32((uint32_t)v) + pop32((uint32_t)(v >> 32)));
}

// The pattern with its last symbol in bits 1-0, as the register holds what came in
static uint64_t sync_pat(const air_sync_t *s)
{
	uint64_t v = 0;

	for (uint8_t i = 0; i < AIR_SYNC_SYMS_MAX / 4; i++)
		v = (v << 8) | s->syms[i];
	return v >> (2 * (AIR_SYNC_SYMS_MAX - s->nsyms));
}

static uint64_t sync_mask(const air_sync_t *s)
{
	return REG_MASK >> (2 * (AIR_SYNC_SYMS_MAX - s->nsyms));
}

// Symbols of a frame up to and including the pattern's last
static int16_t sync_need(const air_sync_t *s)
{
	return (int16_t)(s->nsyms - s->frame_at);
}

static uint32_t syms_us(const air_framer_t *fr, uint16_t n)
{
	return (uint32_t)(((uint64_t)n * 1000000u) / fr->mode->profile.sym_rate);
}

static uint8_t slice(const air_framer_t *fr, int8_t s)
{
	int16_t v = (int16_t)(s - fr->off);
	int16_t th = (int16_t)(fr->lvl * 2 / 3);

	if (fr->mode->profile.levels == 2)
		return v >= 0 ? 1 : 3;
	if (v >= th)
		return 1;
	if (v >= 0)
		return 0;
	return v > -th ? 2 : 3;
}

// The soft value a mode gets: offset removed, +3 at AIR_SOFT_OUTER
static int8_t rescale(const air_framer_t *fr, int8_t s)
{
	int32_t v = ((int32_t)(s - fr->off) * AIR_SOFT_OUTER) / fr->lvl;

	return (int8_t)(v > 127 ? 127 : v < -127 ? -127 : v);
}

// The soft value of the symbol back symbols before the newest (0 = the newest)
static int8_t hist_get(const air_framer_t *fr, uint8_t back)
{
	return fr->hist[(uint8_t)(fr->hist_at + AIR_SYNC_SYMS_MAX - 1 - back) % AIR_SYNC_SYMS_MAX];
}

// Fit the newest soft values to pattern s: in = off + g x level, least squares. False when they can't be this
// pattern at any sane level; else the outer level and offset
static bool sync_fit(const air_framer_t *fr, const air_sync_t *s, int16_t *lvl, int16_t *off)
{
	int32_t se = 0, see = 0, ss = 0, sse = 0, n = s->nsyms, det, num, l, o, resid = 0;

	for (uint8_t k = 0; k < s->nsyms; k++) {
		int8_t e = air_sym_level(air_sym_get(s->syms, k));
		int8_t v = hist_get(fr, (uint8_t)(s->nsyms - 1 - k));

		se += e;
		see += e * e;
		ss += v;
		sse += v * e;
	}
	det = n * see - se * se;
	if (det <= 0)
		return false;		// a pattern of one level only: no slope to fit
	num = n * sse - ss * se;
	l = (3 * num) / det;
	o = (ss - (num * se) / det) / n;
	if (l < LVL_MIN || l > LVL_MAX || o < -OFF_MAX || o > OFF_MAX)
		return false;
	// Noise passes the signs now and then, at a level of its own; it does not lie on a line
	for (uint8_t k = 0; k < s->nsyms; k++) {
		int32_t d = hist_get(fr, (uint8_t)(s->nsyms - 1 - k)) - o - (l * air_sym_level(air_sym_get(s->syms, k))) / 3;

		resid += d < 0 ? -d : d;
	}
	if (resid * RESID_DIV > n * l)
		return false;
	*lvl = (int16_t)l;
	*off = (int16_t)o;
	return true;
}

// Symbols of pattern s the newest soft values read wrong at the levels in force
static uint8_t sync_soft_errs(const air_framer_t *fr, const air_sync_t *s)
{
	uint8_t errs = 0;

	for (uint8_t k = 0; k < s->nsyms; k++)
		if (slice(fr, hist_get(fr, (uint8_t)(s->nsyms - 1 - k))) != air_sym_get(s->syms, k))
			errs++;
	return errs;
}

// Is pattern s what just came in? With soft values: the signs must match first (they hold at any deviation), then
// the fit sets the levels and the pattern must read right at them. track: move the levels a quarter of the way (a
// sync where one was due) instead of taking the new ones whole. Without soft values: the dibits as given
static bool sync_match(air_framer_t *fr, const air_sync_t *s, bool track, uint8_t *errs)
{
	uint64_t x = (fr->reg ^ sync_pat(s)) & sync_mask(s);
	int16_t lvl, off, lvl0 = fr->lvl, off0 = fr->off;

	if (fr->run < s->nsyms)
		return false;
	if (!fr->soft_in) {
		*errs = pop64((x | (x >> 1)) & REG_LOW);
		return *errs <= s->max_errs;
	}
	if (pop64(x & REG_SIGN) > s->max_errs || !sync_fit(fr, s, &lvl, &off))
		return false;
	if (track) {
		lvl = (int16_t)(lvl0 + (lvl - lvl0) / 4);
		off = (int16_t)(off0 + (off - off0) / 4);
	}
	fr->lvl = lvl;
	fr->off = off;
	*errs = sync_soft_errs(fr, s);
	if (*errs <= s->max_errs)
		return true;
	fr->lvl = lvl0;
	fr->off = off0;
	return false;
}

static void buf_put(air_framer_t *fr, uint16_t i, uint8_t d, int8_t soft)
{
	air_sym_put(fr->hard, i, d);
	if (fr->soft)
		fr->soft[i] = soft;
}

static void buf_swap(air_framer_t *fr, uint16_t a, uint16_t b)
{
	uint8_t d = air_sym_get(fr->hard, a);

	air_sym_put(fr->hard, a, air_sym_get(fr->hard, b));
	air_sym_put(fr->hard, b, d);
	if (fr->soft) {
		int8_t s = fr->soft[a];

		fr->soft[a] = fr->soft[b];
		fr->soft[b] = s;
	}
}

static void buf_reverse(air_framer_t *fr, uint16_t a, uint16_t n)
{
	for (uint16_t b = (uint16_t)(a + n); n > 1 && a < --b; a++)
		buf_swap(fr, a, b);
}

static void search_start(air_framer_t *fr)
{
	fr->state = ST_SEARCH;
	fr->misses = 0;
	fr->ring_at = 0;
	fr->lvl = AIR_SOFT_OUTER;
	fr->off = 0;
}

static void drop(air_framer_t *fr)
{
	// A lock the head hook never agreed to was a chance match: the mode saw nothing of it and hears nothing now
	bool was = fr->state == ST_LOCKED && fr->confirmed;

	search_start(fr);
	if (was && fr->mode->rx_lost)
		fr->mode->rx_lost(fr->ctx);
}

static void deliver(air_framer_t *fr, uint8_t flags)
{
	air_frame_t f;

	f.t_us = fr->t_frame;
	f.sync_id = fr->sync_id;
	f.sync_errs = fr->sync_errs;
	f.flags = (uint8_t)(flags | (fr->sync_seen ? 0 : AIR_FRAME_NO_SYNC));
	f.nsyms = fr->bpos;
	f.offset = (uint16_t)(fr->upos - fr->bpos);
	f.rssi_dbm = fr->rssi_dbm;
	f.hard = fr->hard;
	f.soft = fr->soft_out ? fr->soft : NULL;
	if (flags & AIR_FRAME_HEAD) {
		uint16_t r = fr->mode->head(&f, fr->ctx);

		if (r & AIR_HEAD_AGAIN) {
			r &= (uint16_t)~AIR_HEAD_AGAIN;
			// Not one it can have: the head must fit the buffer whole
			fr->head_at = r > fr->upos && r <= fr->buf_syms && fr->upos == fr->bpos ? r : 0;
			fr->unit = fr->head_at ? 0 : UINT16_MAX;
		} else {
			fr->head_at = 0;
			fr->unit = r ? (r < fr->upos ? fr->upos : r) : UINT16_MAX;
			if (r)
				fr->confirmed = true;
		}
		return;
	}
	fr->stats.frames++;
	fr->mode->rx_frame(&f, fr->ctx);
}

static void rx_sym(air_framer_t *fr, uint8_t d, int8_t soft, bool search);

// The head hook said no: the sync was a chance match. What came in behind it is in the buffer; a real sync may be
// in there, so search it again before going on with the stream
static void replay(air_framer_t *fr, uint16_t from)
{
	uint16_t n = fr->bpos;
	bool soft_in = fr->soft_in;
	uint32_t t = fr->t_sym;

	fr->stats.rejects++;
	drop(fr);
	if (fr->ring || fr->relock)
		return;			// the ring lives in the same buffer; and never a replay inside a replay
	fr->relock = true;
	fr->soft_in = false;		// the buffer holds symbols as sliced and scaled: no second fit
	fr->run = 0;
	for (uint16_t i = 0; i < n; i++) {
		// A lock in here writes the buffer behind the read index only (its frame starts at most i symbols back)
		fr->t_sym = t - syms_us(fr, (uint16_t)(n - 1 - i));
		rx_sym(fr, air_sym_get(fr->hard, i), fr->soft ? fr->soft[i] : 0, i >= from);
	}
	fr->t_sym = t;
	fr->soft_in = soft_in;
	fr->relock = false;
}

// After a symbol went into the buffer: the head hook, a full buffer, the end of the unit
static void progress(air_framer_t *fr)
{
	const air_mode_t *m = fr->mode;

	while (fr->head_at && fr->upos >= fr->head_at) {
		deliver(fr, AIR_FRAME_HEAD);
		if (fr->unit == UINT16_MAX) {
			// Symbols after the pattern that locked us are searched again (need: lock_syms kept in ring_at)
			replay(fr, fr->ring_at);
			return;
		}
	}
	if (fr->unit && fr->upos >= fr->unit) {
		deliver(fr, 0);
		fr->upos = fr->bpos = 0;
		fr->sync_seen = false;
		fr->head_at = m->head ? m->head_syms : 0;
		fr->unit = fr->head_at ? 0 : m->frame_syms;
	} else if (fr->bpos >= fr->buf_syms && !fr->head_at) {
		deliver(fr, AIR_FRAME_MORE);
		fr->bpos = 0;
	}
}

// A frame starts need symbols back from the newest: take them from the register (or the ring) and go on from there
static bool lock(air_framer_t *fr, const air_sync_t *s, uint8_t errs)
{
	const air_mode_t *m = fr->mode;
	int16_t need = sync_need(s);
	uint16_t n = (uint16_t)need;

	if (need < 1 || fr->run < n || n > fr->buf_syms)
		return false;		// the frame started before our history did
	if (fr->ring) {
		// Rotate the ring so the frame's first symbol is at 0
		uint16_t st = (uint16_t)((fr->ring_at + fr->ring - n) % fr->ring);

		buf_reverse(fr, 0, st);
		buf_reverse(fr, st, (uint16_t)(fr->ring - st));
		buf_reverse(fr, 0, fr->ring);
	} else if (n > AIR_SYNC_SYMS_MAX) {
		return false;
	}
	// The symbols the register still has: sliced again at the levels this sync gave
	for (uint16_t k = n > AIR_SYNC_SYMS_MAX ? (uint16_t)(n - AIR_SYNC_SYMS_MAX) : 0; k < n; k++) {
		uint8_t back = (uint8_t)(n - 1 - k);

		if (fr->soft_in)
			buf_put(fr, k, slice(fr, hist_get(fr, back)), rescale(fr, hist_get(fr, back)));
		else
			buf_put(fr, k, (uint8_t)((fr->reg >> (2 * back)) & 3), fr->relock ? hist_get(fr, back) : 0);
	}
	fr->state = ST_LOCKED;
	fr->misses = 0;
	fr->sync_seen = true;
	fr->sync_id = s->id;
	fr->sync_errs = errs;
	fr->upos = fr->bpos = n;
	fr->ring_at = n;		// locked: where the search goes on if the head hook says no
	fr->t_frame = fr->t_sym - syms_us(fr, (uint16_t)(n - 1));
	fr->head_at = m->head ? m->head_syms : 0;
	fr->unit = fr->head_at ? 0 : m->frame_syms;
	fr->confirmed = !fr->head_at;
	fr->stats.locks++;
	fr->stats.syncs++;
	progress(fr);
	return true;
}

static void rx_sym(air_framer_t *fr, uint8_t d, int8_t soft, bool search)
{
	const air_profile_t *p = &fr->mode->profile;
	uint16_t due = 0;
	uint8_t errs;

	// The soft value kept: scaled to nominal, or in a replay the one kept before
	int8_t keep = fr->soft_in ? 0 : fr->relock ? soft : 0;

	if (fr->soft_in || fr->relock) {
		fr->hist[fr->hist_at] = soft;
		fr->hist_at = (uint8_t)((fr->hist_at + 1) % AIR_SYNC_SYMS_MAX);
	}
	if (fr->soft_in) {
		d = slice(fr, soft);
		keep = rescale(fr, soft);
	}
	fr->reg = ((fr->reg << 2) | d) & REG_MASK;
	if (fr->run < UINT16_MAX)
		fr->run++;

	if (fr->state == ST_SEARCH) {
		if (fr->ring) {
			buf_put(fr, fr->ring_at, d, keep);
			fr->ring_at = (uint16_t)((fr->ring_at + 1) % fr->ring);
		}
		for (uint8_t i = 0; search && i < p->nsyncs; i++) {
			if (!sync_match(fr, &p->sync[i], false, &errs))
				continue;
			if (lock(fr, &p->sync[i], errs))
				return;
			search_start(fr);	// the levels it set are no use without the lock
		}
		return;
	}

	if (fr->upos == 0)
		fr->t_frame = fr->t_sym;
	buf_put(fr, fr->bpos, d, keep);
	fr->bpos++;
	fr->upos++;
	// A sync where one belongs
	for (uint8_t i = 0; i < p->nsyncs; i++) {
		const air_sync_t *s = &p->sync[i];
		int16_t need = sync_need(s);

		if ((s->flags & AIR_SYNC_START_ONLY) || need < 1)
			continue;
		if ((uint16_t)need > due)
			due = (uint16_t)need;
		if (fr->upos != (uint16_t)need || fr->sync_seen || !sync_match(fr, s, true, &errs))
			continue;
		fr->sync_seen = true;
		fr->sync_id = s->id;
		fr->sync_errs = errs;
		fr->misses = 0;
		fr->stats.syncs++;
	}
	if (fr->upos == due && !fr->sync_seen) {
		fr->stats.misses++;
		if (++fr->misses >= fr->mode->lost_frames) {
			drop(fr);
			fr->run = fr->run > AIR_SYNC_SYMS_MAX ? AIR_SYNC_SYMS_MAX : fr->run;
			return;
		}
	}
	// A sync where none belongs counts only while the last one due was missed: the signal we follow may be gone
	// and another one there. (A short pattern also turns up in payload, so never while our own syncs come.)
	if (fr->misses && !(fr->upos <= due && fr->sync_seen)) {
		for (uint8_t i = 0; i < p->nsyncs; i++) {
			const air_sync_t *s = &p->sync[i];
			int16_t lvl = fr->lvl, off = fr->off;

			if (fr->upos == (uint16_t)sync_need(s) || !sync_match(fr, s, false, &errs))
				continue;
			if (sync_need(s) <= AIR_SYNC_SYMS_MAX && !fr->ring) {
				if (fr->confirmed && fr->mode->rx_lost)
					fr->mode->rx_lost(fr->ctx);
				if (lock(fr, s, errs))
					return;
			}
			fr->lvl = lvl;
			fr->off = off;
			drop(fr);		// its frame began before what the register holds: wait for the next one
			return;
		}
	}
	progress(fr);
}

void air_framer_init(air_framer_t *fr, const air_mode_t *mode, void *ctx, uint8_t *hard, int8_t *soft,
		     uint16_t buf_syms)
{
	memset(fr, 0, sizeof(*fr));
	fr->mode = mode;
	fr->ctx = ctx;
	fr->hard = hard;
	fr->soft = soft;
	fr->buf_syms = buf_syms;
	for (uint8_t i = 0; i < mode->profile.nsyncs; i++) {
		int16_t need = sync_need(&mode->profile.sync[i]);

		// A pattern further into its frame than the register reaches: keep that much history in the buffer
		if (need > AIR_SYNC_SYMS_MAX && (uint16_t)need > fr->ring && (uint16_t)need <= buf_syms)
			fr->ring = (uint16_t)need;
	}
	air_framer_rx_reset(fr);
}

void air_framer_rx_reset(air_framer_t *fr)
{
	search_start(fr);
	fr->run = 0;
	fr->reg = 0;
	fr->relock = false;
}

void air_framer_rx(air_framer_t *fr, const air_chunk_t *c)
{
	if (c->flags & (AIR_CHUNK_GAP | AIR_CHUNK_NO_SIGNAL)) {
		// The count from the last sync means nothing across a hole
		drop(fr);
		fr->run = 0;
		if (c->flags & AIR_CHUNK_NO_SIGNAL)
			return;
	}
	fr->soft_in = fr->soft_out = c->soft != NULL;
	fr->rssi_dbm = c->rssi_dbm;
	for (uint16_t i = 0; i < c->nsyms; i++) {
		fr->t_sym = c->t_us + syms_us(fr, i);
		rx_sym(fr, air_sym_get(c->hard, i), c->soft ? c->soft[i] : 0, true);
	}
}

bool air_framer_locked(const air_framer_t *fr)
{
	return fr->state == ST_LOCKED;
}

void air_framer_tx_start(air_framer_t *fr, uint32_t fit)
{
	fr->txc.nsyms = 0;
	fr->txc.flags = 0;
	fr->txc.at_slot = 0;
	fr->tx_double = (fit & AIR_FIT_RATE_DOUBLED) != 0;
}

uint16_t air_framer_tx_space(const air_framer_t *fr, const hal_air_t *air)
{
	uint8_t chunks = air->tx_space();
	uint16_t n;

	// A chunk goes to the queue the moment it is full, so with no room there the open one takes nothing either
	if (!chunks)
		return 0;
	n = (uint16_t)(chunks * air->tx_chunk_syms - fr->txc.nsyms);
	return fr->tx_double ? n / 2 : n;
}

bool air_framer_tx(air_framer_t *fr, const hal_air_t *air, const uint8_t *syms, uint16_t first, uint16_t nsyms)
{
	if (nsyms > air_framer_tx_space(fr, air))
		return false;
	for (uint16_t i = 0; i < (fr->tx_double ? 2 * nsyms : nsyms); i++) {
		air_sym_put(fr->txc.syms, fr->txc.nsyms++, air_sym_get(syms, (uint16_t)(first + (fr->tx_double ? i / 2 : i))));
		if (fr->txc.nsyms == air->tx_chunk_syms) {
			air->tx_queue(&fr->txc);
			fr->txc.nsyms = 0;
		}
	}
	return true;
}

bool air_framer_tx_finish(air_framer_t *fr, const hal_air_t *air)
{
	if (!fr->txc.nsyms) {
		air->tx_end();
		return true;
	}
	fr->txc.flags = AIR_TX_LAST;
	if (!air->tx_queue(&fr->txc)) {
		fr->txc.flags = 0;
		return false;
	}
	fr->txc.nsyms = 0;
	fr->txc.flags = 0;
	return true;
}
