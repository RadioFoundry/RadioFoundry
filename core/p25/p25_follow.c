// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The receive-only P25 trunking follower (p25_follow.h, docs/spec/p25-trunking.md 4).
#include "../config.h"
#if CONFIG_P25_TRUNK	// the whole file
#include "p25_follow.h"

#include <string.h>

// It sits in memory the caller places, with the engine; the tightest radio lends both from its vocoder
_Static_assert(sizeof(p25_follow_t) <= 400, "the trunking follower grew past its budget");

#define IDEN_VALID	0x01
#define IDEN_PROG	0x02	// programmed: never replaced off air
#define IDEN_TDMA	0x04

enum { PH_DWELL, PH_IDENT, PH_BACKOFF };
// Why a voice channel is left
enum { LEAVE_TERM, LEAVE_ENC, LEAVE_OTHER };

static bool elapsed(uint32_t now, uint32_t since, uint32_t ms)
{
	return (uint32_t)(now - since) >= ms;
}

static void count(uint16_t *c)
{
	if (*c != UINT16_MAX)
		(*c)++;
}

static void tune(p25_follow_t *f, uint32_t hz, uint8_t what, uint32_t id)
{
	f->tuning = true;
	if (f->hooks->tune)
		f->hooks->tune(hz, what, id, f->ctx);
}

// ── Identifiers and channels ──
static uint32_t chan_hz(const p25_follow_t *f, uint16_t ch)
{
	const p25_follow_iden_e *i = &f->iden[ch >> 12];

	if (!(i->flags & IDEN_VALID))
		return 0;
	return p25_chan_hz(ch, i->base_hz, i->spacing5, i->slots);
}

static void iden_learn(p25_follow_t *f, const p25_iden_air_t *a)
{
	p25_follow_iden_e *i = &f->iden[a->id & 15];
	uint32_t spacing5 = (uint32_t)a->spacing * 25u;

	if ((f->cfg->flags & P25_FOLLOW_PLAN_ONLY) || (i->flags & IDEN_PROG))
		return;
	// The base is in 5 Hz steps: one past 32 bits of Hz, or none, is not a channel plan
	if (!a->base || a->base > UINT32_MAX / 5 || !spacing5)
		return;
	i->base_hz = a->base * 5u;
	i->spacing5 = (uint16_t)spacing5;
	i->slots = a->slots ? a->slots : 1;
	i->flags = (uint8_t)(IDEN_VALID | (a->tdma ? IDEN_TDMA : 0));
	f->view.idens |= (uint16_t)(1u << (a->id & 15));
}

// ── The control channel list: the programmed ones, then the ones learned off air ──
static uint8_t cc_count(const p25_follow_t *f)
{
	return (uint8_t)(f->cfg->ncc + f->nlearn);
}

static uint32_t cc_hz(const p25_follow_t *f, uint8_t i)
{
	if (i < f->cfg->ncc) {
		const p25_follow_cc_t *c = &f->cfg->cc[i];

		return c->hz ? c->hz : c->chan ? chan_hz(f, c->chan) : 0;
	}
	return chan_hz(f, f->learn[i - f->cfg->ncc]);
}

static void cc_learn(p25_follow_t *f, uint16_t ch)
{
	if (!ch)
		return;
	for (uint8_t i = 0; i < f->cfg->ncc; i++)
		if (f->cfg->cc[i].chan == ch)
			return;
	for (uint8_t i = 0; i < f->nlearn; i++)
		if (f->learn[i] == ch)
			return;
	if (f->nlearn < P25_FOLLOW_LEARN) {
		f->learn[f->nlearn++] = ch;
		f->view.learned = f->nlearn;
	}
}

// ── Hunt ──
static void hunt_try(p25_follow_t *f, uint32_t now);

static void hunt_next(p25_follow_t *f, uint32_t now)
{
	uint8_t n = cc_count(f);

	f->tried++;
	if (!n)
		return;
	f->view.cc = (uint8_t)(f->view.cc + 1 >= n ? 0 : f->view.cc + 1);
	hunt_try(f, now);
}

