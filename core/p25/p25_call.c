// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A conventional P25 voice call (p25.h): on receive, the units of p25_rx.c become a call with its fields and its
// voice frames in order; on transmit, the units are built and released symbol by symbol. Clause numbers are
// TIA-102.BAAA-A's.

#include "p25.h"

#include <string.h>

// ── Receive ──
#if CONFIG_P25_ALIAS_TRACE
volatile p25_alias_trace_t p25_alias_trace;
static void trace_unit(const p25_call_rx_t *c, const p25_rx_unit_t *u, const uint8_t *raw, int valid)
{
	uint32_t serial = p25_alias_trace.count + 1;
	volatile p25_alias_trace_entry_t *e = &p25_alias_trace.entries[(serial - 1) % 64];
	e->serial = 0;
	e->t_us = u ? u->t_us : 0;
	e->src = c->call.have_lc ? c->call.lc.src : 0;
	e->nac = u ? u->nac : c->call.nac;
	e->duid = u ? u->duid : 255;
	e->flags = (c->active ? 1 : 0) | (valid > 0 ? 2 : valid < 0 ? 4 : 0) |
		(c->call.have_alg && c->call.algid != P25_ALGID_CLEAR ? 8 : 0);
	for (unsigned i = 0; i < 9; i++) e->raw[i] = raw ? raw[i] : 0;
	e->serial = serial;
	p25_alias_trace.count = serial;
}
#else
#define trace_unit(c, u, raw, valid) ((void)0)
#endif
#if CONFIG_P25_ALIAS
// TIA-102.AABF-D-2 7.3.35/36. Only pair A followed by B of the same conveyance,
// after learning the current caller. No alias is carried between calls or callers.
static bool alias_lc(p25_call_rx_t *c, const uint8_t raw[P25_LC_BYTES])
{
	p25_alias_stats_t *s = &c->stats.alias;
	s->lc_other++;
	s->last_src = c->call.have_lc ? c->call.lc.src : 0;
	memcpy(s->last_lc, raw, P25_LC_BYTES);
	if (raw[1] == 0x90 && (raw[0] == 0x15 || raw[0] == 0x17)) {
		if (!c->call.have_lc || !c->call.lc.src)
			s->no_source++;
		// This header names a group. Never reinterpret an identified private call.
		if (c->call.have_lc && c->call.lc.lcf != P25_LCF_GROUP)
			return false;
		s->parts++;
		c->alias90_age = 0;
		char name[15];
		uint32_t recovered_src;
		bool complete = p25_alias90_lc(&c->alias90, raw, c->call.have_lc ? c->call.lc.src : 0,
					     c->call.lc.tgid, name, &recovered_src);
		s->mfid90_src = c->call.have_lc ? c->call.lc.src : 0;
		s->mfid90_result = c->alias90.result;
		s->mfid90_seen = c->alias90.seen;
		s->mfid90_count = c->alias90.count;
		s->mfid90_seq = c->alias90.seq;
		if (c->alias90.result >= P25_ALIAS90_BAD_HEADER)
			s->rejected++;
		if (!complete)
			return false;
		if (!c->call.have_lc || !c->call.lc.src) {
			// The complete alias payload includes its SUID in the CRC check.
			// Do not infer identity from incomplete fragments or a grant alone.
			c->call.lc.lcf = P25_LCF_GROUP;
			c->call.lc.mfid = 0x90;
			c->call.lc.src = recovered_src;
			c->call.lc.tgid = c->alias90.tg;
			c->call.have_lc = true;
		}
		s->complete++;
		s->decoded_src = c->call.lc.src;
		memcpy(s->last_name, name, sizeof name);
		if (!memcmp(c->call.alias, name, sizeof name))
			return false;
		memcpy(c->call.alias, name, sizeof name);
		return true;
	}
	uint8_t op = raw[0] & 0x3F;
	if (op < 0x32 || op > 0x35)
		return false;
	if (raw[1] != 0xA4 || raw[0] != op) {
		s->rejected++;
		c->alias_opcode = 0;
		return false;
	}
	if (!c->call.have_lc || c->call.lc.src == 0) {
		s->no_source++;
		c->alias_opcode = 0;
		return false;
	}
	for (unsigned i = 2; i < P25_LC_BYTES; i++) {
		if (raw[i] && (raw[i] < 0x20 || raw[i] > 0x7E)) {
			s->rejected++;
			c->alias_opcode = 0;
			return false;
		}
	}
	s->parts++;
	if ((raw[0] & 1) == 0) {
		memcpy(c->alias_part, raw + 2, 7);
		c->alias_opcode = raw[0];
		c->alias_age = 0;
		return false;
	}
	bool paired = c->alias_opcode != 0 && raw[0] == c->alias_opcode + 1;
	c->alias_opcode = 0;
	if (!paired) {
		s->unpaired++;
		return false;
	}
	char name[15] = { 0 };
	memcpy(name, c->alias_part, 7);
	memcpy(name + 7, raw + 2, 7);
	bool ended = false;
	for (unsigned i = 0; i < 14; i++) {
		if (!name[i])
			ended = true;
		else if (ended) {
			s->rejected++;
			return false;	// non-null bytes after the padding are malformed
		}
	}
	size_t len = strlen(name);
	while (len && name[len - 1] == ' ')
		name[--len] = 0;
	s->complete++;
	s->decoded_src = c->call.lc.src;
	memcpy(s->last_name, name, sizeof name);
	if (!memcmp(name, c->call.alias, sizeof name))
		return false;
	memcpy(c->call.alias, name, sizeof name);
	return true;
}
#endif

