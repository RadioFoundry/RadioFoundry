// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The P25 trunking follower (p25_follow.h, docs/spec/p25-trunking.md 4) and its transmit side (5).
#include "../config.h"
#if CONFIG_P25_TRUNK	// the whole file
#include "p25_follow.h"

#include <string.h>

// It sits in memory the caller places, with the engine; the tightest radio lends both from its vocoder
_Static_assert(sizeof(p25_follow_t) <= 480, "the trunking follower grew past its budget");

#define IDEN_VALID	0x01
#define IDEN_PROG	0x02	// programmed: never replaced off air
#define IDEN_TDMA	0x04
#define IDEN_OFF	0x08	// the transmit offset is known
#define IDEN_OFF_STEP	0x10	// ... in channel spacings, else in 250 kHz units

#define NO_OP		0xFF	// acc.pend: nothing waiting

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

static void count8(uint8_t *c)
{
	if (*c != UINT8_MAX)
		(*c)++;
}

static void tune(p25_follow_t *f, uint32_t hz, uint8_t what, uint32_t id)
{
	f->tuning = true;
	f->acc.heard = false;
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

// The subscriber's transmit frequency for a channel whose receive frequency is rx: 0 when the offset is not known
static uint32_t chan_tx_hz(const p25_follow_t *f, uint16_t ch, uint32_t rx)
{
	const p25_follow_iden_e *i = &f->iden[ch >> 12];
	uint32_t mag = (uint32_t)(i->tx_off < 0 ? -(int32_t)i->tx_off : i->tx_off);
	uint32_t unit = (i->flags & IDEN_OFF_STEP) ? i->spacing5 * 5u : 250000u;

	if (!rx || !(i->flags & IDEN_VALID) || !(i->flags & IDEN_OFF) || !unit || mag > UINT32_MAX / unit)
		return 0;
	mag *= unit;
	if (i->tx_off < 0)
		return mag < rx ? rx - mag : 0;
	return mag <= UINT32_MAX - rx ? rx + mag : 0;
}

static void iden_learn(p25_follow_t *f, const p25_iden_air_t *a)
{
	p25_follow_iden_e *i = &f->iden[a->id & 15];
	uint32_t spacing5 = (uint32_t)a->spacing * 25u;

	if (f->cfg->flags & P25_FOLLOW_PLAN_ONLY)
		return;
	if (i->flags & IDEN_PROG) {
		// A programmed identifier without a transmit offset takes the one off air (only that)
		if (!(i->flags & IDEN_OFF)) {
			i->tx_off = a->tx_off;
			i->flags |= (uint8_t)(IDEN_OFF | (a->form == P25_OP_IDEN_UP ? 0 : IDEN_OFF_STEP));
		}
		return;
	}
	// The base is in 5 Hz steps: one past 32 bits of Hz, or none, is not a channel plan
	if (!a->base || a->base > UINT32_MAX / 5 || !spacing5)
		return;
	i->base_hz = a->base * 5u;
	i->spacing5 = (uint16_t)spacing5;
	i->slots = a->slots ? a->slots : 1;
	// The transmit offset: the general form's unit is 250 kHz, the VHF/UHF and TDMA forms' the channel spacing
	// (unconfirmed, docs/spec/p25-trunking.md 9)
	i->tx_off = a->tx_off;
	i->flags = (uint8_t)(IDEN_VALID | IDEN_OFF | (a->form == P25_OP_IDEN_UP ? 0 : IDEN_OFF_STEP) |
			     (a->tdma ? IDEN_TDMA : 0));
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

// The inbound frequency of control channel i: programmed, else its receive frequency plus its identifier's offset (its
// channel number programmed, learned, or the one its own broadcasts give)
static uint32_t cc_tx_hz(const p25_follow_t *f, uint8_t i)
{
	uint16_t ch = 0;

	if (i < f->cfg->ncc) {
		if (f->cfg->cc[i].tx_hz)
			return f->cfg->cc[i].tx_hz;
		ch = f->cfg->cc[i].chan;
	} else if (i < cc_count(f)) {
		ch = f->learn[i - f->cfg->ncc];
	}
	if (!ch)
		ch = f->acc.cc_chan;
	return ch ? chan_tx_hz(f, ch, cc_hz(f, i)) : 0;
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

static void acc_lost(p25_follow_t *f);

static void hunt_start(p25_follow_t *f, uint32_t now)
{
	acc_lost(f);
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
	f->acc.t_cc = now;
	f->acc.heard = true;
	if (f->view.reg == P25_REG_NO_TX)
		f->view.reg = P25_REG_IDLE;	// this one may have an inbound frequency
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
	} else if (src && src == c->unit_id) {
		// A group call that names us as its talker is not one to listen to: the site repeating the grant of a call
		// we don't hold (its answer to our request is acc_grant's), or another unit with our identity
		return false;
	}
	// The call just left on its terminator: its updates go on a little while, and so do the grants of our own call
	// (we talked, the site repeats them). An encrypted call skipped: its grants repeated and its updates (which carry
	// no service options) are skipped for as long as they come
	if ((update || f->left_enc || (src && src == c->unit_id)) && ch == f->left_ch && tg == f->left_tg && !elapsed(now, f->t_left, P25_FOLLOW_REJOIN_MS)) {
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


// ── The transmit side: registration, affiliation, voice requests (docs/spec/p25-trunking.md 5) ──
static bool tx_on(const p25_follow_t *f)
{
	return (f->cfg->flags & P25_FOLLOW_TX) && f->cfg->unit_id && !f->view.unverified;
}

static uint32_t acc_rand(p25_follow_t *f, uint32_t span)
{
	uint32_t r = f->acc.rng;

	r ^= r << 13;
	r ^= r >> 17;
	r ^= r << 5;
	f->acc.rng = r;
	return span ? r % span : 0;
}

static void acc_queue(p25_follow_t *f, uint8_t op, uint32_t now, uint32_t wait)
{
	f->acc.pend = op;
	f->acc.tries = 0;
	f->acc.sent = false;
	f->acc.t_next = now + wait;
}

static void acc_drop(p25_follow_t *f)
{
	f->acc.pend = NO_OP;
	f->acc.sent = false;
}

// A blocked system (p25_wacn_no_tx, p25_sysid_no_tx): the transmit side goes silent until p25_follow_begin, whatever
// is heard or answered later
static void sys_block(p25_follow_t *f)
{
	f->acc.blocked = true;
	f->view.sys_ok = false;
	f->view.reg = P25_REG_BLOCKED;
	if (f->view.req == P25_REQ_WAIT || f->view.req == P25_REQ_QUEUED)
		f->view.req = P25_REQ_IDLE;
	acc_drop(f);
}

static void denied(p25_follow_t *f, uint8_t svc, uint8_t reason)
{
	f->view.deny_svc = svc;
	f->view.deny_reason = reason;
	count8(&f->view.denials);
}

static void reg_start(p25_follow_t *f, uint32_t now)
{
	p25_follow_acc_t *a = &f->acc;

	if (f->view.req == P25_REQ_WAIT || f->view.req == P25_REQ_QUEUED)
		f->view.req = P25_REQ_TIMEOUT;	// (registration takes the inbound side: a request in hand is dropped)
	f->view.reg = P25_REG_WAIT;
	f->view.aff_tg = 0;
	a->reg_sysid = f->view.sysid;
	a->reg_rfss = f->view.rfss;
	a->reg_site = f->view.site;
	acc_queue(f, P25_ISP_U_REG_REQ, now, acc_rand(f, 2 * P25_ACC_GAP_MS));
}

static void aff_start(p25_follow_t *f, uint32_t now)
{
	if (!f->cfg->talk_group) {
		f->view.reg = P25_REG_READY;	// nothing to affiliate to
		acc_drop(f);
		return;
	}
	f->view.reg = P25_REG_AFF;
	acc_queue(f, P25_ISP_GRP_AFF_REQ, now, acc_rand(f, P25_ACC_GAP_MS));
}

// The control channel was lost: what waited for it ends. A registration that was accepted stands; one under way
// starts again on the next control channel
static void acc_lost(p25_follow_t *f)
{
	p25_follow_acc_t *a = &f->acc;

	a->site_heard = false;
	a->cc_chan = 0;
	f->view.sys_ok = false;	// (the next control channel is verified again)
	f->view.rfss = f->view.site = 0xFF;
	if (f->view.req == P25_REQ_WAIT || f->view.req == P25_REQ_QUEUED)
		f->view.req = P25_REQ_TIMEOUT;
	if (f->view.reg == P25_REG_WAIT || f->view.reg == P25_REG_AFF)
		f->view.reg = P25_REG_IDLE;
	acc_drop(f);
}

// The site's identity heard: registered elsewhere, register here
static void acc_site(p25_follow_t *f)
{
	p25_follow_acc_t *a = &f->acc;
	uint8_t r = f->view.reg;

	a->site_heard = true;
	if (f->acc.blocked || r == P25_REG_OFF || r == P25_REG_IDLE || r == P25_REG_NO_TX)
		return;
	if (a->reg_site == 0xFF) {
		// Registration began before the site was heard: it is this one
		a->reg_sysid = f->view.sysid;
		a->reg_rfss = f->view.rfss;
		a->reg_site = f->view.site;
		return;
	}
	if (a->reg_sysid != f->view.sysid || a->reg_rfss != f->view.rfss || a->reg_site != f->view.site) {
		f->view.reg = P25_REG_IDLE;
		acc_drop(f);
	}
}

// The block in hand goes now: on the control channel's inbound frequency, through the caller
static void acc_send(p25_follow_t *f, uint32_t now)
{
	const p25_follow_cfg_t *c = f->cfg;
	p25_follow_acc_t *a = &f->acc;
	uint32_t hz = cc_tx_hz(f, f->view.cc);
	uint8_t blk[P25_TSBK_BYTES];
	p25_isp_t m;

	// (acc_tick sends nothing in either case; this is the last door before the air)
	if (f->acc.blocked) {
		acc_drop(f);
		return;
	}
	if (!f->view.sys_ok)
		return;
	if (!hz) {
		if (a->pend == P25_ISP_GRP_V_REQ)
			f->view.req = P25_REQ_NO_CHAN;
		f->view.reg = P25_REG_NO_TX;
		acc_drop(f);
		return;
	}
	memset(&m, 0, sizeof m);
	m.opcode = a->pend;
	m.src = c->unit_id;
	m.tg = c->talk_group;
	m.wacn = f->view.wacn <= 0xFFFFFu ? f->view.wacn : any_wacn(c) ? 0 : c->wacn;
	m.sysid = f->view.sysid <= 0xFFFu ? f->view.sysid : any_sysid(c) ? 0 : c->sysid;
	p25_isp_make(&m, blk);
	if (!f->hooks->send || !f->hooks->send(hz, blk, f->ctx))
		count8(&f->view.send_fail);	// (as a block without an answer: sent again)
	else if (a->pend == P25_ISP_U_REG_REQ)
		count8(&f->view.reg_tx);
	else if (a->pend == P25_ISP_GRP_AFF_REQ)
		count8(&f->view.aff_tx);
	else
		count8(&f->view.req_tx);
	a->sent = true;
	a->t_sent = now;
}

// The block in hand got no answer after its last send
static void acc_no_answer(p25_follow_t *f, uint32_t now)
{
	uint8_t op = f->acc.pend;

	acc_drop(f);
	count8(&f->view.timeouts);
	if (op == P25_ISP_GRP_V_REQ) {
		f->view.req = P25_REQ_TIMEOUT;
	} else {
		f->view.reg = P25_REG_RETRY;
		f->acc.t_sent = now;
	}
}

// On the control channel held, every tick: registration when due, the block in hand sent, resent or given up
static void acc_tick(p25_follow_t *f, uint32_t now)
{
	p25_follow_acc_t *a = &f->acc;

	if (!tx_on(f) || f->acc.blocked)
		return;
	if (!(f->cfg->flags & P25_FOLLOW_NO_REG)) {
		if (f->view.reg == P25_REG_IDLE && f->view.sys_ok && (a->site_heard || elapsed(now, a->t_cc, P25_ACC_SITE_MS))) {
			reg_start(f, now);
		} else if (f->view.reg == P25_REG_RETRY && elapsed(now, a->t_sent, a->backoff)) {
			a->backoff = a->backoff * 2 > P25_ACC_RETRY_MAX ? P25_ACC_RETRY_MAX : a->backoff * 2;
			reg_start(f, now);
		}
	}
	if (a->pend == NO_OP)
		return;
	if (a->sent) {
		bool queued = f->view.req == P25_REQ_QUEUED;

		if (!elapsed(now, a->t_sent, queued ? P25_ACC_QUEUE_MS : P25_ACC_ANSWER_MS))
			return;
		if (queued || ++a->tries >= P25_ACC_TRIES) {
			acc_no_answer(f, now);
			return;
		}
		a->sent = false;
		a->t_next = now + P25_ACC_GAP_MS + acc_rand(f, 3 * P25_ACC_GAP_MS);
		return;
	}
	// Sent only after a block of this channel was heard since the tune, so its status symbols are known
	// ... and, on this control channel, after its network status broadcast verified the system (TIA-102.AABD-B 5)
	if ((int32_t)(now - a->t_next) < 0 || !a->heard || !f->view.sys_ok)
		return;
	if (a->busy) {
		count8(&f->view.inb_busy);
		a->t_next = now + P25_ACC_BUSY_MS + acc_rand(f, 3 * P25_ACC_BUSY_MS);
		return;
	}
	acc_send(f, now);
}

// The request in hand is this one and went out at least once. An answer heard before that is not ours: a site that
// repeats its answers to a unit, or to one with our identity, must not register us without a word on the air
static bool acc_asked(const p25_follow_t *f, uint8_t op)
{
	return f->acc.pend == op && (f->acc.sent || f->acc.tries != 0);
}

// Answers addressed to us
static void acc_block(p25_follow_t *f, const p25_tsbk_t *m, uint32_t now)
{
	const p25_follow_cfg_t *c = f->cfg;
	p25_follow_acc_t *a = &f->acc;

	if (f->acc.blocked)
		return;
	switch (m->kind) {
	case P25_TSBK_REG_RSP:
		if (m->u.reg.id != c->unit_id || f->view.reg != P25_REG_WAIT || !acc_asked(f, P25_ISP_U_REG_REQ))
			return;
		if (m->u.reg.rv == P25_RV_ACCEPT) {
			count8(&f->view.reg_ok);
			a->backoff = P25_ACC_RETRY_MS;
			aff_start(f, now);
		} else if (m->u.reg.rv == P25_RV_FAIL) {
			acc_drop(f);
			f->view.reg = P25_REG_RETRY;
			a->t_sent = now;
		} else {
			acc_drop(f);
			f->view.reg = P25_REG_DENIED;
			denied(f, P25_ISP_U_REG_REQ, m->u.reg.rv);
		}
		break;
	case P25_TSBK_AFF_RSP:
		if (m->u.aff.dst != c->unit_id || f->view.reg != P25_REG_AFF || m->u.aff.tg != c->talk_group ||
		    !acc_asked(f, P25_ISP_GRP_AFF_REQ))
			return;
		acc_drop(f);
		if (m->u.aff.gav == P25_RV_ACCEPT) {
			count8(&f->view.aff_ok);
			f->view.reg = P25_REG_READY;
			f->view.aff_tg = m->u.aff.tg;
		} else if (m->u.aff.gav == P25_RV_FAIL) {
			f->view.reg = P25_REG_RETRY;
			a->t_sent = now;
		} else {
			f->view.reg = P25_REG_DENIED;
			denied(f, P25_ISP_GRP_AFF_REQ, m->u.aff.gav);
		}
		break;
	case P25_TSBK_DENY:
	case P25_TSBK_QUEUED:
		// The answer names the request by its opcode
		if (m->u.rsp.dst != c->unit_id || a->pend == NO_OP || !acc_asked(f, m->u.rsp.svc))
			return;
		if (m->kind == P25_TSBK_QUEUED) {
			if (a->pend == P25_ISP_GRP_V_REQ) {
				count8(&f->view.queued);
				f->view.req = P25_REQ_QUEUED;
				a->sent = true;
				a->t_sent = now;
			}
			return;
		}
		denied(f, m->u.rsp.svc, m->u.rsp.reason);
		if (a->pend == P25_ISP_GRP_V_REQ)
			f->view.req = P25_REQ_DENIED;
		else
			f->view.reg = P25_REG_DENIED;
		acc_drop(f);
		break;
	default:
		break;
	}
}

// A grant: true when it is ours (the answer to our voice request), which tunes our voice channel
static bool acc_grant(p25_follow_t *f, uint8_t op, const p25_grant_t *g, uint32_t now)
{
	const p25_follow_cfg_t *c = f->cfg;
	uint32_t rx, tx;

	if ((f->view.req != P25_REQ_WAIT && f->view.req != P25_REQ_QUEUED) || op != P25_OP_GRP_V_GRANT ||
	    g->src != c->unit_id || g->tg != c->talk_group || !acc_asked(f, P25_ISP_GRP_V_REQ))
		return false;
	acc_drop(f);
	count8(&f->view.req_grant);
	rx = chan_hz(f, g->ch);
	tx = chan_tx_hz(f, g->ch, rx);
	if (!rx || !tx || (f->iden[g->ch >> 12].flags & IDEN_TDMA)) {
		f->view.req = P25_REQ_NO_CHAN;
		return true;
	}
	f->view.req = P25_REQ_TALK;
	f->view.state = P25_FOLLOW_VOICE;
	f->view.voice_hz = rx;
	f->view.talk_hz = tx;
	f->view.unit_call = false;
	f->view.enc = false;
	f->view.tg = g->tg;
	f->view.src = c->unit_id;
	f->view.dst = 0;
	f->left_ch = f->acc.talk_ch = g->ch;
	f->t_call = f->t_good = now;
	tune(f, rx, P25_FOLLOW_TUNE_TALK, g->tg);
	return true;
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
	f->acc.heard = true;
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
		if (p25_wacn_no_tx(m.u.net.wacn) || p25_sysid_no_tx(m.u.net.sysid))
			sys_block(f);
		else if (!f->acc.blocked)
			f->view.sys_ok = true;
		f->view.wacn = m.u.net.wacn;
		f->view.sysid = m.u.net.sysid;
		f->acc.cc_chan = m.u.net.ch;
		break;
	case P25_TSBK_RFSS:
		if (p25_sysid_no_tx(m.u.site.sysid))
			sys_block(f);
		f->view.sysid = m.u.site.sysid;
		f->view.rfss = m.u.site.rfss;
		f->view.site = m.u.site.site;
		f->acc.cc_chan = m.u.site.ch;
		acc_site(f);
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
		// While we ask for a channel, nothing else is followed
		if (acc_grant(f, m.opcode, g, now) || f->view.req == P25_REQ_WAIT || f->view.req == P25_REQ_QUEUED)
			break;
		if (!consider(f, m.opcode, g->unit, g->ch, g->tg, g->dst, g->src, g->has_svc, g->svc, now) && g->ch2 &&
		    m.opcode == P25_OP_GRP_V_UPDT)
			consider(f, m.opcode, false, g->ch2, g->tg2, 0, 0, false, 0, now);
		break;
	}
	case P25_TSBK_REG_RSP:
	case P25_TSBK_AFF_RSP:
	case P25_TSBK_DENY:
	case P25_TSBK_QUEUED:
		if (tx_on(f))
			acc_block(f, &m, now);
		break;
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
		// The transmit offset in 250 kHz units where it is a multiple of them, else in spacings; 0 is not known
		if (p->tx_off_hz && p->tx_off_hz % 250000 == 0 && p->tx_off_hz / 250000 <= INT16_MAX &&
		    p->tx_off_hz / 250000 >= -INT16_MAX) {
			i->tx_off = (int16_t)(p->tx_off_hz / 250000);
			i->flags |= IDEN_OFF;
		} else if (p->tx_off_hz && p->tx_off_hz % (int32_t)p->spacing_hz == 0 &&
			   p->tx_off_hz / (int32_t)p->spacing_hz <= INT16_MAX && p->tx_off_hz / (int32_t)p->spacing_hz >= -INT16_MAX) {
			i->tx_off = (int16_t)(p->tx_off_hz / (int32_t)p->spacing_hz);
			i->flags |= IDEN_OFF | IDEN_OFF_STEP;
		}
		f->view.idens |= (uint16_t)(1u << n);
	}
	f->acc.pend = NO_OP;
	f->acc.backoff = P25_ACC_RETRY_MS;
	f->acc.rng = (cfg->unit_id * 2654435761u) ^ now ^ 0x5A5A5A5Au;
	if (!f->acc.rng)
		f->acc.rng = 1;
	f->view.reg = !tx_on(f) || (cfg->flags & P25_FOLLOW_NO_REG) ? P25_REG_OFF : P25_REG_IDLE;
	if (p25_wacn_no_tx(cfg->wacn) || p25_sysid_no_tx(cfg->sysid) || (cfg->flags & P25_FOLLOW_BLOCKED)) {
		f->acc.blocked = true;
		f->view.reg = P25_REG_BLOCKED;
	}
	f->view.deny_svc = 0xFF;
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
	f->view.unverified = false;	// a block that passed its CRC
	on_block(f, blk, nac, now);
	view_end(f);
}

void p25_follow_unit(p25_follow_t *f, const p25_rx_unit_t *u, uint32_t now)
{
	uint8_t blk[P25_TSBK_BYTES];

	if (f->tuning || f->view.state == P25_FOLLOW_OFF)
		return;
	view_begin(f);
	// A voice channel's link control may carry the network or RFSS status broadcast: a blocked system there blocks
	// as it does here
	if (p25_unit_sys_no_tx(u))
		sys_block(f);
	switch (u->duid) {
	case P25_DUID_TSDU: {
		uint8_t n = u->nsyms <= p25_blocks_syms(1) ? 1 : u->nsyms <= p25_blocks_syms(2) ? 2 : 3;
		uint16_t nss = (uint16_t)(u->nsyms / 36);

		// The site's status symbols say whether its inbound channel is busy: the last one of the unit counts
		if (nss)
			f->acc.busy = p25_status_get(u->air, (uint16_t)(nss - 1)) == P25_SS_BUSY;

		for (uint8_t k = 0; k < n && !f->tuning; k++) {
			bool good = p25_block_get(u->air, k, blk) >= 0;
			if (!good && !((f->cfg->flags & P25_FOLLOW_BAD_CRC) && rx_seen(&f->seen, blk, P25_TSBK_BYTES))) {
				f->view.crc_fail++;
				continue;
			}
			f->view.crc_taken += !good;
			f->view.unverified = !good;
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
			uint8_t data[P25_TSBK_BYTES];
			p25_tsbk_t m;

			count(&f->view.mbt);
			f->view.mbt_op = h.opcode;
			// The extended network status broadcast is the system's identity as the abbreviated one is: taken as
			// that block. Other packets are counted only
			if (u->nsyms >= p25_blocks_syms(2) && p25_block_decode(u->air, 1, data) >= 0 &&
			    p25_mbt_net_parse(blk, data, &m)) {
				p25_tsbk_make(&m, blk);
				f->view.blocks++;
				on_block(f, blk, u->nac, now);
			}
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
		// (on our own voice channel the caller ends it: p25_follow_talk_end)
		if (f->view.state == P25_FOLLOW_VOICE && f->view.req != P25_REQ_TALK)
			leave(f, LEAVE_TERM, now);
		break;
	default:
		break;
	}
	view_end(f);
}

void p25_follow_call(p25_follow_t *f, const p25_call_t *c, uint32_t now)
{
	if (f->tuning || f->view.state != P25_FOLLOW_VOICE || f->view.req == P25_REQ_TALK)
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
		} else {
			acc_tick(f, now);
		}
		break;
	case P25_FOLLOW_VOICE:
		if (f->view.req == P25_REQ_TALK) {
			// We transmit there and hear nothing: only the end the caller reports, or a limit, brings us back
			if (elapsed(now, f->t_call, P25_ACC_TALK_MAX_MS)) {
				f->view.req = P25_REQ_IDLE;
				leave(f, LEAVE_OTHER, now);
			}
		} else if (elapsed(now, f->t_good, c->voice_lost_ms ? c->voice_lost_ms : P25_FOLLOW_VOICE_LOST_MS)) {
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

uint8_t p25_follow_can_talk(const p25_follow_view_t *v, uint8_t flags)
{
	if (!(flags & P25_FOLLOW_TX) || v->reg == P25_REG_NO_TX || v->unverified)
		return P25_TALK_OFF;
	if (v->reg == P25_REG_BLOCKED)
		return P25_TALK_BLOCKED;
	if ((v->state != P25_FOLLOW_CONTROL && v->state != P25_FOLLOW_VOICE) || !v->sys_ok)
		return P25_TALK_NO_CC;
	if (!(flags & P25_FOLLOW_NO_REG) && v->reg != P25_REG_READY)
		return P25_TALK_NOT_REG;
	return P25_TALK_OK;
}

void p25_follow_ptt(p25_follow_t *f, bool down, uint32_t now)
{
	if (f->view.state == P25_FOLLOW_OFF)
		return;
	view_begin(f);
	f->acc.ptt = down;
	if (!down) {
		// Released before the grant: the request is dropped (a grant that comes later is not taken)
		if (f->view.req != P25_REQ_TALK) {
			if (f->acc.pend == P25_ISP_GRP_V_REQ)
				acc_drop(f);
			f->view.req = P25_REQ_IDLE;
		}
	} else if (tx_on(f) && f->view.req != P25_REQ_TALK && f->view.req != P25_REQ_WAIT &&
		   f->view.req != P25_REQ_QUEUED && p25_follow_can_talk(&f->view, f->cfg->flags) == P25_TALK_OK) {
		f->view.req = P25_REQ_WAIT;
		f->view.deny_svc = 0xFF;
		acc_queue(f, P25_ISP_GRP_V_REQ, now, acc_rand(f, P25_ACC_GAP_MS));
		// Following a call (our group's in its hang time too): back to the control channel to ask there
		if (f->view.state == P25_FOLLOW_VOICE)
			leave(f, LEAVE_TERM, now);
	}
	view_end(f);
}

void p25_follow_talk_end(p25_follow_t *f, uint32_t now)
{
	if (f->view.req != P25_REQ_TALK)
		return;
	view_begin(f);
	f->view.req = P25_REQ_IDLE;
	if (f->view.state == P25_FOLLOW_VOICE)
		leave(f, LEAVE_TERM, now);
	view_end(f);
}

#endif