// Tune the candidate view.cc; one with no frequency yet is passed over. A whole pass without an answer: wait on
// the last good one (or the first) a while, longer each pass, then try them all again
static void hunt_try(p25_follow_t *f, uint32_t now)
{
	uint8_t n = cc_count(f);

	f->have_id = false;
	f->t_state = now;
	while (f->tried < n) {
		uint32_t hz = cc_hz(f, f->view.cc);

		if (hz) {
			f->phase = PH_DWELL;
			f->view.cc_hz = hz;
			tune(f, hz, P25_FOLLOW_TUNE_CONTROL, 0);
			return;
		}
		f->tried++;
		f->view.cc = (uint8_t)(f->view.cc + 1 >= n ? 0 : f->view.cc + 1);
	}
	f->phase = PH_BACKOFF;
	f->tried = 0;
	f->view.cc = f->good < n ? f->good : 0;
	f->view.cc_hz = n ? cc_hz(f, f->view.cc) : 0;
	if (f->view.cc_hz)
		tune(f, f->view.cc_hz, P25_FOLLOW_TUNE_CONTROL, 0);
}

static void hunt_start(p25_follow_t *f, uint32_t now)
{
	f->view.state = P25_FOLLOW_HUNT;
	count(&f->view.hunts);
	f->t_nocc = now;
	f->tried = 0;
	f->backoff = P25_FOLLOW_BACKOFF_MS;
	f->view.cc = f->good < cc_count(f) ? f->good : 0;
	hunt_try(f, now);
}

static void accept(p25_follow_t *f, uint16_t nac, uint32_t now)
{
	f->view.state = P25_FOLLOW_CONTROL;
	f->view.nac = nac;
	f->view.fallback = false;
	f->good = f->view.cc;
	f->t_good = now;
}

static bool any_wacn(const p25_follow_cfg_t *c) { return c->wacn > 0xFFFFFu; }
static bool any_sysid(const p25_follow_cfg_t *c) { return c->sysid > 0xFFFu; }

// ── Following ──
static bool group_wanted(const p25_follow_t *f, uint16_t tg, uint32_t now)
{
	const p25_follow_cfg_t *c = f->cfg;

	if (!tg)
		return false;
	if (f->hold_tg && c->hold_ms && !elapsed(now, f->t_hold, c->hold_ms))
		return tg == f->hold_tg;
	if ((c->flags & P25_FOLLOW_ALL_GROUPS) || tg == c->talk_group)
		return true;
	for (uint8_t i = 0; i < c->ngroups; i++)
		if (c->groups[i] == tg)
			return true;
	return false;
}

static void refuse(p25_follow_t *f, uint8_t why, uint16_t tg)
{
	f->view.refused = why;
	f->view.refused_tg = tg;
}

// One grant (a group's or a unit to unit call's) on channel ch: followed if wanted and playable. True if followed
static bool consider(p25_follow_t *f, uint8_t op, bool unit, uint16_t ch, uint16_t tg, uint32_t dst, uint32_t src,
		     bool has_svc, uint8_t svc, uint32_t now)
{
	const p25_follow_cfg_t *c = f->cfg;
	const p25_follow_iden_e *i = &f->iden[ch >> 12];
	bool update = op != P25_OP_GRP_V_GRANT && op != P25_OP_UU_V_GRANT;
	uint32_t hz;

	if (unit) {
		if ((c->flags & P25_FOLLOW_NO_UNIT) || !c->unit_id || (dst != c->unit_id && src != c->unit_id))
			return false;
	} else if (!group_wanted(f, tg, now)) {
		return false;
	}
	// The call just left on its terminator: its updates go on a little while. An encrypted call skipped: its grants
	// repeated and its updates (which carry no service options) are skipped for as long as they come
	if ((update || f->left_enc) && ch == f->left_ch && tg == f->left_tg && !elapsed(now, f->t_left, P25_FOLLOW_REJOIN_MS)) {
		if (f->left_enc)
			f->t_left = now;
		return false;
	}
	if (has_svc && (svc & P25_SVC_PROTECTED) && (c->flags & P25_FOLLOW_SKIP_ENC)) {
		count(&f->view.enc_skip);
		refuse(f, P25_REFUSE_ENC, tg);
		f->left_ch = ch;
		f->left_tg = tg;
		f->left_enc = true;
		f->t_left = now;
		return false;
	}
	if (!(i->flags & IDEN_VALID) || !(hz = chan_hz(f, ch))) {
		count(&f->view.no_iden);
		refuse(f, P25_REFUSE_NO_IDEN, tg);
		return false;
	}
	if (i->flags & IDEN_TDMA) {
		count(&f->view.tdma);
		refuse(f, P25_REFUSE_TDMA, tg);
		return false;
	}
	f->view.state = P25_FOLLOW_VOICE;
	f->view.voice_hz = hz;
	f->view.unit_call = unit;
	f->view.enc = has_svc && (svc & P25_SVC_PROTECTED);
	f->view.tg = unit ? 0 : tg;
	f->view.dst = unit ? dst : 0;
	f->view.src = src;
	f->left_ch = ch;
	f->t_call = f->t_good = now;
	count(&f->view.follows);
	tune(f, hz, unit ? P25_FOLLOW_TUNE_UNIT : P25_FOLLOW_TUNE_GROUP, unit ? dst : tg);
	return true;
}