static void call_start(p25_call_rx_t *c, const p25_rx_unit_t *u, bool late)
{
	memset(&c->call, 0, sizeof c->call);
#if CONFIG_P25_ALIAS
	c->alias_opcode = 0;
	memset(&c->alias90, 0, sizeof c->alias90);
#endif
	c->call.nac = u->nac;
	c->call.late = late;
	c->call.algid = P25_ALGID_CLEAR;
	c->call.rssi_dbm = u->rssi_dbm;
	c->active = true;
	c->stats.calls++;
}

static void call_end(p25_call_rx_t *c)
{
	if (!c->active)
		return;
	c->active = false;
#if CONFIG_P25_ALIAS
	c->alias_opcode = 0;
	memset(&c->alias90, 0, sizeof c->alias90);
	memset(c->call.alias, 0, sizeof c->call.alias);
#endif
	if (c->sink->end)
		c->sink->end(c->ctx);
}

static void on_unit(const p25_rx_unit_t *u, void *ctx)
{
	p25_call_rx_t *c = ctx;
	uint8_t buf[P25_HDR_BYTES];
	bool fresh = false, changed = false;

	c->stats.units++;
#if CONFIG_P25_ALIAS
	if (c->alias_opcode && ++c->alias_age > 12)
		c->alias_opcode = 0;
	if (c->alias90.count && ++c->alias90_age > 80)
		memset(&c->alias90, 0, sizeof c->alias90);
#endif
	if (c->active && c->call.nac != u->nac)
		call_end(c);
	if (p25_unit_sys_no_tx(u))
		c->stats.sys_no_tx++;
#if CONFIG_P25_TRUNK
	if (c->sink->unit)
		c->sink->unit(u, c->ctx);
#endif
	switch (u->duid) {
	case P25_DUID_HDU:
		trace_unit(c, u, NULL, 0);
		// A header opens a call, also on top of one whose terminator was missed
		call_end(c);
		call_start(c, u, false);
		fresh = true;
		if (p25_hdu_get(u->air, buf) >= 0) {
			c->call.have_alg = true;
			c->call.algid = buf[10];
			c->call.kid = (uint16_t)(buf[11] << 8 | buf[12]);
			c->call.lc.mfid = buf[9];
			c->call.lc.tgid = (uint16_t)(buf[13] << 8 | buf[14]);
		}
		break;
	case P25_DUID_LDU1:
	case P25_DUID_LDU2:
		if (!c->active) {
			call_start(c, u, true);
			fresh = true;
		}
		if (u->duid == P25_DUID_LDU1) {
			uint8_t raw[P25_LC_BYTES];
			p25_lc_t lc;

			int lc_result = p25_ldu_lc_get(u->air, raw);
			trace_unit(c, u, lc_result >= 0 ? raw : NULL, lc_result >= 0 ? 1 : -1);
			if (lc_result < 0) {
				c->stats.lc_fail++;
#if CONFIG_P25_ALIAS
				c->alias_opcode = 0;
#endif
			} else if (p25_lc_parse(raw, &lc)) {
#if CONFIG_P25_ALIAS
				if (!c->call.have_lc || lc.src != c->call.lc.src || lc.lcf != c->call.lc.lcf ||
				    lc.dst != c->call.lc.dst || lc.tgid != c->call.lc.tgid) {
					c->alias_opcode = 0;
					// Learning the first source is not a caller change. Keep pending vendor
					// fragments for this group; their CRC-checked SUID must still match lc.src.
					if ((c->call.have_lc && c->call.lc.src) || lc.lcf != P25_LCF_GROUP ||
					    (c->alias90.tg && c->alias90.tg != lc.tgid))
						memset(&c->alias90, 0, sizeof c->alias90);
					memset(c->call.alias, 0, sizeof c->call.alias);
				}
#endif
				if (!c->call.have_lc || memcmp(&lc, &c->call.lc, sizeof lc))
					changed = true;
				c->call.lc = lc;
				c->call.have_lc = true;
#if CONFIG_P25_ALIAS
			} else if (alias_lc(c, raw)) {
				changed = true;
#endif
			}
		} else {
			trace_unit(c, u, NULL, 0);
			uint8_t es[P25_ES_BYTES];

			if (p25_ldu_es_get(u->air, es) < 0) {
				c->stats.es_fail++;
			} else {
				uint16_t kid = (uint16_t)(es[10] << 8 | es[11]);

				if (!c->call.have_alg || es[9] != c->call.algid || kid != c->call.kid)
					changed = true;
				c->call.have_alg = true;
				c->call.algid = es[9];
				c->call.kid = kid;
			}
		}
		break;
	case P25_DUID_TDULC:
#if CONFIG_P25_ALIAS
		if (u->air && u->nsyms >= P25_TDULC_SYMS) {
			uint8_t raw[P25_LC_BYTES];
			if (p25_tdulc_get(u->air, raw) >= 0) {
				trace_unit(c, u, raw, 1);
				if (p25_alias90_word(raw)) {
					if (!c->active) {
						if (raw[0] != 0x15 || !raw[4] || raw[4] > 16 || raw[5] != 1 ||
						    !(raw[2] || raw[3])) return;
						call_start(c, u, true);
						c->call.algid = 0;
						if (c->sink->start) c->sink->start(&c->call, c->ctx);
					}
					if (alias_lc(c, raw) && c->sink->update)
						c->sink->update(&c->call, c->ctx);
					return;	// alias data is not a voice frame or the end of its sequence
				}

			} else {
				trace_unit(c, u, NULL, -1);
			}
		}
#endif
		call_end(c);
		return;
	case P25_DUID_TDU:
		trace_unit(c, u, NULL, 0);
		call_end(c);
		return;
	default:
		return;
	}
	if (u->rssi_dbm)
		c->call.rssi_dbm = u->rssi_dbm;
	if (fresh) {
		if (c->sink->start)
			c->sink->start(&c->call, c->ctx);
	} else if (changed && c->sink->update) {
		c->sink->update(&c->call, c->ctx);
	}
	if (u->duid == P25_DUID_HDU)
		return;
	// Encrypted voice decodes to noise: it stays here. (A late entry learns the algorithm with its first LDU2;
	// an LDU1 before that is taken as clear, as a call without encryption sync is)
	if (c->call.algid != P25_ALGID_CLEAR)
		return;
	for (uint8_t n = 0; n < P25_VOICE_FRAMES; n++) {
		uint8_t imbe[P25_IMBE_BYTES];
		int errs = p25_ldu_voice_get(u->air, n, imbe);

		c->stats.voice++;
		c->stats.voice_bits += (uint32_t)P25_VOICE_ERRS_TOTAL(errs);
		if (P25_VOICE_ERRS_BAD(errs))
			c->stats.voice_bad++;
		if (c->sink->voice)
			c->sink->voice(imbe, (uint8_t)errs, c->ctx);
	}
}

