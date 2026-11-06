// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The DMR Tier III mobile station engine, receive side (t3_ms.h, docs/spec/dmr-trunking.md)
#include <string.h>
#include "dmr/trunk/t3_ms.h"

static bool since(uint32_t now, uint32_t t, uint32_t ms)
{
	return (uint32_t)(now - t) >= ms;
}

static uint32_t or_default(uint16_t v, uint32_t def)
{
	return v != 0 ? v : def;
}

// An MBC continuation follows its header on the same slot one TDMA frame later; allow one lost frame
#define MBC_GAP_MS	130

const char *t3_state_name(t3_state_t s)
{
	switch (s) {
	case T3_OFF: return "off";
	case T3_HUNT: return "hunt";
	case T3_CAMPED: return "camped";
	case T3_FOLLOW: return "follow";
	}
	return "?";
}

const char *t3_reg_name(t3_reg_t r)
{
	switch (r) {
	case T3_REG_UNKNOWN: return "unknown";
	case T3_REG_NOT_NEEDED: return "not-needed";
	case T3_REG_LISTEN: return "listen";
	case T3_REG_NEEDED: return "needed";
	case T3_REG_OK: return "ok";
	case T3_REG_DENIED: return "denied";
	}
	return "?";
}

static void tune(t3_ms_t *ms, uint32_t now, const t3_chan_t *c, uint8_t slot, bool payload)
{
	ms->tuned = *c;
	ms->tuned_slot = slot;
	ms->tuned_ms = now;
	ms->pending.type = T3_ACT_TUNE;
	ms->pending.chan = *c;
	ms->pending.slot = slot;
	ms->pending.payload = payload;
	ms->mbc[0].open = false;
	ms->mbc[1].open = false;
	ms->slc_foreign = 0;
}

// ---- Hunting (6.3.2.1, Annex D) ----

static uint8_t hunt_count(const t3_ms_t *ms)
{
	return (uint8_t)(ms->cfg->nctrl + ms->cfg->nhunt);
}

static bool hunt_candidate(const t3_ms_t *ms, uint8_t i, t3_chan_t *out)
{
	const t3_cfg_t *c = ms->cfg;
	if (i < c->nctrl) {
		*out = c->ctrl[i];
		return out->rx_hz != 0;
	}
	i = (uint8_t)(i - c->nctrl);
	return i < c->nhunt && t3_plan_lookup(&c->plan, &ms->learned, c->hunt[i], out);
}

static void hunt_next(t3_ms_t *ms, uint32_t now)
{
	uint32_t dwell = or_default(ms->cfg->dwell_ms, T3_DWELL_MS);
	ms->hunt_deadline = now + dwell;
	if (ms->retry_tscc) {
		// The channel just lost comes first: a fade more often than a move
		ms->retry_tscc = false;
		tune(ms, now, &ms->tscc, 0, false);
		return;
	}
	uint8_t n = hunt_count(ms);
	for (uint8_t k = 0; k < n; k++) {
		t3_chan_t c;
		uint8_t i = ms->hunt_idx;
		ms->hunt_idx = (uint8_t)((ms->hunt_idx + 1u) % n);
		if (hunt_candidate(ms, i, &c)) {
			tune(ms, now, &c, 0, false);
			return;
		}
	}
	// Nothing to tune to: stay where we are and look again after the dwell
}

static void enter_hunt(t3_ms_t *ms, uint32_t now, bool retry)
{
	ms->state = T3_HUNT;
	ms->confirmed = false;
	ms->tscc_slots = 0;
	ms->csc_valid = false;
	ms->retry_tscc = retry && ms->tscc.rx_hz != 0;
	ms->st.hunts++;
	hunt_next(ms, now);
}

// ---- Start, stop ----

void t3_ms_start(t3_ms_t *ms, const t3_cfg_t *cfg, uint32_t now)
{
	memset(ms, 0, sizeof(*ms));
	ms->cfg = cfg;
	ms->auth = cfg->auth;
	ms->reg = T3_REG_UNKNOWN;
	t3_learn_clear(&ms->learned);
	enter_hunt(ms, now, false);
}

void t3_ms_stop(t3_ms_t *ms)
{
	ms->state = T3_OFF;
	ms->pending.type = T3_ACT_NONE;
	ms->confirmed = false;
}