// Back to the control channel held. why: LEAVE_*; a call that ended on its terminator, or was found encrypted, is
// not joined again on its updates
static void leave(p25_follow_t *f, uint8_t why, uint32_t now)
{
	bool keep = why == LEAVE_TERM || why == LEAVE_ENC;

	if (!f->view.unit_call && f->cfg->hold_ms && why != LEAVE_ENC) {
		f->hold_tg = f->view.tg;
		f->t_hold = now;
	}
	f->left_tg = keep ? f->view.tg : 0;
	f->left_ch = keep ? f->left_ch : 0;
	f->left_enc = why == LEAVE_ENC;
	f->t_left = now;
	f->view.cc = f->good;
	f->view.cc_hz = cc_hz(f, f->good);
	if (!f->view.cc_hz) {
		// Held without knowing its frequency (taken while waiting on a candidate that had none): look again
		hunt_start(f, now);
		return;
	}
	f->view.state = P25_FOLLOW_CONTROL;
	f->t_good = now;	// the control channel gets its full time to show up again
	tune(f, f->view.cc_hz, P25_FOLLOW_TUNE_CONTROL, 0);
}

// ── Blocks ──
static void on_block(p25_follow_t *f, const uint8_t blk[P25_TSBK_BYTES], uint16_t nac, uint32_t now)
{
	const p25_follow_cfg_t *c = f->cfg;
	uint8_t st = f->view.state;
	p25_tsbk_t m;
	bool known = p25_tsbk_parse(blk, &m);

	if (st != P25_FOLLOW_HUNT && st != P25_FOLLOW_CONTROL)
		return;
	if (c->nac != P25_NAC_ANY && nac != c->nac) {
		// Another system on the frequency
		count(&f->view.wrong_sys);
		if (st == P25_FOLLOW_HUNT && f->phase != PH_BACKOFF)
			hunt_next(f, now);
		return;
	}
	if (m.kind == P25_TSBK_MFR) {
		count(&f->view.mfr);
	} else if (f->view.op[m.opcode] != UINT8_MAX) {
		f->view.op[m.opcode]++;
	}
	if (st == P25_FOLLOW_HUNT) {
		bool need = !any_wacn(c) || !any_sysid(c);

		// A system identity: it decides the candidate
		if (known && (m.kind == P25_TSBK_NET || (m.kind == P25_TSBK_RFSS && any_wacn(c)))) {
			uint16_t sysid = m.kind == P25_TSBK_NET ? m.u.net.sysid : m.u.site.sysid;
			bool ok = (any_sysid(c) || sysid == c->sysid) && (m.kind != P25_TSBK_NET || any_wacn(c) || m.u.net.wacn == c->wacn);

			if (!ok) {
				count(&f->view.wrong_sys);
				if (f->phase != PH_BACKOFF)
					hunt_next(f, now);
				return;
			}
			f->have_id = true;
		}
		if (need && !f->have_id) {
			if (f->phase == PH_DWELL) {
				f->phase = PH_IDENT;
				f->t_state = now;
			}
			return;
		}
		accept(f, nac, now);
	}
	f->t_good = now;
	if (!known)
		return;
	switch (m.kind) {
	case P25_TSBK_IDEN:
		iden_learn(f, &m.u.iden);
		break;
	case P25_TSBK_NET:
		if ((!any_wacn(c) && m.u.net.wacn != c->wacn) || (!any_sysid(c) && m.u.net.sysid != c->sysid)) {
			// The channel held now speaks for another system
			count(&f->view.wrong_sys);
			f->good = (uint8_t)(f->view.cc + 1 >= cc_count(f) ? 0 : f->view.cc + 1);
			hunt_start(f, now);
			return;
		}
		f->view.wacn = m.u.net.wacn;
		f->view.sysid = m.u.net.sysid;
		break;
	case P25_TSBK_RFSS:
		f->view.sysid = m.u.site.sysid;
		f->view.rfss = m.u.site.rfss;
		f->view.site = m.u.site.site;
		break;
	case P25_TSBK_ADJ:
		if (any_sysid(c) || m.u.site.sysid == c->sysid)
			cc_learn(f, m.u.site.ch);
		break;
	case P25_TSBK_SCCB:
		cc_learn(f, m.u.sccb.ch[0]);
		cc_learn(f, m.u.sccb.ch[1]);
		break;
	case P25_TSBK_GRANT: {
		const p25_grant_t *g = &m.u.grant;

		count(&f->view.grants);
		if (!consider(f, m.opcode, g->unit, g->ch, g->tg, g->dst, g->src, g->has_svc, g->svc, now) && g->ch2 &&
		    m.opcode == P25_OP_GRP_V_UPDT)
			consider(f, m.opcode, false, g->ch2, g->tg2, 0, 0, false, 0, now);
		break;
	}
	default:
		break;
	}
}