static void on_lost(void *ctx)
{
	trace_unit(ctx, NULL, NULL, 0);
	call_end(ctx);
}

void p25_call_rx_init(p25_call_rx_t *c, uint16_t nac, const p25_call_sink_t *sink, void *ctx)
{
	memset(c, 0, sizeof *c);
	c->sink = sink;
	c->ctx = ctx;
	// (on_unit's context is c; the framer's is c->rx, the same address)
	p25_rx_init(&c->rx, nac, on_unit, on_lost, c);
}

// ── Transmit ──
enum { TX_HDU, TX_LDU1, TX_LDU2, TX_TERM, TX_DONE };

static void tx_unit(p25_call_tx_t *t, uint8_t state)
{
	t->state = state;
	t->sent = 0;
	t->nvoice = 0;
	if (state == TX_HDU) {
		uint8_t hdr[P25_HDR_BYTES] = { 0 };	// no message indicator: clear voice

		hdr[10] = P25_ALGID_CLEAR;
		hdr[13] = (uint8_t)(t->tgid >> 8);
		hdr[14] = (uint8_t)t->tgid;
		t->nsyms = t->ready = P25_HDU_SYMS;
		p25_unit_begin(t->unit, t->nsyms, t->nac, P25_DUID_HDU, t->ss);
		p25_hdu_put(t->unit, hdr);
	} else if (state == TX_LDU1 || state == TX_LDU2) {
		static const uint8_t lsd[2] = { 0, 0 };

		// Nothing of an LDU goes out before its first voice frame: an end that comes first sends the terminator
		// right after the LDU before (8.2.2), not an LDU of silence
		t->nsyms = P25_LDU_SYMS;
		t->ready = 0;
		p25_unit_begin(t->unit, t->nsyms, t->nac, state == TX_LDU1 ? P25_DUID_LDU1 : P25_DUID_LDU2, t->ss);
		if (state == TX_LDU1) {
			p25_ldu_lc_put(t->unit, t->lc);
		} else {
			uint8_t es[P25_ES_BYTES] = { 0 };

			es[9] = P25_ALGID_CLEAR;
			p25_ldu_es_put(t->unit, es);
		}
		p25_ldu_lsd_put(t->unit, lsd);
	} else if (state == TX_TERM) {
		t->nsyms = t->ready = P25_TDU_SYMS;
		p25_unit_begin(t->unit, t->nsyms, t->nac, P25_DUID_TDU, t->ss);
	} else {
		t->nsyms = t->ready = 0;
	}
}