// ---- Which calls to follow ----

static bool is_allcall(uint32_t id)
{
	return id == T3_ID_ALLMSID || id == T3_ID_ALLMSIDL || id == T3_ID_ALLMSIDZ;
}

bool t3_ms_wants_group(const t3_ms_t *ms, uint32_t tg, bool emergency)
{
	const t3_cfg_t *c = ms->cfg;
	if (is_allcall(tg)) {
		return (c->opts & T3_OPT_ALLCALL) != 0;
	}
	if (tg == T3_ID_NULL || t3_is_gateway(tg)) {
		return false;
	}
	if ((c->opts & T3_OPT_MONITOR) || (emergency && (c->opts & T3_OPT_EMERGENCY_ANY))) {
		return true;
	}
	for (uint8_t i = 0; i < c->ntg; i++) {
		if (c->tg[i].id == tg && (c->tg[i].flags & T3_TG_LISTEN)) {
			return true;
		}
	}
	return false;
}

static bool wants_grant(const t3_ms_t *ms, const t3_csbk_t *p, bool *group)
{
	const t3_grant_t *g = &p->u.grant;
	switch (p->op) {
	case T3_OP_TV_GRANT:
	case T3_OP_BTV_GRANT:
		*group = true;
		return t3_ms_wants_group(ms, g->dst, g->emergency);
	case T3_OP_PV_GRANT:
	case T3_OP_PV_GRANT_DX:
		*group = false;
		return (ms->cfg->opts & T3_OPT_PRIVATE) && ms->cfg->own_id != 0 && g->dst == ms->cfg->own_id;
	default:
		// Data grants: the data services come later; a grant to a gateway or another MS is not ours
		return false;
	}
}

static bool is_grant(uint8_t op)
{
	return op >= T3_OP_PV_GRANT && op <= T3_OP_TD_GRANT_MI;
}

// The TSCC keeps repeating a grant while the call lasts (late entry). One we just left isn't followed again so soon
static bool just_left(const t3_ms_t *ms, uint32_t now, const t3_grant_t *g, const t3_chan_t *c)
{
	return ms->cleared_ms != 0 && !since(now, ms->cleared_ms, T3_REGRANT_MS) && ms->cleared.dst == g->dst &&
	       ms->cleared.chan.lcn == c->lcn && ms->cleared.slot == g->slot;
}

static bool same_call(const t3_call_t *a, const t3_grant_t *g, const t3_chan_t *c)
{
	return a->dst == g->dst && a->chan.lcn == c->lcn && a->chan.rx_hz == c->rx_hz && a->slot == g->slot;
}

static void follow(t3_ms_t *ms, uint32_t now, const t3_csbk_t *p, const t3_chan_t *c, bool group)
{
	const t3_grant_t *g = &p->u.grant;
	t3_chan_t ch = *c;
	if (ch.cc == T3_CC_ANY) {
		ch.cc = ms->tscc.cc;
	}
	ms->call.op = p->op;
	ms->call.dst = g->dst;
	ms->call.src = g->src;
	ms->call.group = group;
	ms->call.emergency = g->emergency;
	ms->call.chan = ch;
	ms->call.slot = g->slot;
	ms->call.start_ms = now;
	ms->protect_dis = false;
	ms->state = T3_FOLLOW;
	ms->last_act_ms = now;
	ms->st.followed++;
	tune(ms, now, &ch, g->slot, true);
}

// A grant heard on the TSCC (camped) or on the payload channel (a move or a second call). chan is set when an
// appended CG_AP gave the frequencies
static void on_grant(t3_ms_t *ms, uint32_t now, const t3_csbk_t *p, const t3_chan_t *chan)
{
	bool group = false;
	if (ms->state == T3_CAMPED) {
		ms->st.grants++;
	}
	if (!wants_grant(ms, p, &group)) {
		return;
	}
	t3_chan_t c;
	if (chan != NULL) {
		c = *chan;
	} else if (p->u.grant.lpcn == T3_LPCN_ABSOLUTE ||
		   !t3_plan_lookup(&ms->cfg->plan, &ms->learned, p->u.grant.lpcn, &c)) {
		ms->st.unmapped++;
		ms->st.last_unmapped = p->u.grant.lpcn;
		return;
	}
	if (ms->state == T3_FOLLOW) {
		// Already in this call: the payload channel repeats its grant for late entry
		if (same_call(&ms->call, &p->u.grant, &c)) {
			return;
		}
		// A grant for another call while in one: only the call being followed may move us (6.6.2.4)
		if (ms->call.dst != p->u.grant.dst) {
			return;
		}
	} else if (just_left(ms, now, &p->u.grant, &c)) {
		return;
	}
	follow(ms, now, p, &c, group);
}