// ── Entry points ──
static void view_begin(p25_follow_t *f) { f->seq++; }
static void view_end(p25_follow_t *f) { f->seq++; }

void p25_follow_begin(p25_follow_t *f, const p25_follow_cfg_t *cfg, const p25_follow_hooks_t *hooks, void *ctx,
		      uint32_t now)
{
	memset(f, 0, sizeof *f);
	f->cfg = cfg;
	f->hooks = hooks;
	f->ctx = ctx;
	f->good = 0xFF;
	f->view.wacn = UINT32_MAX;
	f->view.sysid = UINT16_MAX;
	f->view.rfss = f->view.site = 0xFF;
	for (uint8_t n = 0; cfg->iden && n < P25_FOLLOW_IDENS; n++) {
		const p25_follow_iden_t *p = &cfg->iden[n];
		p25_follow_iden_e *i = &f->iden[n];

		if (!(cfg->iden_set & (1u << n)) || !p->base_hz || p->spacing_hz % 5 || !p->spacing_hz ||
		    p->spacing_hz / 5 > UINT16_MAX)
			continue;
		i->base_hz = p->base_hz;
		i->spacing5 = (uint16_t)(p->spacing_hz / 5);
		i->slots = p->slots > 1 ? p->slots : 1;
		i->flags = (uint8_t)(IDEN_VALID | IDEN_PROG | (p->tdma ? IDEN_TDMA : 0));
		f->view.idens |= (uint16_t)(1u << n);
	}
	f->seq = 1;
	hunt_start(f, now);
	f->seq = 2;
}

void p25_follow_end(p25_follow_t *f)
{
	view_begin(f);
	f->view.state = P25_FOLLOW_OFF;
	view_end(f);
}

void p25_follow_retuned(p25_follow_t *f, uint32_t now)
{
	view_begin(f);
	f->tuning = false;
	// Timers run from the moment the receiver is there
	f->t_state = now;
	if (f->view.state == P25_FOLLOW_VOICE)
		f->t_call = f->t_good = now;
	else if (f->view.state == P25_FOLLOW_CONTROL)
		f->t_good = now;
	view_end(f);
}

void p25_follow_tsbk(p25_follow_t *f, const uint8_t blk[P25_TSBK_BYTES], uint16_t nac, uint32_t now)
{
	if (f->tuning || f->view.state == P25_FOLLOW_OFF)
		return;
	view_begin(f);
	f->view.blocks++;
	on_block(f, blk, nac, now);
	view_end(f);
}