void p25_call_tx_begin(p25_call_tx_t *t, uint16_t nac, const p25_lc_t *lc, uint8_t ss)
{
	memset(t, 0, sizeof *t);
	t->nac = nac;
	t->ss = ss;
	t->tgid = lc->lcf == P25_LCF_GROUP ? lc->tgid : 0;
	p25_lc_build(lc, t->lc);
	tx_unit(t, TX_HDU);
}

uint16_t p25_call_tx_ready(const p25_call_tx_t *t, uint16_t *first)
{
	*first = t->sent;
	return (uint16_t)(t->ready - t->sent);
}

void p25_call_tx_sent(p25_call_tx_t *t, uint16_t n)
{
	t->sent = (uint16_t)(t->sent + n);
	if (t->sent < t->nsyms || t->state == TX_DONE)
		return;
	switch (t->state) {
	case TX_HDU:
		tx_unit(t, t->ending ? TX_TERM : TX_LDU1);
		break;
	case TX_LDU1:
		// A terminator may follow either LDU (8.2.2)
		tx_unit(t, t->ending ? TX_TERM : TX_LDU2);
		break;
	case TX_LDU2:
		tx_unit(t, t->ending ? TX_TERM : TX_LDU1);
		break;
	default:
		tx_unit(t, TX_DONE);
		break;
	}
}

bool p25_call_tx_wants_voice(const p25_call_tx_t *t)
{
	return (t->state == TX_LDU1 || t->state == TX_LDU2) && t->nvoice < P25_VOICE_FRAMES;
}

void p25_call_tx_voice(p25_call_tx_t *t, const uint8_t imbe[P25_IMBE_BYTES])
{
	if (!p25_call_tx_wants_voice(t))
		return;
	p25_ldu_voice_put(t->unit, t->nvoice, imbe);
	t->nvoice++;
	t->ready = p25_ldu_voice_sym(t->nvoice);
}

void p25_call_tx_end(p25_call_tx_t *t, const uint8_t silence[P25_IMBE_BYTES])
{
	t->ending = true;
	// An LDU without a voice frame yet has nothing of it on air: not sent at all
	if ((t->state == TX_LDU1 || t->state == TX_LDU2) && t->nvoice == 0) {
		tx_unit(t, TX_TERM);
		return;
	}
	while (p25_call_tx_wants_voice(t))
		p25_call_tx_voice(t, silence);
}

bool p25_call_tx_done(const p25_call_tx_t *t)
{
	return t->state == TX_DONE;
}

#if CONFIG_P25_TRUNK
void p25_call_tx_single(p25_call_tx_t *t, uint16_t nsyms)
{
	// As a terminator: the unit goes out whole, then the transmission is done
	t->state = TX_TERM;
	t->ending = true;
	t->nvoice = 0;
	t->nsyms = t->ready = nsyms;
	t->sent = 0;
}
#endif