static void return_to_tscc(t3_ms_t *ms, uint32_t now)
{
	ms->cleared = ms->call;
	ms->cleared_ms = now != 0 ? now : 1;
	ms->protect_dis = false;
	ms->state = T3_CAMPED;
	ms->last_valid_ms = now;
	ms->st.returns++;
	tune(ms, now, &ms->tscc, 0, false);
}

// ---- The control channel ----

static void set_reg(t3_ms_t *ms, bool reg)
{
	if (!reg) {
		ms->reg = T3_REG_NOT_NEEDED;
	} else if (ms->cfg->opts & T3_OPT_LISTEN_ONLY) {
		ms->reg = T3_REG_LISTEN;
	} else if (ms->reg != T3_REG_OK && ms->reg != T3_REG_DENIED) {
		ms->reg = T3_REG_NEEDED;
	}
}

static bool authorized(t3_ms_t *ms, uint16_t code)
{
	if ((ms->cfg->opts & T3_OPT_LEARN_SYSCODE) && !ms->auth_learned) {
		t3_syscode_t s;
		t3_syscode_split(code, &s);
		ms->auth.model = s.model;
		ms->auth.net = s.net;
		ms->auth_learned = true;
	}
	return t3_authorize(&ms->auth, code) == T3_AUTH_OK;
}

static void confirm(t3_ms_t *ms, uint32_t now, uint8_t slot, uint8_t cc, const t3_aloha_t *a)
{
	if (ms->state == T3_HUNT) {
		ms->state = T3_CAMPED;
		ms->tscc = ms->tuned;
		ms->tscc.cc = cc;
	}
	if (ms->syscode != 0 && ms->syscode != a->syscode) {
		// Another network: what was learned belongs to the old one. A site change of one network keeps it
		t3_syscode_t o, n;
		t3_syscode_split(ms->syscode, &o);
		t3_syscode_split(a->syscode, &n);
		if (o.model != n.model || o.net != n.net) {
			t3_learn_clear(&ms->learned);
		}
	}
	ms->confirmed = true;
	ms->syscode = a->syscode;
	ms->aloha = *a;
	ms->tscc_slots |= (uint8_t)(1u << (slot - 1u));
	ms->last_valid_ms = now;
	set_reg(ms, a->reg);
}

// A complete PDU heard on the TSCC we are hunting or camped on. cdef: the appended block of an MBC, or NULL
static void on_tscc(t3_ms_t *ms, uint32_t now, uint8_t slot, uint8_t cc, const t3_csbk_t *p, const t3_cdef_t *cdef)
{
	if (p->op == T3_OP_C_ALOHA) {
		if (!authorized(ms, p->u.aloha.syscode)) {
			ms->st.foreign++;
			if (ms->state == T3_HUNT) {
				hunt_next(ms, now);
			}
			return;
		}
		confirm(ms, now, slot, cc, &p->u.aloha);
		return;
	}
	if (ms->state != T3_CAMPED) {
		// Only an Aloha confirms a TSCC (6.3.2.2.2): it carries the system code
		return;
	}
	if (p->op == T3_OP_C_BCAST) {
		if (!authorized(ms, p->u.bcast.syscode)) {
			ms->st.foreign++;
			return;
		}
		ms->last_valid_ms = now;
		set_reg(ms, p->u.bcast.reg);
		if (p->u.bcast.type == T3_ANN_CHAN_FREQ && cdef != NULL && ms->cfg->plan.learn) {
			t3_cdef_t c = *cdef;
			if (c.chan == T3_LPCN_NULL) {
				c.chan = (uint16_t)(p->u.bcast.parms2 & 0xFFFu);
			}
			if (t3_learn_add(&ms->learned, &c)) {
				ms->st.learned++;
			}
		}
		return;
	}
	// Grants and the rest carry no system code: the color code and the confirmed channel vouch for them
	ms->last_valid_ms = now;
	if (is_grant(p->op)) {
		if (cdef != NULL) {
			t3_chan_t c = { cdef->chan, cdef->rx_hz, cdef->tx_hz, cdef->cc };
			if (ms->cfg->plan.learn && t3_learn_add(&ms->learned, cdef)) {
				ms->st.learned++;
			}
			on_grant(ms, now, p, &c);
		} else {
			on_grant(ms, now, p, NULL);
		}
	}
}