void p25_follow_unit(p25_follow_t *f, const p25_rx_unit_t *u, uint32_t now)
{
	uint8_t blk[P25_TSBK_BYTES];

	if (f->tuning || f->view.state == P25_FOLLOW_OFF)
		return;
	view_begin(f);
	switch (u->duid) {
	case P25_DUID_TSDU: {
		uint8_t n = u->nsyms <= p25_blocks_syms(1) ? 1 : u->nsyms <= p25_blocks_syms(2) ? 2 : 3;

		for (uint8_t k = 0; k < n && !f->tuning; k++) {
			if (p25_block_get(u->air, k, blk) < 0) {
				f->view.crc_fail++;
				continue;
			}
			f->view.blocks++;
			on_block(f, blk, u->nac, now);
		}
		break;
	}
	case P25_DUID_PDU: {
		p25_mbt_t h;

		if (f->view.state == P25_FOLLOW_VOICE)
			break;
		if (p25_block_get(u->air, 0, blk) < 0) {
			f->view.crc_fail++;
		} else if (p25_mbt_parse(blk, &h)) {
			// Recognized and counted; its data blocks are not decoded
			count(&f->view.mbt);
			f->view.mbt_op = h.opcode;
		} else {
			count(&f->view.pdu_other);
		}
		break;
	}
	case P25_DUID_HDU:
	case P25_DUID_LDU1:
	case P25_DUID_LDU2:
		if (f->view.state == P25_FOLLOW_VOICE)
			f->t_good = now;
		break;
	case P25_DUID_TDU:
	case P25_DUID_TDULC:
		if (f->view.state == P25_FOLLOW_VOICE)
			leave(f, LEAVE_TERM, now);
		break;
	default:
		break;
	}
	view_end(f);
}

void p25_follow_call(p25_follow_t *f, const p25_call_t *c, uint32_t now)
{
	if (f->tuning || f->view.state != P25_FOLLOW_VOICE)
		return;
	view_begin(f);
	if (c->have_lc && c->lc.lcf == P25_LCF_GROUP && !f->view.unit_call && c->lc.tgid != f->view.tg) {
		// Not the call granted (the channel was given to another one meanwhile)
		count(&f->view.wrong_call);
		leave(f, LEAVE_OTHER, now);
	} else if (c->have_alg && c->algid != P25_ALGID_CLEAR) {
		f->view.enc = true;
		if (f->cfg->flags & P25_FOLLOW_SKIP_ENC) {
			count(&f->view.enc_skip);
			refuse(f, P25_REFUSE_ENC, f->view.tg);
			leave(f, LEAVE_ENC, now);
		}
	} else if (c->have_lc && !f->view.src) {
		f->view.src = c->lc.src;
	}
	view_end(f);
}

void p25_follow_tick(p25_follow_t *f, uint32_t now)
{
	const p25_follow_cfg_t *c = f->cfg;

	if (f->tuning || f->view.state == P25_FOLLOW_OFF)
		return;
	view_begin(f);
	switch (f->view.state) {
	case P25_FOLLOW_HUNT:
		if (c->fallback_ms && !f->view.fallback && elapsed(now, f->t_nocc, c->fallback_ms)) {
			f->view.fallback = true;
			if (f->hooks->fallback)
				f->hooks->fallback(f->ctx);
		}
		if (f->phase == PH_DWELL && elapsed(now, f->t_state, P25_FOLLOW_DWELL_MS)) {
			hunt_next(f, now);
		} else if (f->phase == PH_IDENT && elapsed(now, f->t_state, P25_FOLLOW_ID_MS)) {
			hunt_next(f, now);
		} else if (f->phase == PH_BACKOFF && elapsed(now, f->t_state, f->backoff)) {
			f->backoff = f->backoff * 2 > P25_FOLLOW_BACKOFF_MAX ? P25_FOLLOW_BACKOFF_MAX : f->backoff * 2;
			f->tried = 0;
			hunt_try(f, now);
		}
		break;
	case P25_FOLLOW_CONTROL:
		if (elapsed(now, f->t_good, P25_FOLLOW_CC_LOST_MS)) {
			count(&f->view.cc_lost);
			hunt_start(f, now);
		}
		break;
	case P25_FOLLOW_VOICE:
		if (elapsed(now, f->t_good, c->voice_lost_ms ? c->voice_lost_ms : P25_FOLLOW_VOICE_LOST_MS)) {
			count(&f->view.voice_lost);
			leave(f, LEAVE_OTHER, now);
		} else if (elapsed(now, f->t_call, c->call_max_ms ? c->call_max_ms : P25_FOLLOW_CALL_MAX_MS)) {
			leave(f, LEAVE_OTHER, now);
		}
		break;
	default:
		break;
	}
	view_end(f);
}

void p25_follow_view(const p25_follow_t *f, p25_follow_view_t *out)
{
	uint8_t seq;

	do {
		seq = f->seq;
		*out = f->view;
	} while ((seq & 1) || seq != f->seq);
}

#endif