// ---- The payload channel ----

static bool for_call(const t3_ms_t *ms, bool group, uint32_t dst)
{
	if (dst == T3_ID_ALLMSI) {
		return true;
	}
	if (group != ms->call.group) {
		return false;
	}
	if (group) {
		return dst == ms->call.dst;
	}
	// An individual call: either party
	return dst == ms->call.dst || dst == ms->call.src;
}

static void on_payload(t3_ms_t *ms, uint32_t now, uint8_t slot, const t3_csbk_t *p, const t3_cdef_t *cdef)
{
	if (slot != ms->call.slot) {
		// The other slot of the payload channel carries another call or the TSCC (composite)
		return;
	}
	ms->last_act_ms = now;
	switch (p->op) {
	case T3_OP_P_CLEAR:
		if (for_call(ms, p->u.clear.group, p->u.clear.dst)) {
			t3_chan_t c;
			if (p->u.clear.lpcn != T3_LPCN_NULL && p->u.clear.lpcn != ms->tscc.lcn &&
			    t3_plan_lookup(&ms->cfg->plan, &ms->learned, p->u.clear.lpcn, &c)) {
				// Go to another TSCC (7.1.1.1.4): it must still confirm with an Aloha before T_Nosig
				c.cc = ms->tscc.cc;
				ms->tscc = c;
			}
			return_to_tscc(ms, now);
		}
		break;
	case T3_OP_P_MAINT:
		if (p->u.maint.kind == T3_MAINT_DISCON && for_call(ms, ms->call.group, p->u.maint.dst)) {
			return_to_tscc(ms, now);
		}
		break;
	case T3_OP_P_PROTECT:
		if (for_call(ms, p->u.protect.group, p->u.protect.dst)) {
			if (p->u.protect.kind == T3_PROTECT_DIS_PTT) {
				ms->protect_dis = true;
			} else if (p->u.protect.kind == T3_PROTECT_EN_PTT) {
				ms->protect_dis = false;
			}
		}
		break;
	default:
		if (is_grant(p->op)) {
			if (cdef != NULL) {
				t3_chan_t c = { cdef->chan, cdef->rx_hz, cdef->tx_hz, cdef->cc };
				on_grant(ms, now, p, &c);
			} else {
				on_grant(ms, now, p, NULL);
			}
		}
		break;
	}
}

// ---- Bursts in ----

static void deliver(t3_ms_t *ms, uint32_t now, uint8_t slot, uint8_t cc, const t3_csbk_t *p, const t3_cdef_t *cdef)
{
	if (ms->state == T3_FOLLOW) {
		on_payload(ms, now, slot, p, cdef);
	} else {
		on_tscc(ms, now, slot, cc, p, cdef);
	}
}

static bool cc_ok(const t3_ms_t *ms, uint8_t cc)
{
	uint8_t want = ms->state == T3_FOLLOW ? ms->call.chan.cc : ms->tuned.cc;
	if (ms->state == T3_CAMPED) {
		want = ms->tscc.cc;
	}
	return want == T3_CC_ANY || want == cc;
}

void t3_ms_rx(t3_ms_t *ms, uint32_t now, uint8_t slot, uint8_t dt, uint8_t cc, const uint8_t d[T3_CSBK_BYTES])
{
	if (ms->state == T3_OFF || slot < 1 || slot > 2 || !since(now, ms->tuned_ms, T3_SETTLE_MS)) {
		return;
	}
	if (dt != T3_DT_CSBK && dt != T3_DT_MBC_HDR && dt != T3_DT_MBC_CONT) {
		return;
	}
	if (!cc_ok(ms, cc)) {
		return;
	}
	uint8_t si = (uint8_t)(slot - 1u);
	t3_csbk_t p;
	if (dt == T3_DT_MBC_CONT) {
		bool have = ms->mbc[si].open && !since(now, ms->mbc[si].ms, MBC_GAP_MS);
		ms->mbc[si].open = false;
		if (!have) {
			return;
		}
		t3_cdef_t c;
		if (!t3_crc_ok(d, 0) || !t3_cdef_parse(d, ms->mbc[si].hdr.op, &c)) {
			ms->st.crc_bad++;
			return;
		}
		deliver(ms, now, slot, cc, &ms->mbc[si].hdr, &c);
		return;
	}
	uint16_t mask = dt == T3_DT_CSBK ? T3_CRC_MASK_CSBK : T3_CRC_MASK_MBC_HDR;
	if (!t3_crc_ok(d, mask)) {
		ms->st.crc_bad++;
		return;
	}
	ms->st.csbk++;
	bool known = t3_csbk_parse(d, &p);
	if (ms->state == T3_FOLLOW && slot == ms->call.slot) {
		// Any good CSBK on the call's slot keeps it alive, a manufacturer's own included
		ms->last_act_ms = now;
	}
	if (!known || p.fid != 0) {
		return;
	}
	if (dt == T3_DT_MBC_HDR) {
		// The header of a multi-block PDU: wait for its continuation
		ms->mbc[si].open = true;
		ms->mbc[si].hdr = p;
		ms->mbc[si].ms = now;
		return;
	}
	deliver(ms, now, slot, cc, &p, NULL);
}

void t3_ms_slc(t3_ms_t *ms, uint32_t now, uint32_t slc)
{
	t3_sysparms_t sp;
	if (ms->state == T3_OFF || ms->state == T3_FOLLOW || !since(now, ms->tuned_ms, T3_SETTLE_MS) ||
	    !t3_sysparms_parse(slc, &sp) || !sp.control) {
		return;
	}
	// MODEL, NET and SITE; the PAR bits aren't in the short LC, so ours are assumed
	uint16_t code = (uint16_t)(((uint16_t)sp.model << 14) | ((uint16_t)sp.netsite << 2) | T3_PAR_AB);
	t3_auth_t a = ms->auth;
	a.category = T3_PAR_A;
	bool learning = (ms->cfg->opts & T3_OPT_LEARN_SYSCODE) && !ms->auth_learned;
	if (!learning && t3_authorize(&a, code) != T3_AUTH_OK) {
		// Another system's control channel: no need to wait for its Aloha. The short LC's CRC is 8 bits, so one
		// corrupt word passes now and then: act on two alike in a row
		ms->st.foreign_slc++;
		bool again = ms->slc_foreign == (uint32_t)code + 1u;
		ms->slc_foreign = (uint32_t)code + 1u;
		if (again && ms->state == T3_HUNT) {
			ms->slc_foreign = 0;
			hunt_next(ms, now);
		}
		return;
	}
	ms->slc_foreign = 0;
	if (ms->state == T3_CAMPED) {
		ms->csc = sp.csc;
		ms->csc_valid = true;
	}
}

void t3_ms_activity(t3_ms_t *ms, uint32_t now, uint8_t slot)
{
	if (ms->state == T3_FOLLOW && slot == ms->call.slot && since(now, ms->tuned_ms, T3_SETTLE_MS)) {
		ms->last_act_ms = now;
	}
}

// ---- Timers, actions out ----

bool t3_ms_poll(t3_ms_t *ms, uint32_t now, t3_act_t *act)
{
	switch (ms->state) {
	case T3_OFF:
		break;
	case T3_HUNT:
		if (ms->pending.type == T3_ACT_NONE && (int32_t)(now - ms->hunt_deadline) >= 0) {
			hunt_next(ms, now);
		}
		break;
	case T3_CAMPED:
		if (since(now, ms->last_valid_ms, or_default(ms->cfg->nosig_ms, T3_NOSIG_MS))) {
			enter_hunt(ms, now, true);
		}
		break;
	case T3_FOLLOW:
		if (since(now, ms->last_act_ms, or_default(ms->cfg->inactive_ms, T3_INACTIVE_MS)) ||
		    since(now, ms->call.start_ms, T3_CALL_MAX_MS)) {
			return_to_tscc(ms, now);
		}
		break;
	}
	if (ms->pending.type == T3_ACT_NONE) {
		return false;
	}
	*act = ms->pending;
	ms->pending.type = T3_ACT_NONE;
	return true;
}
