// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The DMR Tier III mobile station engine (t3_ms.h, docs/spec/dmr-trunking.md)
#include <string.h>
#include "config.h"
#include "dmr/trunk/t3_ms.h"

static bool since(uint32_t now, uint32_t t, uint32_t ms)
{
	return (uint32_t)(now - t) >= ms;
}

// now is earlier than t (wrapping)
static bool before(uint32_t now, uint32_t t)
{
	return (int32_t)(now - t) < 0;
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

// May the radio transmit at all? Access code behind this folds away when the build leaves it out
static bool may_send(const t3_ms_t *ms)
{
	return CONFIG_DMR_TRUNK_TX && !(ms->cfg->opts & T3_OPT_LISTEN_ONLY);
}

static uint32_t my_id(const t3_ms_t *ms)
{
	return ms->cfg->reg_id != 0 ? ms->cfg->reg_id : ms->cfg->own_id;
}

static bool is_me(const t3_ms_t *ms, uint32_t id)
{
	return id != T3_ID_NULL && (id == ms->cfg->own_id || id == my_id(ms));
}

// Actions wait in a short queue: a SEND goes out before the retune that follows it (P_MAINT, then back). A retune
// drops a retune not yet taken and whatever was queued after it
static void push(t3_ms_t *ms, const t3_act_t *a)
{
	if (a->type == T3_ACT_TUNE) {
		for (uint8_t i = 0; i < ms->nq; i++) {
			if (ms->q[i].type == T3_ACT_TUNE) {
				ms->nq = i;
				break;
			}
		}
	}
	if (ms->nq < T3_ACT_QUEUE) {
		ms->q[ms->nq++] = *a;
	}
}

static void tune(t3_ms_t *ms, uint32_t now, const t3_chan_t *c, uint8_t slot, bool payload)
{
	ms->tuned = *c;
	ms->tuned_ms = now;
	ms->level_dbm = 0;
	ms->weak_since = 0;
	t3_act_t a;
	memset(&a, 0, sizeof(a));
	a.type = T3_ACT_TUNE;
	a.chan = *c;
	a.slot = slot;
	a.payload = payload;
	push(ms, &a);
	ms->mbc[0].open = false;
	ms->mbc[1].open = false;
	ms->udt_rx.open = false;
	ms->slc_foreign = 0;
	ms->aloha_here = false;
	ms->lent_ms = 0;
	ms->tx.count = 0;
	ms->tx.udt = false;
}

static void send(t3_ms_t *ms, const uint8_t burst[T3_CSBK_BYTES], uint8_t slot, bool payload)
{
	t3_act_t a;
	memset(&a, 0, sizeof(a));
	a.type = T3_ACT_SEND;
	a.slot = slot;
	a.payload = payload;
	memcpy(a.burst, burst, T3_CSBK_BYTES);
	push(ms, &a);
	ms->st.sent++;
}

// The short data upload: the UDT header, then its blocks in the next frames
static void send_udt(t3_ms_t *ms, uint8_t slot)
{
	t3_act_t a;
	memset(&a, 0, sizeof(a));
	a.type = T3_ACT_SEND;
	a.slot = slot;
	a.udt = true;
	a.nblocks = ms->tx.nblocks;
	a.blocks = (const uint8_t (*)[T3_UDT_BLOCK])ms->up;
	memcpy(a.burst, ms->tx.burst, T3_CSBK_BYTES);
	push(ms, &a);
	ms->st.sent++;
}

static void finish(t3_ms_t *ms, uint32_t now, t3_res_t r);
static bool is_voice(t3_svc_t s);

// A logical channel's frequencies: the plan (learned, table, fixed), else a programmed control channel with that
// number. A one-carrier site grants its own carrier's other slot, which a radio with only the control channel
// programmed (no plan, no Chan_Freq heard yet) knows that way
static bool chan_lookup(const t3_ms_t *ms, uint16_t lcn, t3_chan_t *out)
{
	const t3_cfg_t *c = ms->cfg;
	if (t3_plan_lookup(&c->plan, &ms->learned, lcn, out)) {
		return true;
	}
	if (lcn == T3_LPCN_NULL || lcn > T3_LPCN_MAX) {
		return false;
	}
	for (uint8_t i = 0; i < c->nctrl; i++) {
		if (c->ctrl[i].lcn == lcn && c->ctrl[i].rx_hz != 0) {
			*out = c->ctrl[i];
			return true;
		}
	}
	return false;
}

// ---- Hunting (6.3.2.1, Annex D) ----

// The adjacent sites' TSCCs come first (6.7.1.7: they are the likelier to serve), then the programmed ones
static uint8_t hunt_count(const t3_ms_t *ms)
{
	return (uint8_t)(ms->nadj + ms->cfg->nctrl);
}

static bool hunt_candidate(const t3_ms_t *ms, uint8_t i, t3_chan_t *out)
{
	const t3_cfg_t *c = ms->cfg;
	if (i < ms->nadj) {
		*out = ms->adj[i].chan;
		return out->rx_hz != 0;
	}
	i = (uint8_t)(i - ms->nadj);
	if (i < c->nctrl) {
		*out = c->ctrl[i];
		return out->rx_hz != 0;
	}
	return false;
}

static void hunt_next(t3_ms_t *ms, uint32_t now)
{
	uint32_t dwell = T3_DWELL_MS;
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
		if (ms->hunt_idx == 0) {
			ms->hunt_strict = false;	// a whole pass found nothing better: take any TSCC again
		}
		if (hunt_candidate(ms, i, &c)) {
			tune(ms, now, &c, 0, false);
			return;
		}
	}
	// Nothing to tune to: stay where we are and look again after the dwell
}

static uint32_t rnd(t3_ms_t *ms);

// C_BCAST MassReg (6.4.5.1, table 7.76): Broadcast_Parms1 holds Reg_Window (bits 8-5) and the Aloha mask (4-0),
// Parms2 the address. Applicable when the mask's low bits of the address match the radio's ID. A window starts a
// registration at a random time inside it; another MassReg before the window ends is ignored; Reg_Window 0 cancels
static void on_mass_reg(t3_ms_t *ms, uint32_t now, const t3_bcast_t *b)
{
	static const uint32_t window_ms[16] = { 0, 500, 1000, 2000, 5000, 10000, 20000, 30000, 100000, 300000,
						1000000, 3000000, 10000000, 30000000, 100000000, 200000000 };
	uint8_t mask = (uint8_t)(b->parms1 & 0x1Fu);
	uint8_t win = (uint8_t)((b->parms1 >> 5) & 0xFu);
	uint32_t bits = mask >= 24u ? 0xFFFFFFu : ((1u << mask) - 1u);
	if (!ms->confirmed || mask > 24u || ((my_id(ms) ^ b->parms2) & bits) != 0) {
		return;
	}
	if (win == 0) {
		ms->massreg = false;	// a registration already under way completes
		return;
	}
	if (ms->massreg && (int32_t)(now - ms->massreg_end) < 0) {
		return;
	}
	ms->massreg = true;
	ms->massreg_at = now + rnd(ms) % (window_ms[win] + 1u);
	ms->massreg_end = now + window_ms[win];
	ms->massregs++;
}

static void enter_hunt(t3_ms_t *ms, uint32_t now, bool retry)
{
	if (ms->ax.state != T3_AX_IDLE) {
		finish(ms, now, T3_RES_LOST);
	}
	ms->state = T3_HUNT;
	ms->confirmed = false;
	ms->slc_here = false;
	ms->asleep = false;
	ms->massreg = false;	// another TSCC cancels it (6.4.5.1)
	ms->tscc_slots = 0;
	ms->csc_valid = false;
	ms->retry_tscc = retry && ms->tscc.rx_hz != 0;
	ms->hunt_strict = false;
	ms->vote.on = false;
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
	ms->rnd = cfg->seed != 0 ? cfg->seed : (cfg->own_id ^ (now * 2654435761u)) | 1u;
	t3_learn_clear(&ms->learned);
	enter_hunt(ms, now, false);
}

void t3_ms_stop(t3_ms_t *ms)
{
	ms->state = T3_OFF;
	ms->nq = 0;
	ms->tx.count = 0;
	ms->tx.udt = false;
	ms->ax.state = T3_AX_IDLE;
	ms->confirmed = false;
}

// ---- Which calls to follow ----

static bool is_allcall(uint32_t id)
{
	return id == T3_ID_ALLMSID || id == T3_ID_ALLMSIDL || id == T3_ID_ALLMSIDZ;
}

// A call the radio makes to this talkgroup is a broadcast: an all-call (6.6.1.7), or a talkgroup set so
static bool is_broadcast(const t3_ms_t *ms, uint32_t tg)
{
	if (is_allcall(tg)) {
		return true;
	}
	for (uint8_t i = 0; i < ms->cfg->ntg; i++) {
		if (ms->cfg->tg[i].id == tg) {
			return (ms->cfg->tg[i].flags & T3_TG_BROADCAST) != 0;
		}
	}
	return false;
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
	return t3_ms_is_dgna(ms, tg);
}

bool t3_ms_is_dgna(const t3_ms_t *ms, uint32_t tg)
{
	if (tg == T3_ID_NULL) {
		return false;
	}
	for (uint8_t i = 0; i < ms->ndgna; i++) {
		if (ms->dgna[i] == tg) {
			return true;
		}
	}
	return tg == ms->dgna_alias_tg;
}

static bool wants_grant(const t3_ms_t *ms, const t3_csbk_t *p, bool *group)
{
	const t3_grant_t *g = &p->u.grant;
	if (ms->stunned) {
		return false;	// no user services, received ones included (6.4.9.0)
	}
	switch (p->op) {
	case T3_OP_TV_GRANT:
	case T3_OP_BTV_GRANT:
		// 6.6.2.2.7: a talkgroup we follow, or the call the radio itself asked for
		*group = true;
		if (is_me(ms, g->src)) {
			return true;
		}
		if (g->late_entry && ms->left_dst != 0 && g->dst == ms->left_dst) {
			// The user left this call: its late-entry grants don't pull the radio back (6.6.2.4.1)
			return false;
		}
		return t3_ms_wants_group(ms, g->dst, g->emergency);
	case T3_OP_PV_GRANT:
		// Either party: a call to the radio, or one it made
		*group = false;
		return is_me(ms, g->src) || ((ms->cfg->opts & T3_OPT_PRIVATE) && is_me(ms, g->dst));
	case T3_OP_PD_GRANT:
	case T3_OP_PD_GRANT_MI:	// multi-item: the same applicability (6.6.3.2.7)
		// 6.6.3.2.7 a: packet data to the radio, or that it sends
		*group = false;
		return is_me(ms, g->src) || is_me(ms, g->dst);
	case T3_OP_TD_GRANT:
	case T3_OP_TD_GRANT_MI:
		// 6.6.3.2.7 b: a talkgroup the radio listens to, or its own data to one
		*group = true;
		return is_me(ms, g->src) || t3_ms_wants_group(ms, g->dst, g->emergency);
	default:
		// The radio is simplex: it never asks for a duplex call and refuses a duplex availability check
		// (6.6.9.2.5), so a duplex grant (53, 54) is never its own; nor a grant to a gateway or another MS
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

// The grant or user data that follows supplementary data to the same destination names its sender (6.4.13.2)
static void supl_sender(t3_ms_t *ms, uint32_t now, uint32_t src, uint32_t dst, bool group)
{
#if !CONFIG_DMR_TRUNK_SUPL
	(void)ms, (void)now, (void)src, (void)dst, (void)group;
#else
	if (ms->supl_rx.count != 0 && ms->supl_rx.src == 0 && !since(now, ms->supl_rx.ms, T3_SUPL_HOLD_MS) &&
	    ms->supl_rx.dst == dst && ms->supl_rx.group == group) {
		ms->supl_rx.src = src;
	}
#endif
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
	supl_sender(ms, now, g->src, g->dst, group);
	ms->call.emergency = g->emergency;
	ms->call.chan = ch;
	ms->call.slot = g->slot;
	ms->call.start_ms = now;
	ms->call.ours = is_me(ms, g->src);
#if CONFIG_DMR_TRUNK_GW
	// The CLI download comes in the call set-up, just before the grant (6.6.4.2, figure 6.45)
	ms->call.cli = ms->cli.ms != 0 && !since(now, ms->cli.ms, T3_CLI_CALL_MS) && ms->cli.gw == g->src &&
		       ms->cli.dst == g->dst && ms->cli.group == group;
#endif
	ms->call.data = p->op == T3_OP_PD_GRANT || p->op == T3_OP_TD_GRANT || p->op == T3_OP_PD_GRANT_MI ||
			p->op == T3_OP_TD_GRANT_MI;
	ms->data_end_ms = 0;
	// The private grant after an accepted ALS radio check, same caller, in time: an ALS call (6.6.2.6.2)
	ms->call.als = !group && !ms->call.data && ms->als_until != 0 && before(now, ms->als_until) &&
		       g->src == ms->als_src && is_me(ms, g->dst);
	// The grant for the radio's own ALS request: it listens (and may end it, 6.4.14.3.4.3)
	ms->call.als_ours = ms->call.ours && !group && !ms->call.data && ms->ax.state != T3_AX_IDLE && ms->ax.als;
	if (ms->call.als) {
		uint32_t s = g->emergency ? or_default(ms->cfg->als_e_s, T3_ALS_E_S) : or_default(ms->cfg->als_s, T3_ALS_S);
		ms->call.als_end = now + s * 1000u;
		ms->als_calls++;
	}
	ms->als_until = 0;
	ms->protect_dis = false;
	ms->protect_other_ms = 0;
	if (g->dst != ms->left_dst || !g->late_entry) {
		ms->left_dst = 0;
	}
	ms->state = T3_FOLLOW;
	ms->last_act_ms = now;
	ms->asleep = false;
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
	if (ms->ax.state != T3_AX_IDLE && !is_me(ms, p->u.grant.src) && !p->u.grant.emergency) {
		// Asking for a service: someone else's call doesn't take the radio off the TSCC meanwhile
		return;
	}
	t3_chan_t c;
	if (chan != NULL) {
		c = *chan;
	} else if (p->u.grant.lpcn == T3_LPCN_ABSOLUTE ||
		   !chan_lookup(ms, p->u.grant.lpcn, &c)) {
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
	} else if (p->u.grant.late_entry && just_left(ms, now, &p->u.grant, &c)) {
		// Only a Late_Entry repeat of the call just left; a set-up grant (Late_Entry 0) is a new call (6.6.2.4.1)
		return;
	}
	follow(ms, now, p, &c, group);
	if (ms->call.ours && ms->ax.state != T3_AX_IDLE &&
	    (ms->call.data ? ms->ax.svc == T3_SVC_PDATA : is_voice(ms->ax.svc))) {
		finish(ms, now, T3_RES_GRANTED);
	} else if (ms->ax.state != T3_AX_IDLE) {
		// An emergency call took us off the TSCC: what we were asking for is lost
		finish(ms, now, T3_RES_LOST);
	}
}

// ---- Power save (6.4.7) ----

// Traffic for the radio: awake T_Awake from now
static void ps_wake(t3_ms_t *ms, uint32_t now)
{
	ms->awake_until = now + or_default(ms->cfg->awake_ms, T3_AWAKE_MS);
	if (ms->asleep) {
		ms->woke_ms = now;
	}
	ms->asleep = false;
}

// The power save frame (480 ms, four Common_Slot_Counter steps) is the radio's when PS_Counter (CSC / 4) masked to
// PowerSave_RQ bits equals PowerSave_Offset. The CSC is carried on from the last short LC heard, one step per 120 ms.
// The radio wakes two steps early, so the receiver has synchronized before its frame
#define T3_CSC_STEP_MS		120u
#define T3_PS_EARLY_STEPS	2u
static bool ps_frame(const t3_ms_t *ms, uint32_t now)
{
	uint32_t csc = (ms->csc + (now - ms->csc_ms) / T3_CSC_STEP_MS) & 511u;
	uint32_t mask = (1u << ms->ps_rq) - 1u;
	if (((csc >> 2) & mask) == ms->ps_offset) {
		return true;
	}
	return (csc & 3u) >= 4u - T3_PS_EARLY_STEPS && ((((csc >> 2) + 1u) & mask) == ms->ps_offset);
}

static bool ps_may_sleep(const t3_ms_t *ms, uint32_t now)
{
	return ms->ps_offset != 0 && ms->state == T3_CAMPED && ms->confirmed && ms->reg == T3_REG_OK &&
	       ms->csc_valid && ms->ax.state == T3_AX_IDLE && ms->nq == 0 && ms->tx.count == 0 && !ms->tx.udt &&
	       !ms->udt_rx.open &&
	       !(ms->massreg && (int32_t)(now - ms->massreg_at) >= 0) && (int32_t)(now - ms->awake_until) >= 0 &&
	       !ps_frame(ms, now);
}

bool t3_ms_asleep(const t3_ms_t *ms)
{
	return ms->asleep;
}

static void return_to_tscc(t3_ms_t *ms, uint32_t now)
{
	ms->cleared = ms->call;
	ms->cleared_ms = now != 0 ? now : 1;
	ms->data_end_ms = 0;
	ms->protect_dis = false;
	ms->protect_other_ms = 0;
	ms->state = T3_CAMPED;
	ms->last_valid_ms = now;
	ms->st.returns++;
	ps_wake(ms, now);	// T_Awake after the call (6.4.7.2.1)
	tune(ms, now, &ms->tscc, 0, false);
}

// ---- The control channel ----

// The registration area of a system code: MODEL and NET (so two networks' areas never match) followed by SYS_AREA,
// the DMRLA most significant bits of SITE (t3_sys_area, 6.4.4). A new area needs a new registration. DMRLA 0 is
// reserved (table 6.5: 1 at least), so unset means every site is an area of its own: the radio registers wherever it
// moves, never missing one
static uint16_t reg_area(const t3_ms_t *ms, uint16_t code)
{
	t3_syscode_t s;
	t3_syscode_split(code, &s);
	uint8_t sb = t3_site_bits(s.model);
	uint8_t la = ms->cfg->dmrla != 0 && ms->cfg->dmrla < sb ? ms->cfg->dmrla : sb;
	return (uint16_t)(t3_syscode_14(code) >> (sb - la));
}

static bool area_denied(const t3_ms_t *ms, uint16_t area)
{
	for (uint8_t i = 0; i < T3_DENIED_MAX; i++) {
		if (ms->denied[i] == (uint16_t)(area + 1u)) {
			return true;
		}
	}
	return false;
}

static void set_reg(t3_ms_t *ms, bool reg)
{
	uint16_t area = reg_area(ms, ms->syscode);
	if (!reg) {
		ms->reg = T3_REG_NOT_NEEDED;
	} else if (!may_send(ms)) {	// listen-only, or a build without access
		ms->reg = T3_REG_LISTEN;
	} else if (area_denied(ms, area)) {
		ms->reg = T3_REG_DENIED;
	} else if (ms->reg != T3_REG_OK || ms->reg_area != area) {
		ms->reg = T3_REG_NEEDED;
	}
}

// ---- Access: random access requests and their answers (6.2, 6.4.4, 6.6.1; CONFIG_DMR_TRUNK_TX) ----

const char *t3_res_name(t3_res_t r)
{
	switch (r) {
	case T3_RES_NONE: return "none";
	case T3_RES_PENDING: return "pending";
	case T3_RES_GRANTED: return "granted";
	case T3_RES_DONE: return "done";
	case T3_RES_REFUSED: return "refused";
	case T3_RES_NO_ANSWER: return "no-answer";
	case T3_RES_TIMEOUT: return "timeout";
	case T3_RES_NOT_ALLOWED: return "not-allowed";
	case T3_RES_LOST: return "lost";
	case T3_RES_CANCELED: return "canceled";
	case T3_RES_DIVERTED: return "diverted";
	}
	return "?";
}

static uint32_t rnd(t3_ms_t *ms)
{
	uint32_t x = ms->rnd != 0 ? ms->rnd : 0x2545F491u;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	ms->rnd = x;
	return x;
}

static bool is_voice(t3_svc_t s)
{
	return s == T3_SVC_GROUP_VOICE || s == T3_SVC_IND_VOICE;
}

// A call service: it ends in a grant, and its progress acknowledgements run TP_Timer
static bool is_call(t3_svc_t s)
{
	return is_voice(s) || s == T3_SVC_PDATA;
}

static const t3_tg_t *talk_group(const t3_ms_t *ms)
{
	for (uint8_t i = 0; i < ms->cfg->ntg; i++) {
		if (ms->cfg->tg[i].flags & T3_TG_TALK) {
			return &ms->cfg->tg[i];
		}
	}
	return NULL;
}

// The talkgroups a registration subscribes to as a list (6.4.4.1.13.2): those listened to, in order, up to seven
static uint8_t reg_list(const t3_ms_t *ms, uint32_t out[T3_REG_TG_MAX])
{
	uint8_t n = 0;
	for (uint8_t i = 0; i < ms->cfg->ntg && n < T3_REG_TG_MAX; i++) {
		if ((ms->cfg->tg[i].flags & T3_TG_LISTEN) && ms->cfg->tg[i].id < T3_ID_GATEWAY_FIRST) {
			out[n++] = ms->cfg->tg[i].id;
		}
	}
	return n;
}

// Blocks for a list of n addresses (B.3.2): the first holds 3 after its flags octet, two hold 7
static uint8_t reg_list_blocks(uint8_t n)
{
	return n <= 3u ? 1u : 2u;
}

// Back to the call left for a payload interrupt (ms->call still holds it)
static void resume_call(t3_ms_t *ms, uint32_t now)
{
	ms->state = T3_FOLLOW;
	ms->last_act_ms = now;
	ms->asleep = false;
	ms->left_dst = 0;
	tune(ms, now, &ms->call.chan, ms->call.slot, true);
}

static void finish(t3_ms_t *ms, uint32_t now, t3_res_t r)
{
	t3_svc_t svc = ms->ax.svc;
	ms->ax.state = T3_AX_IDLE;
	ms->ax.result = r;
	if (ms->ax.resume) {
		// 6.4.14.3.3.2: back to the payload channel whatever the answer; not when the TSCC was lost or an
		// emergency grant took the radio elsewhere
		ms->ax.resume = false;
		if (ms->state == T3_CAMPED) {
			resume_call(ms, now);
		}
	}
	if (svc == T3_SVC_REGISTER && r != T3_RES_DONE && ms->reg != T3_REG_DENIED) {
		// No record was made: try again later (6.4.4.1: no answer leaves the record as it was)
		ms->ax.retry_ms = (now + T3_REG_RETRY_MS) | 1u;
	}
}

static void vote_end(t3_ms_t *ms, uint32_t now, bool moving);

static void start_access(t3_ms_t *ms, uint32_t now, t3_svc_t svc, uint32_t dst, bool emergency)
{
	if (ms->vote.on) {
		vote_end(ms, now, false);	// back on our TSCC first
	}
	ps_wake(ms, now);
	ms->ax.state = T3_AX_WAIT_SLOT;
	ms->ax.svc = svc;
	ms->ax.dst = dst;
	ms->ax.emergency = emergency;
	ms->ax.als = false;
	ms->ax.resume = false;
	ms->ax.ip = 0;
	ms->ax.ip_inform = false;
	ms->ax.supl = ms->cfg->supl_len > 0 && ms->cfg->supl_data != NULL &&
		      (svc == T3_SVC_GROUP_VOICE || svc == T3_SVC_IND_VOICE || svc == T3_SVC_PDATA || svc == T3_SVC_SDATA ||
		       svc == T3_SVC_STATUS);
	// The first slot the TSCC uses; a dedicated TSCC on both slots is reached on TS1
	ms->ax.slot = (ms->tscc_slots & 1u) || ms->tscc_slots == 0 ? 1 : 2;
	ms->ax.frames = 0;
	ms->ax.to_air = false;
	ms->ax.tries = 0;
	ms->ax.tc_deadline = now + T3_TRAND_TC_MS;
	ms->ax.ack_deadline = 0;
	ms->ax.result = T3_RES_PENDING;
	ms->ax.reason = 0;
	ms->ax.progress = 0;
	if (svc == T3_SVC_REGISTER) {
		ms->ax.retry_ms = 0;
	}
}

// 6.2.2: may this inbound slot carry the request? Every request needs an Aloha heard on this TSCC first and a free
// AT bit; then the Aloha's Mask and Service Function. An emergency ignores both unless the Mask is 24 (all barred)
static bool permitted(const t3_ms_t *ms, bool at_known, bool at_busy)
{
	if (!at_known || at_busy || !ms->aloha_here) {
		return false;
	}
	const t3_aloha_t *a = &ms->aloha;
	if (ms->ax.emergency && a->mask < 24) {
		return true;
	}
	if (!t3_mask_match(a->mask, a->ms, my_id(ms))) {
		return false;
	}
	if (ms->ax.emergency) {
		return true;
	}
	switch (ms->ax.svc) {
	case T3_SVC_REGISTER:
	case T3_SVC_DEREGISTER:
		return true;	// every Service Function lets registration through
	case T3_SVC_GROUP_VOICE:
	case T3_SVC_IND_VOICE:
	case T3_SVC_PDATA:	// packet data needs a payload channel too (table 6.1)
		return a->service_fn == 0 || a->service_fn == 1;
	default:
		return a->service_fn == 0 || a->service_fn == 2;
	}
}

// The UDT blocks len octets need: the last block's CRC takes 2
static uint8_t udt_blocks(uint8_t len)
{
	return (uint8_t)((len + 2u + T3_UDT_BLOCK - 1u) / T3_UDT_BLOCK);
}

static void build_rand(t3_ms_t *ms, uint8_t out[T3_CSBK_BYTES])
{
	t3_csbk_t p;
	memset(&p, 0, sizeof(p));
	p.lb = true;
	p.op = T3_OP_C_RAND;
	t3_rand_t *q = &p.u.rand;
	q->src = my_id(ms);
	q->dst = ms->ax.dst;
	switch (ms->ax.svc) {
	case T3_SVC_REGISTER:
	case T3_SVC_DEREGISTER: {
		// Table 6.9: register to the system code, or subscribe to the talk group
		const t3_tg_t *tg = talk_group(ms);
		uint32_t list[T3_REG_TG_MAX];
		uint8_t n = reg_list(ms, list);
		q->kind = T3_SK_REGISTRATION;
		q->options = ms->ax.svc == T3_SVC_REGISTER ? T3_SO_REG_REGISTER : 0;
		ms->reg_tgs = 0;
		ms->reg_single = false;
		ms->ps_rq_sent = 0;
		if ((ms->cfg->opts & T3_OPT_REG_TG) && ms->ax.svc == T3_SVC_REGISTER && n >= 2u) {
			// A list (6.4.4.1.13.2): to TATTSI, with the blocks the TSCC's C_AHOY will ask for
			q->extra = (uint8_t)((T3_TRG_TATTSI << 2) | (reg_list_blocks(n) - 1u));
			q->dst = T3_ID_TATTSI;
			ms->reg_tgs = n;
		} else if ((ms->cfg->opts & T3_OPT_REG_TG) && (tg != NULL || n == 1u)) {
			// One talkgroup (6.4.4.1.13.1): the one talked on, else the only one listened to
			q->extra = T3_TRG_TALKGROUP << 2;
			q->dst = tg != NULL ? tg->id : list[0];
			ms->reg_single = ms->ax.svc == T3_SVC_REGISTER;
		} else {
			q->extra = T3_TRG_SYSCODE << 2;
			q->dst = ms->syscode;
		}
		if (ms->ax.svc == T3_SVC_REGISTER && ms->reg_tgs == 0 && !ms->reg_single && ms->cfg->ps_rq != 0) {
			// Power save (6.4.7.2.1). Not with a talkgroup (6.4.4.1.13.1 sets PS_RQ 000) or a list (its answer's
			// Response_Info is the list's)
			ms->ps_rq_sent = ms->cfg->ps_rq & 7u;
			q->options |= (uint8_t)(ms->ps_rq_sent << 1);
		}
		if (ms->ax.svc == T3_SVC_REGISTER && ms->reg_tgs == 0 && !ms->reg_single && ms->cfg->ip_addr != 0) {
			// IP Connection Advice (6.4.11): IPI asks for the address. Not with a talkgroup or a list, whose
			// answers are their own
			q->options |= T3_SO_REG_IP_INFORM;
			ms->ax.ip_inform = true;
		}
		break;
	}
	case T3_SVC_GROUP_VOICE:
		q->kind = T3_SK_TG_VOICE;
		q->options = (uint8_t)((ms->ax.emergency ? T3_SO_EMERGENCY : 0) |
				       (is_broadcast(ms, ms->ax.dst) ? T3_SO_BROADCAST : 0));
		break;
	case T3_SVC_IND_VOICE:
		// Table 6.40: SUPED_VAL 0, then ALS_SERV
		q->kind = T3_SK_IND_VOICE;
		q->options = ms->ax.emergency ? T3_SO_EMERGENCY : 0;
		q->extra = ms->ax.als ? 0x2u : 0u;
		break;
	case T3_SVC_PDATA:
		// Table 6.50: single slot, single item, normal priority; supplementary data is added below when offered.
		// To IPI (multi-part, 6.6.3.2.3) the Proxy Flag 0 says the extended address is IPv4
		q->kind = ms->ax.group ? T3_SK_TG_DATA : T3_SK_IND_DATA;
		q->options = ms->ax.emergency ? T3_SO_EMERGENCY : 0;
		q->proxy = false;
		break;
	case T3_SVC_SDATA:
		// Table 6.55: no options; the 4 service-dependent bits are SUPED_VAL 0 and the blocks to upload less one
		q->kind = ms->ax.group ? T3_SK_TG_UDT : T3_SK_IND_UDT;
		q->options = 0;
		q->extra = (uint8_t)((udt_blocks(ms->sd_tx.len) - 1u) & 3u);
		break;
	case T3_SVC_SDPOLL:
		// Table 6.56: POL_FMT in the options; Appended_Short_Data: the blocks less one
		q->kind = T3_SK_UDT_POLL;
		q->options = ms->ax.status & 0x0Fu;
		q->extra = ms->ax.info & 3u;
		break;
	case T3_SVC_DIVERT:
		// Table 6.61: no emergency or privacy; DIVONOFF and the services; SDATA_VAL 0 (the address fits one
		// appended block)
		q->kind = T3_SK_DIVERT;
		q->options = ms->ax.div & 0x1Fu;
		q->extra = 0;
		break;
	case T3_SVC_STATUS:
		// Tables 6.57, 6.58: G/I, SUPED_SV and the status's 5 most significant bits; its 2 least significant where a
		// registration has Appended_UDT_Short_Data, SUPED_VAL above them
		q->kind = T3_SK_STATUS;
		q->options = (uint8_t)((ms->ax.group ? 0x40u : 0u) | ((ms->ax.status >> 2) & 0x1Fu));
		q->extra = (uint8_t)(ms->ax.status & 3u);
		break;
	default:
		q->kind = T3_SK_CANCEL;
		break;
	}
#if CONFIG_DMR_TRUNK_GW
	if (t3_dial_gw(ms->ax.dst)) {
		q->proxy = ms->cfg->dial_n > 20u;	// one appended block holds 20 digits, two 44 (6.6.2.1.2)
	}
#endif
	if (ms->ax.supl) {
		// SUPED_SV, and SUPED_VAL the blocks it needs less one, above the service's 2 low bits (tables 6.40, 6.50,
		// 6.55, 6.57); a status has SUPED_SV above its 5 bits
		q->options |= ms->ax.svc == T3_SVC_STATUS ? T3_SO_STATUS_SUPPL : T3_SO_SUPPL_DATA;
		q->extra = (uint8_t)((q->extra & 3u) | ((udt_blocks(ms->cfg->supl_len) - 1u) & 3u) << 2);
	}
	t3_csbk_build(&p, T3_CRC_MASK_CSBK, out);
}

// The answer to a random access burst comes in one of the NWait + 1 frames after the burst's own (6.2.1.1.5, figure
// 6.7: NRand_Wait 1 is "one of the next two TDMA frames"). The wait counts down at the access slot's tick and the next
// try is decided at a tick: the count holds that last frame's tick too, so its answer lands before the decision
static uint16_t ra_window(const t3_ms_t *ms)
{
	return (uint16_t)(t3_nwait_frames(ms->aloha.nrand_wait) + 2u);
}

static void ra_slot(t3_ms_t *ms, uint32_t now, bool at_known, bool at_busy)
{
	switch (ms->ax.state) {
	case T3_AX_WAIT_SLOT: {
		if (ms->ax.frames > 0) {
			ms->ax.frames--;
			return;
		}
		if (!permitted(ms, at_known, at_busy)) {
			// Keep the slot already chosen: the next permitted one goes (6.2.2)
			return;
		}
		uint8_t b[T3_CSBK_BYTES];
		build_rand(ms, b);
		send(ms, b, ms->ax.slot, false);
		ms->ax.tries++;
		ms->st.ra_tries++;
		uint8_t k = (uint8_t)((ms->st.ra_tries - 1u) % T3_RA_LOG);
		ms->st.ra_log[k].tx_ms = now;
		ms->st.ra_log[k].tx_slot = ms->ax.slot;
		ms->st.ra_log[k].frames = T3_RA_NONE;
		ms->st.ra_log[k].rx_slot = 0;
		ms->st.ra_log[k].rx_ms = 0;
		ms->st.ra_log[k].op = 0;
		ms->st.ra_log[k].air_ms = 0;
		ms->ax.to_air = true;
		ms->ax.state = T3_AX_WAIT_RESP;
		ms->ax.frames = ra_window(ms);
		if (ms->ax.svc == T3_SVC_REGISTER) {
			// 6.4.4.1: the old record goes as soon as the request is sent
			ms->reg = T3_REG_NEEDED;
			ms->reg_area = 0;
			ms->reg_tg_ok = 0;
			ms->ip_advised = false;
		}
		break;
	}
	case T3_AX_WAIT_RESP: {
		if (ms->ax.frames > 1) {
			ms->ax.frames--;
			return;
		}
		uint8_t max = ms->ax.emergency ? T3_NRAND_NE : T3_NRAND_NR;
		if (ms->ax.tries >= max) {
			finish(ms, now, T3_RES_NO_ANSWER);
			return;
		}
		// Back off a random number of frames before the next try (table 6.3)
		ms->ax.state = T3_AX_WAIT_SLOT;
		ms->ax.frames = (uint16_t)(rnd(ms) % t3_backoff_frames(ms->aloha.backoff));
		break;
	}
	default:
		break;
	}
}

// The registration's record is made: the area it holds for
static void registered(t3_ms_t *ms, uint32_t now)
{
	ms->reg = ms->reg_stale ? T3_REG_NEEDED : T3_REG_OK;	// stale: the list sent is no longer ours
	ms->reg_stale = false;
	ms->reg_area = reg_area(ms, ms->syscode);
	ms->awake_until = now + or_default(ms->cfg->awake_ms, T3_AWAKE_MS);
}

// C_ACKD (and C_NACKD, C_WACKD, C_QACKD: the reason code says which) to the radio
static void on_ack(t3_ms_t *ms, uint32_t now, const t3_ack_t *a)
{
	if (!may_send(ms) || (ms->ax.state != T3_AX_WAIT_RESP && ms->ax.state != T3_AX_ACKED) || !is_me(ms, a->dst)) {
		return;
	}
	t3_svc_t svc = ms->ax.svc;
	ps_wake(ms, now);
	ms->ax.reason = a->reason;
	ms->ax.info = a->info;
	switch (t3_ack_class(a->reason)) {
	case T3_WACK:
	case T3_QACK:
		// More to come: TP_Timer for a call, TNP_Timer for the rest; each further one restarts it
		ms->ax.state = T3_AX_ACKED;
		ms->ax.progress = (uint8_t)t3_ack_class(a->reason);
		ms->ax.ack_deadline = now + (is_call(svc) ? T3_TP_MS : T3_TNP_MS);
		break;
	case T3_ACK:
		if (svc == T3_SVC_REGISTER && a->reason != T3_RC_REG_ACCEPTED && a->reason != T3_RC_REG_SUBSCRIPTION) {
			break;	// not a registration answer (table 7.41): wait for one, or TNP_Timer
		}
		if (svc == T3_SVC_REGISTER) {
			registered(ms, now);
			// From IPI, after the address went up: the system took it (6.4.11.1.4)
			ms->ip_advised = ms->ax.ip_inform && a->src == T3_ID_IPI;
			// Reg_Accepted or Reg_Subscription/Attachment: Response_Info says which of a list were taken. Only
			// the first reg_tgs bits mean anything: all taken is 1111111b however many went (6.4.4.1.13.2)
			uint8_t sent_bits = (uint8_t)(0x7Fu & ~((1u << (T3_REG_TG_MAX - ms->reg_tgs)) - 1u));
			ms->reg_tg_ok = ms->reg_tgs != 0 ? (uint8_t)(a->info & sent_bits) : 0u;
			if (ms->reg_single) {
				// One talkgroup: Reg_Accepted took it, Reg_Subscription/Attachment didn't (6.4.4.1.13.1)
				ms->reg_tg_ok = a->reason == T3_RC_REG_ACCEPTED ? 0x40u : 0u;
			}
			// PowerSave_Offset: the wake frame, only in a Reg_Accepted (table 7.41); 0 = no power save (table 6.13)
			ms->ps_rq = a->reason == T3_RC_REG_ACCEPTED ? ms->ps_rq_sent : 0u;
			ms->ps_offset = ms->ps_rq != 0 ? (uint8_t)(a->info & ((1u << ms->ps_rq) - 1u)) : 0u;
		} else if (svc == T3_SVC_DEREGISTER) {
			ms->reg = T3_REG_NEEDED;
			ms->deregistered = true;
		} else if (is_voice(svc) && a->reason == T3_RC_MS_ALERTING) {
			// The called radio rings: the call goes on
			ms->ax.state = T3_AX_ACKED;
			ms->ax.ack_deadline = now + T3_TP_MS;
			break;
		}
		finish(ms, now, T3_RES_DONE);
		break;
	case T3_NACK:
		if (svc == T3_SVC_REGISTER && a->reason == T3_RC_REG_DENIED) {
			// Barred from this area until the engine restarts; find another TSCC (6.4.4.1)
			uint16_t area = reg_area(ms, ms->syscode);
			uint8_t i = 0;
			while (i < T3_DENIED_MAX - 1 && ms->denied[i] != 0 && ms->denied[i] != (uint16_t)(area + 1u)) {
				i++;
			}
			ms->denied[i] = (uint16_t)(area + 1u);
			ms->reg = T3_REG_DENIED;
			finish(ms, now, T3_RES_REFUSED);
			enter_hunt(ms, now, false);
		} else if (svc == T3_SVC_REGISTER && a->reason == T3_RC_IP_CONN_FAILED && ms->ax.ip_inform) {
			// Registered, the address not taken (6.4.11.1 f, 6.4.11.1.4): no IP bearer service here, or the
			// address is another's. The radio stays (the standard leaves it to the radio), reached by its ID;
			// no power save, which only a Reg_Accepted grants
			registered(ms, now);
			ms->ps_rq = 0;
			ms->ps_offset = 0;
			finish(ms, now, T3_RES_DONE);
		} else if (svc == T3_SVC_REGISTER && a->reason == T3_RC_REG_REFUSED) {
			// Look for another TSCC; this one is tried again when the hunt comes back to it
			finish(ms, now, T3_RES_REFUSED);
			enter_hunt(ms, now, false);
		} else {
			if (a->reason == T3_RC_MS_NOT_REGISTERED && svc != T3_SVC_REGISTER && !ms->deregistered &&
			    (ms->reg == T3_REG_OK || ms->reg == T3_REG_NOT_NEEDED)) {
				// The TSCC has no record of the radio (6.4.4.1.12): register again before asking more
				ms->reg = T3_REG_NEEDED;
			}
			finish(ms, now, T3_RES_REFUSED);
		}
		break;
	default:
		break;
	}
}

// C_AHOY on the TSCC or P_AHOY on the payload channel addressed to the radio: radio check, availability check (OACSU),
// call cancel, selective clear, authentication (refused without K). The answer goes on the inbound slot
// after the next CACH of the slot the AHOY came on, without random access
static void answer_info(t3_ms_t *ms, uint8_t slot, bool payload, uint8_t reason, uint8_t info, uint32_t to,
			bool then_return)
{
	t3_csbk_t p;
	memset(&p, 0, sizeof(p));
	p.lb = true;
	p.op = payload ? T3_OP_P_ACKU : T3_OP_C_ACKU;
	p.u.ack.reason = reason;
	p.u.ack.info = info;
	p.u.ack.dst = to;
	p.u.ack.src = my_id(ms);
	t3_csbk_build(&p, T3_CRC_MASK_CSBK, ms->tx.burst);
	ms->tx.count = 1;
	ms->tx.slot = slot;
	ms->tx.payload = payload;
	ms->tx.then_return = then_return;
	ms->tx.udt = false;
}

static void answer(t3_ms_t *ms, uint8_t slot, bool payload, uint8_t reason, uint32_t to, bool then_return)
{
	answer_info(ms, slot, payload, reason, 0, to, then_return);
}

// A status delivered by C_AHOY (6.6.6.1.1.4): Service_Options_Mirror holds its 5 most significant bits, Appended_Blocks
// the 2 least. To the radio it is answered (a poll with the radio's own status, 6.6.6.2.1.3); to a talkgroup it isn't
static bool on_status(t3_ms_t *ms, uint32_t now, uint8_t slot, const t3_ahoy_t *h)
{
	bool to_me = !h->group && is_me(ms, h->dst);
	if (ms->stunned || h->kind != T3_SK_STATUS || (!to_me && !(h->group && t3_ms_wants_group(ms, h->dst, false)))) {
		return false;
	}
	uint8_t v = (uint8_t)(((h->options & 0x1Fu) << 2) | (h->blocks & 3u));
	// The TSCC sends one to a talkgroup more than once, and one to the radio again when the answer was lost: a
	// repeat is answered again but not counted as another
	bool repeat = ms->status_rx.ms != 0 && !since(now, ms->status_rx.ms, T3_REPEAT_MS) &&
		      ms->status_rx.src == h->src && ms->status_rx.dst == h->dst && ms->status_rx.group == h->group &&
		      ms->status_rx.value == v;
	if (!repeat) {
		ms->status_rx.count++;
	}
	ms->status_rx.src = h->src;
	ms->status_rx.dst = h->dst;
	ms->status_rx.group = h->group;
	ms->status_rx.value = v;
	ms->status_rx.ms = now != 0 ? now : 1;
	supl_sender(ms, now, h->src, h->dst, h->group);	// supplementary data from SUPLI came first (6.4.13.2)
	if (to_me && may_send(ms)) {
		if (v == T3_STATUS_POLL) {
			answer_info(ms, slot, false, T3_RC_MS_POLL_ACCEPTED, ms->own_status, h->src, false);
		} else {
			answer(ms, slot, false, T3_RC_MS_ACCEPTED, h->src, false);
		}
	}
	return true;
}

// Load a UDT upload: the header in tx.burst, the blocks in up[], sent as one transmission on the next inbound slot
static void queue_udt(t3_ms_t *ms, uint32_t now, uint8_t slot, t3_udt_t *u, const uint8_t *data, uint8_t len,
		      uint8_t fill, bool count_pad, uint8_t spare, bool access)
{
	uint8_t pad = 0;
	u->op = T3_OP_C_UDTHU;
	u->src = my_id(ms);
	u->blocks = t3_udt_pack(data, len, fill, ms->up, &pad);
	u->pad = count_pad ? (uint8_t)(pad + spare / 4u) : 0u;
	t3_udt_build(u, ms->tx.burst);
	ms->tx.count = 1;
	ms->tx.slot = slot;
	ms->tx.payload = false;
	ms->tx.then_return = false;
	ms->tx.udt = true;
	ms->tx.nblocks = u->blocks;
	if (access) {
		ms->ax.state = T3_AX_ACKED;
		ms->ax.ack_deadline = now + T3_TNP_MS;
	}
}

// The character formats pad with 1111b nibbles and count them (B.3.3-B.3.5, B.3.8, B.3.9); binary, NMEA and IP with
// zeros, uncounted
static bool udt_chars(uint8_t format)
{
	return format == T3_UDTF_BCD || format == T3_UDTF_ISO7 || format == T3_UDTF_ISO8 || format == T3_UDTF_UTF16 ||
	       format == T3_UDTF_DGNA_ALIAS;
}

bool t3_dial_gw(uint32_t id)
{
	return (id >= T3_ID_PSTNI && id <= T3_ID_LINEI) || id == T3_ID_DISPATI || (id >= T3_ID_PSTNDI && id <= T3_ID_DISPATDI);
}

void t3_bcd_text(const uint8_t *bcd, uint8_t n, char *out)
{
	static const char digit[] = "0123456789*#";
	for (uint8_t i = 0; i < n; i++) {
		uint8_t d = (uint8_t)(i & 1u ? bcd[i / 2u] & 0x0Fu : bcd[i / 2u] >> 4);
		out[i] = d < 12u ? digit[d] : '?';
	}
	out[n] = '\0';
}

// A request to a dialing gateway needs the digits, and goes to the gateway as an individual (6.6.2.2.3)
static bool dial_bad(const t3_ms_t *ms, uint32_t dst, bool group)
{
	if (!t3_dial_gw(dst)) {
		return false;
	}
#if CONFIG_DMR_TRUNK_GW
	return group || ms->cfg->dial == NULL || ms->cfg->dial_n == 0;
#else
	(void)ms, (void)group;
	return true;
#endif
}

uint8_t t3_dial_pack(const char *digits, uint8_t bcd[T3_DIAL_MAX / 2])
{
	uint8_t n = 0;
	memset(bcd, 0xFF, T3_DIAL_MAX / 2);	// DigitNULL fills an odd count's last nibble (table 7.46)
	for (; digits[n] != '\0'; n++) {
		char c = digits[n];
		uint8_t d = c >= '0' && c <= '9' ? (uint8_t)(c - '0') : c == '*' ? 10u : c == '#' ? 11u : 0xFFu;
		if (d == 0xFFu || n >= T3_DIAL_MAX) {
			return 0;
		}
		bcd[n / 2u] = (uint8_t)(n & 1u ? (bcd[n / 2u] & 0xF0u) | d : (d << 4) | 0x0Fu);
	}
	return n;
}

// The TSCC asks for the digits of our call through a gateway (6.6.2.1.2 b, tables 6.41 and 6.54: C_AHOY from the
// gateway): a UDT with SF 1, BCD (B.3.3), to the gateway, in the inbound slots that follow
static bool on_dial_ask(t3_ms_t *ms, uint32_t now, uint8_t slot, const t3_ahoy_t *h)
{
	if (!t3_dial_gw(h->src) || h->group || !is_me(ms, h->dst)) {
		return false;
	}
#if CONFIG_DMR_TRUNK_GW
	const t3_cfg_t *c = ms->cfg;
	if (!may_send(ms) || ms->ax.dst != h->src || c->dial == NULL || c->dial_n == 0 ||
	    (ms->ax.svc != T3_SVC_IND_VOICE && ms->ax.svc != T3_SVC_SDATA && ms->ax.svc != T3_SVC_PDATA) ||
	    (ms->ax.state != T3_AX_WAIT_RESP && ms->ax.state != T3_AX_ACKED)) {
		return true;
	}
	t3_udt_t u;
	memset(&u, 0, sizeof(u));
	u.format = T3_UDTF_BCD;
	u.sf = true;
	u.dst = h->src;
	// Pad Nibble: the 1111b nibbles after the digits (table B.3), an odd count's last one among them
	queue_udt(ms, now, slot, &u, c->dial, (uint8_t)((c->dial_n + 1u) / 2u), 0xFFu, true,
		  (uint8_t)(c->dial_n & 1u ? 4u : 0u), true);
#else
	(void)now, (void)slot;
#endif
	return true;
}

// The caller's number from a gateway (CLI, table 6.33): a UDT download with SF 1, BCD, from PSTNI or PABXI (or another
// dialing gateway) to the radio or a talkgroup it follows, ahead of the call. Kept in cli; to the radio with A 1,
// C_ACKU Message_Accepted to the gateway
static bool cli_heard(t3_ms_t *ms, uint32_t now, uint8_t slot, const t3_udt_t *h, bool to_me)
{
	if (h->op != T3_OP_C_UDTHD || !h->sf || !t3_dial_gw(h->src) || ms->stunned ||
	    (!to_me && !(h->group && t3_ms_wants_group(ms, h->dst, false)))) {
		return false;
	}
#if !CONFIG_DMR_TRUNK_GW
	(void)now, (void)slot;
	return true;
#else
	if (!t3_udt_crc_ok((const uint8_t (*)[T3_UDT_BLOCK])ms->udt_rx.blk, h->blocks)) {
		ms->st.crc_bad++;
		return true;
	}
	if (h->format == T3_UDTF_BCD) {
		uint8_t room = (uint8_t)((h->blocks * T3_UDT_BLOCK - 2u) * 2u);
		uint8_t n = h->pad < room ? (uint8_t)(room - h->pad) : 0u;
		n = n < T3_DIAL_MAX ? n : (uint8_t)T3_DIAL_MAX;
		bool repeat = ms->cli.ms != 0 && !since(now, ms->cli.ms, T3_REPEAT_MS) && ms->cli.gw == h->src &&
			      ms->cli.dst == h->dst && ms->cli.n == n && memcmp(ms->cli.bcd, &ms->udt_rx.blk[0][0], (n + 1u) / 2u) == 0;
		if (!repeat) {
			ms->cli.count++;
		}
		ms->cli.gw = h->src;
		ms->cli.dst = h->dst;
		ms->cli.group = h->group;
		ms->cli.n = n;
		memcpy(ms->cli.bcd, &ms->udt_rx.blk[0][0], (n + 1u) / 2u);
		ms->cli.ms = now != 0 ? now : 1;
	}
	if (to_me && h->a && may_send(ms)) {
		answer(ms, slot, false, T3_RC_MESSAGE_ACCEPTED, h->src, false);
	}
	return true;
#endif
}

// The TSCC asks for our short data (6.6.4.4 c, C_AHOY from SDMI): the UDT header and blocks go in the inbound slots
// that follow, which the TSCC keeps for them (figure 6.43). It asks again after a CRC failure
static bool on_upload(t3_ms_t *ms, uint32_t now, uint8_t slot, const t3_ahoy_t *h)
{
	if (h->src != T3_ID_SDMI || h->group || !is_me(ms, h->dst) || h->kind != T3_SK_IND_UDT) {
		return false;
	}
	if (!may_send(ms) || ms->ax.svc != T3_SVC_SDATA ||
	    (ms->ax.state != T3_AX_WAIT_RESP && ms->ax.state != T3_AX_ACKED)) {
		return true;	// no short data request waiting: nothing to upload
	}
	t3_udt_t u;
	memset(&u, 0, sizeof(u));
	u.group = ms->ax.group;
	u.format = ms->sd_tx.format;
	u.dst = ms->ax.dst;
	bool chars = udt_chars(u.format);
	queue_udt(ms, now, slot, &u, ms->sd_tx.data, ms->sd_tx.len, chars ? 0xFFu : 0u, chars, ms->sd_tx.spare, true);
	return true;
}

// The TSCC asks for the supplementary user data offered with our request (6.4.13.1, C_AHOY from SUPLI, the request's
// Service_Kind): a UDT header with SF 1 to SUPLI and its blocks, in the inbound slots that follow. The TSCC may skip it
// and go on with the call (6.4.13.0 a)
static bool on_supl_ask(t3_ms_t *ms, uint32_t now, uint8_t slot, const t3_ahoy_t *h)
{
	if (h->src != T3_ID_SUPLI || h->group || !is_me(ms, h->dst)) {
		return false;
	}
	if (!may_send(ms) || !ms->ax.supl || (ms->ax.state != T3_AX_WAIT_RESP && ms->ax.state != T3_AX_ACKED)) {
		return true;
	}
	const t3_cfg_t *c = ms->cfg;
	t3_udt_t u;
	memset(&u, 0, sizeof(u));
	u.format = c->supl_format;
	u.sf = true;
	u.dst = T3_ID_SUPLI;
	bool chars = udt_chars(u.format);
	queue_udt(ms, now, slot, &u, c->supl_data, c->supl_len, chars ? 0xFFu : 0u, chars, 0, true);
	return true;
}

// A short data poll to the radio (6.6.5.7, C_AHOY kind UDT poll from the calling MS or SDMI): its data as a UDT to the
// caller in the slots the TSCC keeps, cut to the blocks asked for; refused (C_NACKU) when it has none in that format
static bool on_poll(t3_ms_t *ms, uint32_t now, uint8_t slot, const t3_ahoy_t *h)
{
	if (h->kind != T3_SK_UDT_POLL || h->group || !is_me(ms, h->dst)) {
		return false;
	}
	if (!may_send(ms) || ms->stunned) {
		return true;
	}
	const t3_cfg_t *c = ms->cfg;
	uint8_t format = h->options & 0x0Fu;	// POL_FMT (table 7.60), mirrored
	if (c->poll_len == 0 || c->poll_data == NULL || c->poll_format != format) {
		answer(ms, slot, false, T3_RC_MS_NOT_SUPPORTED, h->src, false);
		return true;
	}
	uint8_t room = (uint8_t)((h->blocks & 3u) + 1u) * T3_UDT_BLOCK - 2u;
	t3_udt_t u;
	memset(&u, 0, sizeof(u));
	u.format = format;
	u.dst = h->src;
	bool chars = udt_chars(format);
	queue_udt(ms, now, slot, &u, c->poll_data, c->poll_len < room ? c->poll_len : room, chars ? 0xFFu : 0u, chars, 0,
		  false);
	return true;
}

// The TSCC asks for a diversion's address (6.6.7.1.2.1, C_AHOY from DIVERTI): one UDT block in Address format to
// MSI or GPI, the address in ADDRESS1 (B.3.2), UDT_DIV set; the TSCC's acknowledgement ends the request
static bool on_divert_ask(t3_ms_t *ms, uint32_t now, uint8_t slot, const t3_ahoy_t *h)
{
	if (h->src != T3_ID_DIVERTI || h->group || !is_me(ms, h->dst)) {
		return false;
	}
	if (!may_send(ms) || ms->ax.svc != T3_SVC_DIVERT || !(ms->ax.div & T3_SO_DIV_SET) ||
	    (ms->ax.state != T3_AX_WAIT_RESP && ms->ax.state != T3_AX_ACKED)) {
		return true;
	}
	uint8_t d[10];
	memset(d, 0, sizeof(d));	// octet 0 reserved, OK 0; ADDRESS2 and 3 ADRNULL
	d[1] = (uint8_t)(ms->ax.div_to >> 16);
	d[2] = (uint8_t)(ms->ax.div_to >> 8);
	d[3] = (uint8_t)ms->ax.div_to;
	t3_udt_t u;
	memset(&u, 0, sizeof(u));
	u.format = T3_UDTF_ADDRESS;
	u.opt = true;	// UDT_DIV
	u.dst = ms->ax.group ? T3_ID_GPI : T3_ID_MSI;
	queue_udt(ms, now, slot, &u, d, sizeof(d), 0, false, 0, true);
	return true;
}

// The TSCC asks for a multi-part packet data call's IP address (6.6.3.2.4 b 2, C_AHOY from IPI; Service_Kind 0010b
// as 6.6.3.1.2 b, or 0100b as table 6.54): one UDT block in IP format (B.3.7, figure B.25: the IPv4 address, 48 bits
// reserved) to IPI, supplementary data as table 6.35's gateway rows; the grant or a refusal follows
//
// IP Connection Advice (6.4.11.1 e, 6.4.11.1.3) is the same upload: the C_AHOY from IPI to a registration with
// IP_Inform says the radio is registered and asks for its own address. The text gives no Service_Kind for it: 0100b
// and 1110b are taken. The final C_ACKD Reg_Accepted or C_NACKD IP_Connection_failed from IPI follows (on_ack)
static bool on_ip_ask(t3_ms_t *ms, uint32_t now, uint8_t slot, const t3_ahoy_t *h)
{
	if (h->src != T3_ID_IPI || h->group || !is_me(ms, h->dst) ||
	    (h->kind != T3_SK_IND_DATA && h->kind != T3_SK_IND_UDT && h->kind != T3_SK_REGISTRATION)) {
		return false;
	}
	if (h->kind == T3_SK_IND_DATA &&
	    !(ms->ax.svc == T3_SVC_PDATA && ms->ax.dst == T3_ID_IPI && ms->ax.ip != 0 &&
	      (ms->ax.state == T3_AX_WAIT_RESP || ms->ax.state == T3_AX_ACKED))) {
		return false;	// no IPv4 address asked for: IPI's data to the radio, the availability check (6.6.3.2.6)
	}
	if (!may_send(ms) || (ms->ax.state != T3_AX_WAIT_RESP && ms->ax.state != T3_AX_ACKED)) {
		return true;
	}
	uint32_t ip;
	if (ms->ax.svc == T3_SVC_REGISTER && ms->ax.ip_inform && h->kind != T3_SK_IND_DATA) {
		if (ms->reg != T3_REG_OK) {
			registered(ms, now);
		}
		ip = ms->cfg->ip_addr;
	} else if (ms->ax.svc == T3_SVC_PDATA && ms->ax.dst == T3_ID_IPI && ms->ax.ip != 0 &&
		   h->kind != T3_SK_REGISTRATION) {
		ip = ms->ax.ip;
	} else {
		return true;
	}
	uint8_t d[10];
	memset(d, 0, sizeof(d));
	d[0] = (uint8_t)(ip >> 24);
	d[1] = (uint8_t)(ip >> 16);
	d[2] = (uint8_t)(ip >> 8);
	d[3] = (uint8_t)ip;
	t3_udt_t u;
	memset(&u, 0, sizeof(u));
	u.format = T3_UDTF_IP;
	u.sf = true;
	u.dst = T3_ID_IPI;
	queue_udt(ms, now, slot, &u, d, sizeof(d), 0, false, 0, true);
	return true;
}

// The TSCC asks for the registration's talkgroup list (table 6.11: Service_Kind_Flag 1, from TATTSI): the addresses
// go up as UDT Address Format (B.3.2) to TATTSI, supplementary data that wants no answer of its own; the
// registration's acknowledgement follows
static bool on_reg_list(t3_ms_t *ms, uint32_t now, uint8_t slot, const t3_ahoy_t *h)
{
	if (h->src != T3_ID_TATTSI || h->group || !is_me(ms, h->dst) || h->kind != T3_SK_REGISTRATION || !h->kind_flag) {
		return false;
	}
	if (!may_send(ms) || ms->ax.svc != T3_SVC_REGISTER || ms->reg_tgs == 0 ||
	    (ms->ax.state != T3_AX_WAIT_RESP && ms->ax.state != T3_AX_ACKED)) {
		return true;
	}
	uint32_t list[T3_REG_TG_MAX];
	uint8_t n = reg_list(ms, list);
	if (n != ms->reg_tgs) {
		return true;	// the list changed since the request: the next registration sends the new one
	}
	// Octet 0: reserved and OK 0 (no one-key talkgroup); the addresses after it, the rest ADRNULL
	uint8_t d[1u + 3u * T3_REG_TG_MAX];
	uint8_t slots = reg_list_blocks(n) == 1u ? 3u : T3_REG_TG_MAX;
	memset(d, 0, sizeof(d));
	for (uint8_t i = 0; i < n; i++) {
		d[1u + 3u * i] = (uint8_t)(list[i] >> 16);
		d[2u + 3u * i] = (uint8_t)(list[i] >> 8);
		d[3u + 3u * i] = (uint8_t)list[i];
	}
	t3_udt_t u;
	memset(&u, 0, sizeof(u));
	u.format = T3_UDTF_ADDRESS;
	u.dst = T3_ID_TATTSI;
	u.sf = true;
	queue_udt(ms, now, slot, &u, d, (uint8_t)(1u + 3u * slots), 0, false, 0, true);
	return true;
}

// The radio challenges the TSCC (tables 6.21, 6.25): C_ACKVIT, the acknowledgement to the stun, revive or kill, with a
// random challenge as its target. The TSCC repeating its C_AHOY gets the same challenge again
static void challenge_tscc(t3_ms_t *ms, uint32_t now, uint8_t slot, uint8_t what)
{
	if (ms->acvit != what || ms->acvit_until == 0 || !before(now, ms->acvit_until)) {
		ms->acvit = what;
		ms->acvit_challenge = rnd(ms) % (T3_AUTH_CHALLENGE_MAX + 1u);
	}
	ms->acvit_until = (now + T3_TNP_MS) | 1u;
	t3_csbk_t p;
	memset(&p, 0, sizeof(p));
	p.lb = true;
	p.op = T3_OP_C_ACKVIT;
	p.u.ahoy.kind_flag = what == T3_ACVIT_REVIVE;
	p.u.ahoy.kind = T3_SK_SUPPLEMENTARY;
	p.u.ahoy.dst = ms->acvit_challenge;
	p.u.ahoy.src = my_id(ms);
	t3_csbk_build(&p, T3_CRC_MASK_CSBK, ms->tx.burst);
	ms->tx.count = 1;
	ms->tx.slot = slot;
	ms->tx.payload = false;
	ms->tx.then_return = false;
	ms->tx.udt = false;
}

// The TSCC's answer to the challenge (tables 6.20, 6.24: C_ACKD Authentication_Response, the response in the source
// field). Right: act, and MS_Accepted to STUNI or KILLI; wrong: Recipient_Refused and nothing done. The TSCC may send
// it again when the final acknowledgement is lost: answered again, counted once
static bool on_tscc_auth(t3_ms_t *ms, uint32_t now, uint8_t slot, const t3_ack_t *a)
{
	if (a->reason != T3_RC_AUTH_RESPONSE || !is_me(ms, a->dst) || ms->acvit == T3_ACVIT_NONE ||
	    !before(now, ms->acvit_until) || !may_send(ms)) {
		return false;
	}
	uint32_t want = t3_auth_response(ms->cfg->auth_key, ms->acvit_challenge, ms->cfg->psn);
	uint32_t gw = ms->acvit == T3_ACVIT_KILL ? T3_ID_KILLI : T3_ID_STUNI;
	if (a->src != want) {
		ms->tscc_auth_fails++;
		answer(ms, slot, false, T3_RC_RECIPIENT_REFUSED, gw, false);
		return true;
	}
	switch (ms->acvit) {
	case T3_ACVIT_STUN:
		if (!ms->stunned) {
			ms->stunned = true;
			ms->stuns++;
		}
		break;
	case T3_ACVIT_REVIVE:
		if (ms->stunned) {
			ms->stunned = false;
			ms->revives++;
		}
		break;
	default:
		if (!ms->killed) {
			ms->killed = true;
			ms->kills++;
		}
		break;
	}
	answer(ms, slot, false, T3_RC_MS_ACCEPTED, gw, false);
	return true;
}

static void on_ahoy(t3_ms_t *ms, uint32_t now, uint8_t slot, const t3_ahoy_t *h, bool payload)
{
	if (!payload && !h->group && is_me(ms, h->dst)) {
		ms->st.ahoys++;
		ms->st.ahoy_src = h->src;
		ms->st.ahoy_ms = now != 0 ? now : 1u;
		ms->st.ahoy_kind = h->kind;
	}
	if (!payload && (on_upload(ms, now, slot, h) || on_reg_list(ms, now, slot, h) ||
			  on_divert_ask(ms, now, slot, h) || on_ip_ask(ms, now, slot, h) || on_poll(ms, now, slot, h) ||
			  on_supl_ask(ms, now, slot, h) || on_dial_ask(ms, now, slot, h))) {
		return;
	}
	if (!payload && (h->kind == T3_SK_STATUS || h->kind == T3_SK_UDT_POLL) && is_me(ms, h->src)) {
		// Our status going to the called party or talkgroup (6.6.6.1.2.1.2 c), or our poll (6.6.5.4 b): it is under
		// way, the final answer (the polled data) follows. Not a status heard, though the talkgroup's may be ours
		t3_svc_t want = h->kind == T3_SK_STATUS ? T3_SVC_STATUS : T3_SVC_SDPOLL;
		if (ms->ax.svc == want && (ms->ax.state == T3_AX_WAIT_RESP || ms->ax.state == T3_AX_ACKED)) {
			ms->ax.state = T3_AX_ACKED;
			ms->ax.ack_deadline = now + T3_TNP_MS;
		}
		return;
	}
	if (!payload && on_status(ms, now, slot, h)) {
		return;
	}
	bool to_me = !h->group && is_me(ms, h->dst);
	bool to_call = payload && h->group && ms->call.group && h->dst == ms->call.dst;
	// A radio check of a talkgroup on the TSCC (6.4.12 b): any member may answer, from its own ID
	bool tg_check = !payload && h->group && h->kind == T3_SK_REGISTRATION && h->src == T3_ID_TSI &&
			t3_ms_wants_group(ms, h->dst, false);
	if (!to_me && !to_call && !tg_check) {
		return;
	}
	ps_wake(ms, now);
	if (h->kind == T3_SK_CANCEL && payload) {
		// Selective clear (6.6.2.3.2.7): MS_Accepted, then leave the channel
		if (may_send(ms)) {
			answer(ms, slot, true, T3_RC_MS_ACCEPTED, h->src, true);
		} else {
			return_to_tscc(ms, now);
		}
		return;
	}
	if (!may_send(ms)) {
		return;
	}
	if (to_me && !payload && h->kind == T3_SK_SUPPLEMENTARY && h->src == T3_ID_KILLI) {
		// Kill (6.4.10.2): only authenticated, and only where allowed; else not supported
		if (!(ms->cfg->opts & T3_OPT_KILL) || !ms->cfg->auth_key_set) {
			answer(ms, slot, false, T3_RC_MS_NOT_SUPPORTED, h->src, false);
		} else {
			challenge_tscc(ms, now, slot, T3_ACVIT_KILL);
		}
	} else if (to_me && !payload && h->kind == T3_SK_SUPPLEMENTARY && h->src == T3_ID_STUNI &&
		   !(ms->cfg->opts & T3_OPT_STUN)) {
		// Stun not accepted (an open system's radios: anyone could send it unauthenticated)
		answer(ms, slot, false, T3_RC_MS_NOT_SUPPORTED, h->src, false);
	} else if (to_me && !payload && h->kind == T3_SK_SUPPLEMENTARY && h->src == T3_ID_STUNI &&
		   ms->cfg->auth_key_set) {
		// With K the radio authenticates the TSCC first (6.4.9.2.2)
		challenge_tscc(ms, now, slot, h->kind_flag ? T3_ACVIT_REVIVE : T3_ACVIT_STUN);
	} else if (to_me && !payload && h->kind == T3_SK_SUPPLEMENTARY && h->src == T3_ID_STUNI) {
		// Stun (Service_Kind_Flag 0) or revive (1), without authentication (6.4.9.1.2)
		ms->stunned = !h->kind_flag;
		if (ms->stunned) {
			ms->stuns++;
		} else {
			ms->revives++;
		}
		answer(ms, slot, false, T3_RC_MESSAGE_ACCEPTED, h->src, false);
	} else if (to_me && h->kind == T3_SK_REGISTRATION && h->src <= T3_AUTH_CHALLENGE_MAX && ms->cfg->auth_key_set) {
		// An authentication challenge (6.4.8.3; on the payload channel answered with P_ACKU, 6.6.2.3.2.2): the source
		// is the challenge, the answer's target the response. One during an access (registration, a call) is
		// progress: the final answer follows within TNP_Timer
		answer(ms, slot, payload, T3_RC_MS_AUTH_RESPONSE,
		       t3_auth_response(ms->cfg->auth_key, h->src, ms->cfg->psn), false);
		ms->auths++;
		if (ms->ax.state == T3_AX_WAIT_RESP || ms->ax.state == T3_AX_ACKED) {
			ms->ax.state = T3_AX_ACKED;
			ms->ax.ack_deadline = now + T3_TNP_MS;
		}
	} else if (h->src == T3_ID_TSI && (h->kind == T3_SK_REGISTRATION ||
					    (payload && h->kind == (h->group ? T3_SK_TG_VOICE : T3_SK_IND_VOICE)))) {
		// Radio check (6.4.12; on the payload channel also tables 6.42, 6.43 and 6.6.2.3.2.1), of the radio or its
		// talkgroup: MS_Accepted from the radio's own ID either way
		answer(ms, slot, payload, T3_RC_MS_ACCEPTED, h->src, false);
	} else if (ms->stunned) {
		return;		// stunned: no user services to answer for
	} else if (to_me && !payload && h->kind == T3_SK_CANCEL) {
		answer(ms, slot, false, T3_RC_MS_ACCEPTED, h->src, false);	// the caller gave up (6.6.1.2.2)
	} else if (to_me && !payload && h->kind == T3_SK_IND_VOICE && h->als) {
		// An ALS radio check (table 6.46): Message_Accepted, and a private grant from the same caller within
		// T_ALS_REQUEST_LIFE_SPAN is an ALS call. Without ALS turned on, not supported (table 6.47)
		if (!(ms->cfg->opts & T3_OPT_ALS)) {
			answer(ms, slot, false, T3_RC_MS_NOT_SUPPORTED, h->src, false);
			return;
		}
		ms->als_src = h->src;
		ms->als_until = (now + T3_ALS_REQUEST_MS) | 1u;
		answer(ms, slot, false, T3_RC_MESSAGE_ACCEPTED, h->src, false);
	} else if (to_me && !payload && h->kind == T3_SK_IND_VOICE) {
		answer(ms, slot, false, T3_RC_MS_ACCEPTED, h->src, false);	// availability check: OACSU
	} else if (payload && h->src == T3_ID_TSI && (h->kind == T3_SK_IND_DATA || h->kind == T3_SK_TG_DATA)) {
		// Packet data radio check on the payload channel (6.6.3.3.2.1; table 6.51, cited there as 6.53)
		answer(ms, slot, true, T3_RC_MESSAGE_ACCEPTED, h->src, false);
	} else if (to_me && !payload && (h->kind == T3_SK_DX_VOICE || h->kind == T3_SK_DX_DATA)) {
		// A full-duplex MS to MS availability check: the radio is simplex (6.6.9.2.5, 6.6.10.2.5)
		answer(ms, slot, false, T3_RC_MS_DUPLEX_NOT_SUPPORTED, h->src, false);
	} else if (to_me && !payload && h->kind == T3_SK_IND_DATA) {
		// Packet data to the radio (6.6.3.2.6): the data service takes it on the payload channel, or the radio
		// refuses when its data terminal can't
		answer(ms, slot, false, ms->data_nack != 0 ? ms->data_nack : T3_RC_MESSAGE_ACCEPTED, h->src, false);
	} else if (to_me) {
		answer(ms, slot, payload, T3_RC_MS_NOT_SUPPORTED, h->src, false);
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
	if (ms->state == T3_HUNT && ms->hunt_strict && ms->cfg->leave_dbm != 0 && ms->level_dbm != 0 &&
	    ms->level_dbm < ms->cfg->leave_dbm) {
		// Hunting away from a weak TSCC: this one is no better (6.3.2.3 a, the hunting threshold)
		hunt_next(ms, now);
		return;
	}
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
			ms->nadj = 0;
			ms->adj_next = 0;
		}
	}
	ms->confirmed = true;
	ms->aloha_here = true;
	ms->lent_ms = 0;	// a lent composite TSCC is back (it comes back with an Aloha)
	ms->syscode = a->syscode;
	ms->aloha = *a;
	ms->tscc_slots |= (uint8_t)(1u << (slot - 1u));
	ms->last_valid_ms = now;
	set_reg(ms, a->reg);
}

// C_MOVE (6.3.3.1 e, D.1.2): an applicable one sends the radio to the TSCC it names, by logical channel or, in an
// MBC, by frequency. Nothing is sent there until an Aloha confirms it; T_Nosig without one starts the hunt. An access
// under way is lost, and so is a mass registration
static void on_move(t3_ms_t *ms, uint32_t now, const t3_move_t *m, const t3_cdef_t *cdef)
{
	uint32_t bits = m->mask >= 24u ? 0xFFFFFFu : ((1u << m->mask) - 1u);
	if (!ms->confirmed || m->mask > 24u || ((my_id(ms) ^ m->ms) & bits) != 0) {
		return;
	}
	t3_chan_t c;
	if (m->lpcn == T3_LPCN_ABSOLUTE) {
		if (cdef == NULL) {
			return;
		}
		c = (t3_chan_t){ .lcn = cdef->chan, .cc = cdef->cc, .rx_hz = cdef->rx_hz, .tx_hz = cdef->tx_hz };
		if (ms->cfg->plan.learn && t3_learn_add(&ms->learned, cdef)) {
			ms->st.learned++;
		}
	} else if (m->lpcn == T3_LPCN_NULL || !chan_lookup(ms, m->lpcn, &c)) {
		return;
	}
	if (c.cc == T3_CC_ANY) {
		c.cc = ms->tscc.cc;
	}
	if (c.rx_hz == ms->tscc.rx_hz && c.tx_hz == ms->tscc.tx_hz) {
		return;
	}
	if (ms->ax.state != T3_AX_IDLE) {
		finish(ms, now, T3_RES_LOST);
	}
	ms->tscc = c;
	ms->confirmed = false;
	ms->massreg = false;
	ms->tscc_slots = 0;
	ms->csc_valid = false;
	ms->moves++;
	ms->last_valid_ms = now;
	tune(ms, now, &ms->tscc, 0, false);
}

// ---- Adjacent sites, Vote Now (6.7.1.3, 6.7.1.7) ----

// An Adjacent_Site or Vote_Now broadcast (tables 7.71, 7.79): Parms1 the adjacent TSCC's 14 system code MSBs; Parms2
// Active_Connection available and its flag, the confirmed and the adjacent channel priority, 4 reserved, the channel
// (4 095: an MBC whose appended block holds the frequencies). Kept when it is the radio's network; the entry's index,
// or -1
static int adj_note(t3_ms_t *ms, const t3_bcast_t *b, const t3_cdef_t *cdef)
{
	uint16_t ch = (uint16_t)(b->parms2 & 0xFFFu);
	t3_chan_t c;
	if (ch == 0) {
		return -1;
	}
	if (ch == T3_LPCN_ABSOLUTE) {
		if (cdef == NULL) {
			return -1;
		}
		c = (t3_chan_t){ .lcn = cdef->chan, .cc = cdef->cc, .rx_hz = cdef->rx_hz, .tx_hz = cdef->tx_hz };
	} else if (!chan_lookup(ms, ch, &c)) {
		return -1;
	}
	uint16_t sys14 = (uint16_t)(b->parms1 & 0x3FFFu);
	if (c.rx_hz == 0 || c.rx_hz == ms->tscc.rx_hz) {
		return -1;
	}
	if (!authorized(ms, (uint16_t)((uint16_t)(sys14 << 2) | (ms->syscode & 3u)))) {
		// A site the radio may not use (its network, sites or category): announced, never sampled
		ms->st.adj_refused++;
		return -1;
	}
	uint8_t i = 0;
	while (i < ms->nadj && ms->adj[i].chan.rx_hz != c.rx_hz) {
		i++;
	}
	if (i == ms->nadj) {
		if (ms->nadj < T3_ADJ_MAX) {
			ms->nadj++;
		} else {
			i = ms->adj_next;
			ms->adj_next = (uint8_t)((ms->adj_next + 1u) % T3_ADJ_MAX);
		}
		memset(&ms->adj[i], 0, sizeof(ms->adj[i]));
	}
	t3_adj_t *a = &ms->adj[i];
	a->chan = c;
	a->sys14 = sys14;
	a->conf_prio = (uint8_t)((b->parms2 >> 19) & 7u);
	a->adj_prio = (uint8_t)((b->parms2 >> 16) & 7u);
	return i;
}

static bool weak(const t3_ms_t *ms, int16_t dbm)
{
	return ms->cfg->leave_dbm != 0 && dbm != 0 && dbm < ms->cfg->leave_dbm;
}

// Whether the adjacent TSCC sampled serves better than ours (home: our level). The algorithm is the radio's to choose
// (6.7.1.3): radiated priorities first (1 highest .. 7; 0 none), then the signal with a margin, so two sites of about
// the same level don't swap the radio back and forth. A worse-priority site only rescues a weak TSCC
static bool vote_better(const t3_ms_t *ms, const t3_adj_t *a, int16_t home)
{
	if (weak(ms, a->dbm)) {
		return false;
	}
	bool stronger = a->dbm != 0 && home != 0 && a->dbm >= home + T3_VOTE_MARGIN_DB;
	if (a->conf_prio != 0 && a->adj_prio != 0 && a->adj_prio != a->conf_prio) {
		return a->adj_prio < a->conf_prio || (weak(ms, home) && stronger);
	}
	return stronger;
}

// Sampling over: move to it (the next Aloha there confirms it, and registers in a new area), or back to our TSCC.
// moving false: back at once (the user wants service)
static void vote_end(t3_ms_t *ms, uint32_t now, bool moving)
{
	ms->vote.on = false;
	t3_adj_t *a = &ms->adj[ms->vote.idx];
	a->dbm = ms->level_dbm;
	a->sampled_ms = now != 0 ? now : 1u;
	if (moving && ms->vote.heard && vote_better(ms, a, ms->vote.home_dbm)) {
		t3_chan_t to = a->chan;
		to.cc = ms->vote.cc;
		// The TSCC left becomes the new one's adjacent site, so a hunt finds it again
		uint8_t conf = a->conf_prio;
		a->chan = ms->tscc;
		a->sys14 = (uint16_t)(ms->syscode >> 2);
		a->conf_prio = a->adj_prio;
		a->adj_prio = conf;
		a->dbm = ms->vote.home_dbm;
		ms->tscc = to;
		ms->tuned.cc = to.cc;
		ms->confirmed = false;
		ms->massreg = false;
		ms->tscc_slots = 0;
		ms->csc_valid = false;
		ms->last_valid_ms = now;
		ms->st.vote_moves++;
		return;
	}
	int16_t home = ms->vote.home_dbm;
	ms->last_valid_ms += now - ms->vote.start;	// T_Nosig doesn't count the time away
	tune(ms, now, &ms->tscc, 0, false);
	ms->level_dbm = home;
}

// A Vote Now (6.7.1.3): an idle radio samples the TSCC named, in the frames the TSCC keeps quiet for it
static void maybe_vote(t3_ms_t *ms, uint32_t now, uint8_t i)
{
	t3_adj_t *a = &ms->adj[i];
	if ((ms->cfg->opts & T3_OPT_NO_ROAM) || ms->state != T3_CAMPED || !ms->confirmed || ms->vote.on ||
	    ms->ax.state != T3_AX_IDLE || ms->nq != 0 || ms->tx.count != 0 || ms->udt_rx.open ||
	    (a->sampled_ms != 0 && !since(now, a->sampled_ms, T3_VOTE_AGAIN_MS) && !weak(ms, ms->level_dbm))) {
		return;
	}
	ms->vote.on = true;
	ms->vote.idx = i;
	ms->vote.start = now;
	ms->vote.until = now + or_default(ms->cfg->vote_ms, T3_VOTE_MS);
	ms->vote.heard = false;
	ms->vote.cc = 0;
	ms->vote.home_dbm = ms->level_dbm;
	ms->st.votes++;
	tune(ms, now, &a->chan, 0, false);
}

// A PDU heard on the TSCC being sampled: only whether it is the site announced
static void on_vote(t3_ms_t *ms, uint8_t cc, const t3_csbk_t *p)
{
	uint16_t code;
	if (p->op == T3_OP_C_ALOHA) {
		code = p->u.aloha.syscode;
	} else if (p->op == T3_OP_C_BCAST) {
		code = p->u.bcast.syscode;
	} else {
		return;
	}
	const t3_adj_t *a = &ms->adj[ms->vote.idx];
	if (authorized(ms, code) && (code >> 2) == a->sys14) {
		ms->vote.heard = true;
		ms->vote.cc = cc;
	}
}

void t3_ms_level(t3_ms_t *ms, uint32_t now, int16_t dbm)
{
	// Asleep the receiver may be off, and just awake it is still settling: its readings are of no signal, and taken
	// they would roam the radio off a good TSCC
	if (dbm == 0 || ms->state == T3_OFF || ms->state == T3_FOLLOW || !since(now, ms->tuned_ms, T3_SETTLE_MS) ||
	    ms->asleep || !since(now, ms->woke_ms, T3_SETTLE_MS)) {
		return;
	}
	ms->level_dbm = ms->level_dbm == 0 ? dbm : (int16_t)((3 * (int32_t)ms->level_dbm + dbm) / 4);
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
		ms->aloha.backoff = p->u.bcast.backoff;
		set_reg(ms, p->u.bcast.reg);
		if (p->u.bcast.type == T3_ANN_MASS_REG) {
			on_mass_reg(ms, now, &p->u.bcast);
		}
		if (p->u.bcast.type == T3_ANN_ADJACENT || p->u.bcast.type == T3_ANN_VOTE_NOW) {
			int i = adj_note(ms, &p->u.bcast, cdef);
			if (i >= 0 && p->u.bcast.type == T3_ANN_VOTE_NOW) {
				maybe_vote(ms, now, (uint8_t)i);
			}
		}
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
	if (p->op == T3_OP_C_ACKD) {
		if (on_tscc_auth(ms, now, slot, &p->u.ack)) {
			return;
		}
		on_ack(ms, now, &p->u.ack);
		return;
	}
	if (p->op == T3_OP_C_AHOY) {
		on_ahoy(ms, now, slot, &p->u.ahoy, false);
		return;
	}
	if (p->op == T3_OP_C_MOVE) {
		on_move(ms, now, &p->u.move, cdef);
		return;
	}
	if (is_grant(p->op)) {
		if (cdef != NULL) {
			t3_chan_t c = { .lcn = cdef->chan, .cc = cdef->cc, .rx_hz = cdef->rx_hz, .tx_hz = cdef->tx_hz };
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
			bool to = false;
			if (p->u.clear.lpcn == T3_LPCN_ABSOLUTE && cdef != NULL) {
				// An MBC: the appended block gives the TSCC's frequencies
				c = (t3_chan_t){ .lcn = cdef->chan, .cc = cdef->cc, .rx_hz = cdef->rx_hz, .tx_hz = cdef->tx_hz };
				to = true;
			} else if (p->u.clear.lpcn != T3_LPCN_NULL && p->u.clear.lpcn != T3_LPCN_ABSOLUTE &&
				   p->u.clear.lpcn != ms->tscc.lcn) {
				to = chan_lookup(ms, p->u.clear.lpcn, &c);
			}
			if (to) {
				// Go to another TSCC (7.1.1.1.4): it must still confirm with an Aloha before T_Nosig
				if (c.cc == T3_CC_ANY) {
					c.cc = ms->tscc.cc;
				}
				ms->tscc = c;
			}
			return_to_tscc(ms, now);
		}
		break;
	case T3_OP_P_MAINT:
		// Another radio ending the call (its target is TSI): the caller of a group call, either party of a
		// private one. The TS clears the call next; leave now
		if (p->u.maint.kind == T3_MAINT_DISCON && !is_me(ms, p->u.maint.src) &&
		    (p->u.maint.src == ms->call.src || (!ms->call.group && p->u.maint.src == ms->call.dst))) {
			return_to_tscc(ms, now);
		}
		break;
	case T3_OP_P_PROTECT: {
		// 6.6.2.3.2.3: DIS_PTT and EN_PTT apply addressed to the radio, the talkgroup of its grant or ALLMSID; not
		// to the other party of an individual call
		const t3_protect_t *pr = &p->u.protect;
		bool ours = pr->dst == T3_ID_ALLMSID || is_me(ms, pr->dst) ||
			    (pr->group && ms->call.group && pr->dst == ms->call.dst);
		switch (pr->kind) {
		case T3_PROTECT_DIS_PTT:
			if (ours) {
				ms->protect_dis = true;
			}
			break;
		case T3_PROTECT_EN_PTT:
			if (ours) {
				ms->protect_dis = false;
			}
			break;
		case T3_PROTECT_EN_PTT_ONE_MS:
			// While these come, only the addressed radio may talk; the rest have PTT off until they stop
			if (is_me(ms, pr->dst)) {
				ms->protect_dis = false;
				ms->protect_other_ms = 0;
			} else {
				ms->protect_other_ms = now != 0 ? now : 1u;
			}
			break;
		case T3_PROTECT_ILLEGALLY_PARKED: {
			// Everyone not in the named call leaves
			bool mine = pr->group ? (ms->call.group && pr->dst == ms->call.dst)
					      : (is_me(ms, pr->dst) || is_me(ms, pr->src));
			if (!mine) {
				return_to_tscc(ms, now);
			}
			break;
		}
		default:
			break;
		}
		break;
	}
	case T3_OP_C_AHOY:
		on_ahoy(ms, now, slot, &p->u.ahoy, true);
		break;
	default:
		if (is_grant(p->op)) {
			if (cdef != NULL) {
				t3_chan_t c = { .lcn = cdef->chan, .cc = cdef->cc, .rx_hz = cdef->rx_hz, .tx_hz = cdef->tx_hz };
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
	bool waiting = ms->ax.state == T3_AX_WAIT_RESP;
	uint16_t left = ms->ax.frames;

	if (ms->state == T3_FOLLOW) {
		on_payload(ms, now, slot, p, cdef);
	} else if (ms->vote.on) {
		on_vote(ms, cc, p);
	} else {
		on_tscc(ms, now, slot, cc, p, cdef);
	}
	if (waiting && ms->ax.state != T3_AX_WAIT_RESP && ms->st.ra_tries != 0) {
		// This PDU answered the last random access burst: how many frames after it (the wait counts down from
		// ra_window)
		uint8_t k = (uint8_t)((ms->st.ra_tries - 1u) % T3_RA_LOG);
		uint16_t wait = ra_window(ms);
		ms->st.ra_log[k].frames = (uint8_t)(left <= wait ? (uint16_t)(wait - left) : 0u);
		ms->st.ra_log[k].rx_slot = slot;
		ms->st.ra_log[k].rx_ms = (uint16_t)(now - ms->st.ra_log[k].tx_ms);
		ms->st.ra_log[k].op = p->op;
	}
}

static bool cc_ok(const t3_ms_t *ms, uint8_t cc)
{
	uint8_t want = ms->state == T3_FOLLOW ? ms->call.chan.cc : ms->tuned.cc;
	if (ms->state == T3_CAMPED && !ms->vote.on) {
		want = ms->tscc.cc;
	}
	return want == T3_CC_ANY || want == cc;
}

// A 24-bit address, most significant octet first
static uint32_t addr24(const uint8_t *d)
{
	return ((uint32_t)d[0] << 16) | ((uint32_t)d[1] << 8) | d[2];
}

// A DGNA download to the radio (6.6.8): DGNA_Address mode replaces the list of up to 15 (ADRNULL: none; all ADRNULL
// deletes them); bit 0 of octet 0 (OK) makes ADDRESS1 the One-key talkgroup. DGNA_Alias mode names a talkgroup of
// the list, or sets the 16th (ADRNULL with no characters deletes it). Answered MS_Accepted when asked
static void dgna_done(t3_ms_t *ms, const t3_udt_t *h, uint8_t slot)
{
	const uint8_t *d = &ms->udt_rx.blk[0][0];
	uint8_t room = (uint8_t)(h->blocks * T3_UDT_BLOCK - 2u);
	bool ok = (d[0] & 1u) != 0;
	if (h->format == T3_UDTF_ADDRESS) {
		uint32_t old_onekey = ms->dgna_onekey;
		bool onekey_listed = false;
		ms->ndgna = 0;
		for (uint8_t o = 1; o + 3u <= room && ms->ndgna < T3_DGNA_MAX; o = (uint8_t)(o + 3u)) {
			uint32_t a = addr24(d + o);
			if (a != T3_ID_NULL) {
				ms->dgna[ms->ndgna++] = a;
				onekey_listed |= a == old_onekey;
			}
		}
		if (ok && addr24(d + 1) != T3_ID_NULL) {
			ms->dgna_onekey = addr24(d + 1);
		} else if (!onekey_listed && old_onekey != ms->dgna_alias_tg) {
			ms->dgna_onekey = 0;	// it was one of the old list
		}
	} else if (h->format == T3_UDTF_DGNA_ALIAS) {
		uint32_t a = addr24(d + 1);
		uint8_t pad = (uint8_t)(h->pad / 2u);
		uint8_t len = room > 4u + pad ? (uint8_t)((room - 4u - pad) / 2u) : 0u;
		if (len > T3_DGNA_ALIAS_MAX) {
			len = T3_DGNA_ALIAS_MAX;
		}
		if (a == T3_ID_NULL) {
			if (ms->dgna_onekey == ms->dgna_alias_tg) {
				ms->dgna_onekey = 0;
			}
			ms->dgna_alias_tg = 0;
			ms->dgna_named = 0;
			ms->dgna_alias_len = 0;
		} else {
			bool listed = false;
			for (uint8_t i = 0; i < ms->ndgna; i++) {
				listed |= ms->dgna[i] == a;
			}
			if (!listed) {
				ms->dgna_alias_tg = a;
			}
			ms->dgna_named = a;
			ms->dgna_alias_len = len;
			for (uint8_t i = 0; i < len; i++) {
				ms->dgna_alias[i] = (uint16_t)((d[4u + 2u * i] << 8) | d[5u + 2u * i]);
			}
			if (ok) {
				ms->dgna_onekey = a;
			}
		}
	} else {
		return;
	}
	ms->dgnas++;
	if (h->a && may_send(ms)) {
		answer(ms, slot, false, T3_RC_MS_ACCEPTED, h->src, false);
	}
}

// The answer to our call is that its destination's calls are diverted (6.6.7.2): a UDT download to us, supplementary,
// from MSI, GPI or DIVERTI, ADDRESS1 the new destination. The request is over; the radio shows it or asks again
static bool diverted(t3_ms_t *ms, uint32_t now, const t3_udt_t *h)
{
	if (h->op != T3_OP_C_UDTHD || !h->sf || h->group || !is_me(ms, h->dst) || ms->stunned ||
	    (h->src != T3_ID_MSI && h->src != T3_ID_GPI && h->src != T3_ID_DIVERTI) || h->format != T3_UDTF_ADDRESS ||
	    (ms->ax.state != T3_AX_WAIT_RESP && ms->ax.state != T3_AX_ACKED) || ms->ax.svc == T3_SVC_DIVERT) {
		return false;
	}
	if (!t3_udt_crc_ok((const uint8_t (*)[T3_UDT_BLOCK])ms->udt_rx.blk, h->blocks)) {
		ms->st.crc_bad++;
		return true;
	}
	const uint8_t *a = &ms->udt_rx.blk[0][1];
	ms->ax.divert_to = (uint32_t)a[0] << 16 | (uint32_t)a[1] << 8 | a[2];
	ms->ax.divert_group = h->src == T3_ID_GPI;
	finish(ms, now, T3_RES_DIVERTED);
	return true;
}

// Supplementary user data for the radio or a talkgroup it follows (6.4.13.2): a UDT download from SUPLI with SF 1,
// held until its sender is named. To the radio with A 1 it is accepted, C_ACKU Message_Accepted to SUPLI. Without
// CONFIG_DMR_TRUNK_SUPL it is taken off the air unanswered: it is no short data
static bool supl_heard(t3_ms_t *ms, uint32_t now, uint8_t slot, const t3_udt_t *h, bool to_me)
{
	if (h->op != T3_OP_C_UDTHD || !h->sf || h->src != T3_ID_SUPLI || ms->stunned ||
	    (!to_me && !(h->group && t3_ms_wants_group(ms, h->dst, false)))) {
		return false;
	}
#if !CONFIG_DMR_TRUNK_SUPL
	(void)now, (void)slot;
	return true;
#else
	if (!t3_udt_crc_ok((const uint8_t (*)[T3_UDT_BLOCK])ms->udt_rx.blk, h->blocks)) {
		ms->st.crc_bad++;
		return true;
	}
	uint8_t room = (uint8_t)(h->blocks * T3_UDT_BLOCK - 2u);
	uint8_t pad = (uint8_t)(h->pad / 2u);
	uint8_t len = pad < room ? (uint8_t)(room - pad) : 0u;
	bool repeat = ms->supl_rx.ms != 0 && !since(now, ms->supl_rx.ms, T3_REPEAT_MS) && ms->supl_rx.dst == h->dst &&
		      ms->supl_rx.group == h->group && ms->supl_rx.format == h->format && ms->supl_rx.len == len &&
		      memcmp(ms->supl_rx.data, &ms->udt_rx.blk[0][0], len) == 0;
	if (!repeat) {
		ms->supl_rx.count++;
		ms->supl_rx.src = 0;
	}
	ms->supl_rx.dst = h->dst;
	ms->supl_rx.group = h->group;
	ms->supl_rx.format = h->format;
	ms->supl_rx.len = len;
	memcpy(ms->supl_rx.data, &ms->udt_rx.blk[0][0], len);
	ms->supl_rx.ms = now != 0 ? now : 1;
	if (to_me && h->a && may_send(ms)) {
		answer(ms, slot, false, T3_RC_MESSAGE_ACCEPTED, T3_ID_SUPLI, false);
	}
	return true;
#endif
}

// A UDT short data message whole (6.6.4.7): kept when it is to the radio or a talkgroup it listens to; one to the
// radio that asks for a response is answered MS_Accepted. A bad CRC gets no answer: the TSCC sends it again
static void udt_done(t3_ms_t *ms, uint32_t now, uint8_t slot)
{
	const t3_udt_t *h = &ms->udt_rx.h;
	bool to_me = !h->group && is_me(ms, h->dst);
	if (h->op == T3_OP_C_DGNAHD) {
		if (!to_me || ms->stunned) {
			return;
		}
		if (!t3_udt_crc_ok((const uint8_t (*)[T3_UDT_BLOCK])ms->udt_rx.blk, h->blocks)) {
			ms->st.crc_bad++;
			return;
		}
		ps_wake(ms, now);
		dgna_done(ms, h, slot);
		return;
	}
	if (diverted(ms, now, h) || supl_heard(ms, now, slot, h, to_me) || cli_heard(ms, now, slot, h, to_me)) {
		return;
	}
	if (ms->stunned || h->op != T3_OP_C_UDTHD || h->sf ||
	    (!to_me && !(h->group && t3_ms_wants_group(ms, h->dst, false)))) {
		return;
	}
	if (!t3_udt_crc_ok((const uint8_t (*)[T3_UDT_BLOCK])ms->udt_rx.blk, h->blocks)) {
		ms->st.crc_bad++;
		return;
	}
	uint8_t room = (uint8_t)(h->blocks * T3_UDT_BLOCK - 2u);
	uint8_t pad = (uint8_t)(h->pad / 2u);
	uint8_t len = pad < room ? (uint8_t)(room - pad) : 0u;
	// A repeat (on_status) is answered again but not counted as another message
	bool repeat = ms->sd_rx.ms != 0 && !since(now, ms->sd_rx.ms, T3_REPEAT_MS) && ms->sd_rx.src == h->src &&
		      ms->sd_rx.dst == h->dst && ms->sd_rx.group == h->group && ms->sd_rx.format == h->format &&
		      ms->sd_rx.len == len && memcmp(ms->sd_rx.data, &ms->udt_rx.blk[0][0], len) == 0;
	if (!repeat) {
		ms->sd_rx.count++;
	}
	ms->sd_rx.src = h->src;
	ms->sd_rx.dst = h->dst;
	ms->sd_rx.group = h->group;
	ms->sd_rx.format = h->format;
	ms->sd_rx.len = len;
	memcpy(ms->sd_rx.data, &ms->udt_rx.blk[0][0], ms->sd_rx.len);
	ms->sd_rx.ms = now != 0 ? now : 1;
	supl_sender(ms, now, h->src, h->dst, h->group);
	// The data our poll asked for: the calling MS's final acknowledgement (6.6.5.5)
	bool polled = ms->ax.svc == T3_SVC_SDPOLL && to_me && h->src == ms->ax.dst &&
		      (ms->ax.state == T3_AX_WAIT_RESP || ms->ax.state == T3_AX_ACKED);
	if (to_me && (h->a || polled) && may_send(ms)) {
		answer(ms, slot, false, T3_RC_MS_ACCEPTED, h->src, false);
	}
	if (polled) {
		finish(ms, now, T3_RES_DONE);
	}
}

static void udt_rx(t3_ms_t *ms, uint32_t now, uint8_t slot, uint8_t dt, const uint8_t d[T3_CSBK_BYTES])
{
	if (dt == T3_DT_DATA_HDR) {
		ms->udt_rx.open = t3_udt_parse(d, &ms->udt_rx.h);
		if (!ms->udt_rx.open) {
			ms->st.crc_bad++;
			return;
		}
		ms->udt_rx.slot = slot;
		ms->udt_rx.got = 0;
		ms->udt_rx.ms = now;
		return;
	}
	// The blocks follow the header in consecutive frames of its slot
	if (!ms->udt_rx.open || slot != ms->udt_rx.slot || since(now, ms->udt_rx.ms, MBC_GAP_MS)) {
		ms->udt_rx.open = false;
		return;
	}
	memcpy(ms->udt_rx.blk[ms->udt_rx.got++], d, T3_UDT_BLOCK);
	ms->udt_rx.ms = now;
	if (ms->udt_rx.got == ms->udt_rx.h.blocks) {
		ms->udt_rx.open = false;
		udt_done(ms, now, slot);
	}
}

void t3_ms_rx(t3_ms_t *ms, uint32_t now, uint8_t slot, uint8_t dt, uint8_t cc, const uint8_t d[T3_CSBK_BYTES])
{
	// Killed (6.4.10): nothing heard, nothing answered
	if (ms->killed || ms->state == T3_OFF || slot < 1 || slot > 2 || !since(now, ms->tuned_ms, T3_SETTLE_MS)) {
		return;
	}
	if (dt != T3_DT_CSBK && dt != T3_DT_MBC_HDR && dt != T3_DT_MBC_CONT && dt != T3_DT_DATA_HDR && dt != T3_DT_R12) {
		return;
	}
	if (!cc_ok(ms, cc)) {
		return;
	}
	if (ms->state == T3_CAMPED && !ms->vote.on && dt == T3_DT_CSBK && (d[0] & 0x3Fu) == T3_OP_C_AHOY &&
	    ms->tscc_slots != 0 && !(ms->tscc_slots & (1u << (slot - 1u)))) {
		// On a slot no Aloha has shown to carry the TSCC: the carrier's payload channel, which the radio has left.
		// Its P_AHOY has C_AHOY's opcode and would be taken for the TSCC's (other PDUs have their own)
		return;
	}
	if (dt == T3_DT_DATA_HDR || dt == T3_DT_R12) {
		udt_rx(ms, now, slot, dt, d);
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
		// Any good CSBK on the call's slot keeps it alive, one with another FID included
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
	    !t3_sysparms_parse(slc, &sp)) {
		return;
	}
	if (!sp.control) {
		// A composite TSCC lent to a call (5.3.2): its carrier's CACH carries P_SYS_Parms with Payload Channel Type
		// 1 instead of C_SYS_Parms. The control channel comes back after the call with an Aloha: wait for it
		// rather than hunt (T_Nosig), and send no random access meanwhile. Only on our system's TSCC: another
		// payload channel's CACH is never heard while camped
		if (ms->state == T3_CAMPED && !ms->vote.on && ms->syscode != 0 &&
		    sp.model == (uint8_t)(ms->syscode >> 14) && sp.netsite == (uint16_t)((ms->syscode >> 2) & 0xFFFu)) {
			ms->slc_here = true;
		}
		if (sp.flag && ms->state == T3_CAMPED && !ms->vote.on && ms->syscode != 0 &&
		    sp.model == (uint8_t)(ms->syscode >> 14) && sp.netsite == (uint16_t)((ms->syscode >> 2) & 0xFFFu)) {
			ms->st.lent++;
			ms->lent_ms = now != 0 ? now : 1u;
			ms->last_valid_ms = now;
		}
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
	if (ms->state == T3_CAMPED && !ms->vote.on) {
		ms->slc_here = true;
		ms->lent_ms = 0;	// the control channel is back
		// The value is that of the four slots whose CACHs carried it: complete only as they end, so its 120 ms
		// began one step before now
		ms->csc = sp.csc;
		ms->csc_ms = now - T3_CSC_STEP_MS;
		ms->csc_valid = true;
	}
}

void t3_ms_slot(t3_ms_t *ms, uint32_t now, uint8_t slot, bool at_known, bool at_busy)
{
	if (!may_send(ms) || ms->state == T3_OFF || ms->state == T3_HUNT || slot < 1 || slot > 2 ||
	    !since(now, ms->tuned_ms, T3_SETTLE_MS)) {
		return;
	}
	if (ms->state == T3_CAMPED && ms->lent_ms != 0 && !since(now, ms->lent_ms, T3_LENT_MS)) {
		return;		// the TSCC is lent to a call: it can't take an access (5.3.2 b)
	}
	if (ms->tx.count != 0 && slot == ms->tx.slot) {
		// An answer, an upload or P_MAINT: no random access, the slot is the radio's
		if (ms->tx.udt) {
			send_udt(ms, slot);
			ms->tx.udt = false;
		} else {
			send(ms, ms->tx.burst, slot, ms->tx.payload);
		}
		if (--ms->tx.count == 0 && ms->tx.then_return && ms->state == T3_FOLLOW) {
			return_to_tscc(ms, now);
		}
		return;
	}
	if (ms->state == T3_CAMPED && ms->ax.state != T3_AX_IDLE && slot == ms->ax.slot) {
		ra_slot(ms, now, at_known, at_busy);
	}
}

uint8_t t3_ms_at_tc(const t3_ms_t *ms, uint8_t slot)
{
	return ms->aloha.offset ? slot : (uint8_t)(3u - slot);
}

// The answer to a random access burst comes in one of the NRand_Wait + 1 frames after the burst itself (6.2.1.1.5).
// A radio can hold a burst list back for a while (after a retune its stack waits for the channel's slot timing):
// counted from the hand-over, the wait would end before an answer in time. So the window starts again from the
// radio's report; a burst it dropped is tried again after the backoff, without waiting for an answer that can't come
void t3_ms_sent(t3_ms_t *ms, uint32_t now, bool ok)
{
	if (!ms->ax.to_air) {
		return;
	}
	ms->ax.to_air = false;
	if (ms->ax.state != T3_AX_WAIT_RESP || ms->st.ra_tries == 0) {
		return;	// answered already, or given up
	}
	uint8_t k = (uint8_t)((ms->st.ra_tries - 1u) % T3_RA_LOG);
	uint32_t d = now - ms->st.ra_log[k].tx_ms;
	ms->st.ra_log[k].air_ms = d == 0 ? 1u : (d > 0xFFFFu ? 0xFFFFu : (uint16_t)d);
	ms->ax.frames = ok ? ra_window(ms) : 1u;
}

void t3_ms_activity(t3_ms_t *ms, uint32_t now, uint8_t slot)
{
	if (ms->state == T3_FOLLOW && slot == ms->call.slot && since(now, ms->tuned_ms, T3_SETTLE_MS)) {
		ms->last_act_ms = now;
	}
}

// A modem that doesn't report the CACH's short LC never hears a lent composite TSCC's P_SYS_Parms (5.3.2). A TSCC's
// slot carries a call's bursts only while lent, so on a TSCC with no short LC heard since the radio came, a voice
// burst, header or terminator in our color code on its slot marks it lent the same way. Where short LCs are heard,
// they alone say so
void t3_ms_call_burst(t3_ms_t *ms, uint32_t now, uint8_t slot, uint8_t cc)
{
	if (ms->state != T3_CAMPED || ms->vote.on || !ms->confirmed || ms->slc_here || slot < 1 || slot > 2 ||
	    !(ms->tscc_slots & (1u << (slot - 1u))) || !cc_ok(ms, cc) || !since(now, ms->tuned_ms, T3_SETTLE_MS)) {
		return;
	}
	ms->st.lent_calls++;
	ms->lent_ms = now != 0 ? now : 1u;
	ms->last_valid_ms = now;
}

// ---- Timers, actions out ----

bool t3_ms_poll(t3_ms_t *ms, uint32_t now, t3_act_t *act)
{
	switch (ms->state) {
	case T3_OFF:
		break;
	case T3_HUNT:
		if (ms->nq == 0 && (int32_t)(now - ms->hunt_deadline) >= 0) {
			hunt_next(ms, now);
		}
		break;
	case T3_CAMPED: {
		if (ms->vote.on) {
			if (ms->nq == 0 && !before(now, ms->vote.until)) {
				vote_end(ms, now, true);
			}
			break;
		}
		// Asleep, nothing can be heard: T_Nosig counts only the time awake. The receiver goes off a little after the
		// engine sleeps, so a PDU can still come in between two polls of a sleep: never move the last one past now,
		// or since() reads it as long ago
		bool sleep = ps_may_sleep(ms, now);
		if (ms->asleep) {
			uint32_t slept = now - ms->ps_tick_ms;
			ms->last_valid_ms = since(now, ms->last_valid_ms, slept) ? ms->last_valid_ms + slept : now;
		}
		if (sleep && !ms->asleep) {
			ms->sleeps++;
		} else if (!sleep && ms->asleep) {
			ms->woke_ms = now;
		}
		ms->asleep = sleep;
		ms->ps_tick_ms = now;
		if (since(now, ms->last_valid_ms, T3_NOSIG_MS)) {
			enter_hunt(ms, now, true);
			break;
		}
		if (!(ms->cfg->opts & T3_OPT_NO_ROAM) && weak(ms, ms->level_dbm) && ms->ax.state == T3_AX_IDLE &&
		    hunt_count(ms) > 1) {
			// Below the threshold (6.3.3.1 a) for T3_WEAK_MS, with somewhere else to look: hunt, and take only a
			// stronger TSCC for one pass
			if (ms->weak_since == 0) {
				ms->weak_since = now != 0 ? now : 1u;
			} else if (since(now, ms->weak_since, T3_WEAK_MS)) {
				ms->st.weak_leaves++;
				ms->hunt_idx = 0;	// from the adjacent sites
				enter_hunt(ms, now, false);
				ms->hunt_strict = true;
				break;
			}
		} else {
			ms->weak_since = 0;
		}
		if (!may_send(ms)) {
			break;
		}
		if (ms->ax.state == T3_AX_ACKED && (int32_t)(now - ms->ax.ack_deadline) >= 0) {
			finish(ms, now, T3_RES_TIMEOUT);	// TP_Timer / TNP_Timer
		} else if ((ms->ax.state == T3_AX_WAIT_SLOT || ms->ax.state == T3_AX_WAIT_RESP) &&
			   (int32_t)(now - ms->ax.tc_deadline) >= 0) {
			finish(ms, now, T3_RES_TIMEOUT);	// TRand_TC
		}
		if (ms->ax.state == T3_AX_IDLE && ms->massreg && (int32_t)(now - ms->massreg_at) >= 0 && ms->aloha_here &&
		    !ms->deregistered && (ms->reg == T3_REG_OK || ms->reg == T3_REG_NEEDED || ms->reg == T3_REG_NOT_NEEDED)) {
			// Mass registration's time: register again by random access (6.4.5.1 d)
			ms->massreg = false;
			start_access(ms, now, T3_SVC_REGISTER, 0, false);
		}
		if (ms->ax.state == T3_AX_IDLE && ms->reg == T3_REG_NEEDED && !ms->deregistered && ms->aloha_here &&
		    (ms->ax.retry_ms == 0 || (int32_t)(now - ms->ax.retry_ms) >= 0)) {
			// Until registered the radio asks for nothing else (6.4.3)
			start_access(ms, now, T3_SVC_REGISTER, 0, false);
		}
		break;
	}
	case T3_FOLLOW:
		if (ms->protect_other_ms != 0 && since(now, ms->protect_other_ms, T3_PROTECT_ONE_MS)) {
			ms->protect_other_ms = 0;
		}
		if (since(now, ms->last_act_ms, T3_INACTIVE_MS) ||
		    since(now, ms->call.start_ms, T3_CALL_MAX_MS)) {
			return_to_tscc(ms, now);
		} else if (ms->data_end_ms != 0 && (int32_t)(now - ms->data_end_ms) >= 0 && ms->tx.count == 0) {
			ms->data_end_ms = 0;
			t3_ms_end_call(ms, now);
		}
		break;
	}
	if (ms->nq == 0) {
		return false;
	}
	*act = ms->q[0];
	ms->nq--;
	memmove(&ms->q[0], &ms->q[1], ms->nq * sizeof(ms->q[0]));
	return true;
}

// ---- Access requests from the radio ----

static bool may_ask(t3_ms_t *ms)
{
	if (!may_send(ms) || ms->stunned || ms->killed || ms->state != T3_CAMPED || !ms->confirmed ||
	    (ms->reg != T3_REG_OK && ms->reg != T3_REG_NOT_NEEDED)) {
		ms->ax.result = T3_RES_NOT_ALLOWED;
		return false;
	}
	return true;
}

void t3_ms_set_stunned(t3_ms_t *ms, bool stunned)
{
	ms->stunned = stunned;
}

void t3_ms_set_killed(t3_ms_t *ms, bool killed)
{
	ms->killed = killed;
}

bool t3_ms_request(t3_ms_t *ms, uint32_t now, uint32_t dst, bool group, bool emergency)
{
	bool bad = dst == T3_ID_NULL || dial_bad(ms, dst, group);
	if (bad || ms->ax.state != T3_AX_IDLE || !may_ask(ms)) {
		if (bad) {
			ms->ax.result = T3_RES_NOT_ALLOWED;
		}
		return false;
	}
	ms->talk_id = dst;
	start_access(ms, now, group ? T3_SVC_GROUP_VOICE : T3_SVC_IND_VOICE, dst, emergency);
	return true;
}

bool t3_ms_status(t3_ms_t *ms, uint32_t now, uint32_t dst, bool group, uint8_t status)
{
	// A poll goes to an MS only (6.6.6.2.2)
	bool bad = dst == T3_ID_NULL || status > T3_STATUS_POLL || (group && status == T3_STATUS_POLL);
	if (bad || ms->ax.state != T3_AX_IDLE || !may_ask(ms)) {
		if (bad) {
			ms->ax.result = T3_RES_NOT_ALLOWED;
		}
		return false;
	}
	start_access(ms, now, T3_SVC_STATUS, dst, false);
	ms->ax.status = status;
	ms->ax.group = group;
	ms->ax.info = 0;
	if (status != T3_STATUS_POLL) {
		ms->own_status = status;
	}
	return true;
}

bool t3_ms_sdata(t3_ms_t *ms, uint32_t now, uint32_t dst, bool group, uint8_t format, const uint8_t *data, uint8_t len)
{
	bool iso7 = format == T3_UDTF_ISO7;
	bool bad = dst == T3_ID_NULL || len == 0 || len > (iso7 ? T3_UDT_ISO7_MAX : T3_UDT_MAX) || format > 0x0Fu ||
		   dial_bad(ms, dst, group);
	if (bad || ms->ax.state != T3_AX_IDLE || !may_ask(ms)) {
		if (bad) {
			ms->ax.result = T3_RES_NOT_ALLOWED;
		}
		return false;
	}
	start_access(ms, now, T3_SVC_SDATA, dst, false);
	ms->ax.group = group;
	ms->ax.info = 0;
	ms->sd_tx.format = format;
	if (iso7) {
		// One character an octet in, 7 bits each on air, first character first (B.3.4); the bits left over in the
		// last octet are 1s, and table B.4 counts them as pad
		memset(ms->sd_tx.data, 0xFF, sizeof(ms->sd_tx.data));
		for (uint16_t i = 0; i < 7u * len; i++) {
			if (!((data[i / 7u] >> (6u - i % 7u)) & 1u)) {
				ms->sd_tx.data[i / 8u] &= (uint8_t)~(0x80u >> (i % 8u));
			}
		}
		ms->sd_tx.len = (uint8_t)((7u * len + 7u) / 8u);
		ms->sd_tx.spare = (uint8_t)(8u * ms->sd_tx.len - 7u * len);
	} else {
		ms->sd_tx.len = len;
		memcpy(ms->sd_tx.data, data, len);
		// A last BCD digit 1111b is DigitNULL (table 7.46): a pad nibble
		ms->sd_tx.spare = format == T3_UDTF_BCD && (data[len - 1u] & 0x0Fu) == 0x0Fu ? 4u : 0u;
	}
	return true;
}

bool t3_ms_pdata(t3_ms_t *ms, uint32_t now, uint32_t dst, bool group)
{
	bool bad = dst == T3_ID_NULL || dial_bad(ms, dst, group);
	if (bad || ms->ax.state != T3_AX_IDLE || !may_ask(ms)) {
		if (bad) {
			ms->ax.result = T3_RES_NOT_ALLOWED;
		}
		return false;
	}
	start_access(ms, now, T3_SVC_PDATA, dst, false);
	ms->ax.group = group;
	return true;
}

bool t3_ms_pdata_ip(t3_ms_t *ms, uint32_t now, uint32_t ipv4)
{
	if (ipv4 == 0 || !t3_ms_pdata(ms, now, T3_ID_IPI, false)) {
		if (ipv4 == 0) {
			ms->ax.result = T3_RES_NOT_ALLOWED;
		}
		return false;
	}
	ms->ax.ip = ipv4;
	return true;
}

bool t3_ms_sdata_poll(t3_ms_t *ms, uint32_t now, uint32_t dst, uint8_t format, uint8_t blocks)
{
	bool bad = dst == T3_ID_NULL || dst > T3_ID_USER_MAX || format > 0x0Fu || blocks < 1u || blocks > 4u;
	if (bad || ms->ax.state != T3_AX_IDLE || !may_ask(ms)) {
		if (bad) {
			ms->ax.result = T3_RES_NOT_ALLOWED;
		}
		return false;
	}
	start_access(ms, now, T3_SVC_SDPOLL, dst, false);
	ms->ax.status = format;			// POL_FMT
	ms->ax.info = (uint8_t)(blocks - 1u);	// Appended_Short_Data
	ms->ax.group = false;
	return true;
}

bool t3_ms_request_als(t3_ms_t *ms, uint32_t now, uint32_t dst, bool emergency)
{
	if (dst == T3_ID_NULL || dst > T3_ID_USER_MAX || ms->ax.state != T3_AX_IDLE || !may_ask(ms)) {
		if (dst == T3_ID_NULL || dst > T3_ID_USER_MAX) {
			ms->ax.result = T3_RES_NOT_ALLOWED;
		}
		return false;
	}
	start_access(ms, now, T3_SVC_IND_VOICE, dst, emergency);
	ms->ax.als = true;
	return true;
}

bool t3_ms_als_tx(const t3_ms_t *ms, uint32_t now)
{
	return ms->call.als && before(now, ms->call.als_end) && t3_ms_can_talk(ms);
}

bool t3_ms_als_over(const t3_ms_t *ms, uint32_t now)
{
	return ms->state == T3_FOLLOW && ms->call.als && !before(now, ms->call.als_end);
}

bool t3_ms_interrupt(t3_ms_t *ms, uint32_t now, bool end_als)
{
	const t3_call_t *c = &ms->call;
	// A voice call followed and not keyed by the radio itself; status 123 only from the MS listening to an ALS call
	bool ok = may_send(ms) && ms->state == T3_FOLLOW && !c->data && !c->als && ms->tx.count == 0 &&
		  ms->ax.state == T3_AX_IDLE && !ms->stunned && !ms->killed && (!end_als || c->als_ours) &&
		  (ms->reg == T3_REG_OK || ms->reg == T3_REG_NOT_NEEDED);
	if (!ok) {
		ms->ax.result = T3_RES_NOT_ALLOWED;
		return false;
	}
	// Addressed to the talker: the talkgroup, or the other MS of an individual call (6.4.14.3.3)
	uint32_t dst = c->group || !is_me(ms, c->dst) ? c->dst : c->src;
	bool group = c->group;
	return_to_tscc(ms, now);	// other calls' grants don't take the radio while it asks (on_grant)
	start_access(ms, now, T3_SVC_STATUS, dst, false);
	ms->ax.status = end_als ? T3_STATUS_ALS_END : T3_STATUS_INTERRUPT;
	ms->ax.group = group;
	ms->ax.info = 0;
	ms->ax.resume = !end_als;
	return true;
}

bool t3_ms_divert(t3_ms_t *ms, uint32_t now, uint8_t services, uint32_t to, bool group)
{
	bool bad = to == T3_ID_NULL || to > T3_ID_USER_MAX || (services & T3_DIV_ALL) == 0;
	if (bad || ms->ax.state != T3_AX_IDLE || !may_ask(ms)) {
		if (bad) {
			ms->ax.result = T3_RES_NOT_ALLOWED;
		}
		return false;
	}
	start_access(ms, now, T3_SVC_DIVERT, group ? T3_ID_GPI : T3_ID_MSI, false);
	ms->ax.div = (uint8_t)(T3_SO_DIV_SET | (services & T3_DIV_ALL));
	ms->ax.div_to = to;
	ms->ax.group = group;
	return true;
}

bool t3_ms_divert_clear(t3_ms_t *ms, uint32_t now, uint8_t services, bool incoming)
{
	if ((services & T3_DIV_ALL) == 0 || ms->ax.state != T3_AX_IDLE || !may_ask(ms)) {
		if ((services & T3_DIV_ALL) == 0) {
			ms->ax.result = T3_RES_NOT_ALLOWED;
		}
		return false;
	}
	start_access(ms, now, T3_SVC_DIVERT, incoming ? T3_ID_DIVERTI : T3_ID_TSI, false);
	ms->ax.div = services & T3_DIV_ALL;
	ms->ax.div_to = 0;
	ms->ax.group = false;
	return true;
}

bool t3_ms_data_ready(const t3_ms_t *ms, uint32_t now)
{
	return may_send(ms) && ms->state == T3_FOLLOW && ms->call.data && ms->tx.count == 0 &&
	       since(now, ms->tuned_ms, T3_SETTLE_MS);
}

void t3_ms_cancel(t3_ms_t *ms, uint32_t now)
{
	if (!is_voice(ms->ax.svc) || ms->ax.state == T3_AX_IDLE) {
		return;
	}
	if (ms->ax.state == T3_AX_WAIT_SLOT && ms->ax.tries == 0) {
		// Nothing sent yet: just drop it (6.6.1.2.1)
		finish(ms, now, T3_RES_CANCELED);
		return;
	}
	// The TSCC has it: ask it to cancel; it answers with C_ACKD (Message_Accepted)
	start_access(ms, now, T3_SVC_CANCEL, ms->ax.dst, ms->ax.emergency);
}

void t3_ms_data_done(t3_ms_t *ms, uint32_t now, bool ok)
{
	if (ms->state != T3_FOLLOW || !ms->call.data || !ms->call.ours) {
		return;
	}
	if (!ok) {
		ms->data_end_ms = 0;
		t3_ms_end_call(ms, now);	// 6.6.3.3.2.5 e: retries spent without an ACK
		return;
	}
	ms->data_end_ms = (now + T3_DATA_LINGER_MS) | 1u;	// 6.6.3.3.2.5 a, c
}

void t3_ms_data_heard(t3_ms_t *ms, uint32_t now)
{
	if (ms->data_end_ms != 0) {
		ms->data_end_ms = (now + T3_DATA_LINGER_MS) | 1u;
	}
}

void t3_ms_end_call(t3_ms_t *ms, uint32_t now)
{
	if (ms->state != T3_FOLLOW) {
		return;
	}
	if (may_send(ms) && (ms->call.ours || !ms->call.group)) {
		// 6.6.2.3.2.5: N_Maint P_MAINT (DISCON) in a row, then back to the TSCC
		t3_csbk_t p;
		memset(&p, 0, sizeof(p));
		p.lb = true;
		p.op = T3_OP_P_MAINT;
		p.u.maint.kind = T3_MAINT_DISCON;
		p.u.maint.dst = T3_ID_TSI;
		p.u.maint.src = my_id(ms);
		t3_csbk_build(&p, T3_CRC_MASK_CSBK, ms->tx.burst);
		ms->tx.count = T3_N_MAINT;
		ms->tx.slot = ms->call.slot;
		ms->tx.payload = true;
		ms->tx.then_return = true;
		ms->tx.udt = false;
		return;
	}
	// A talkgroup call heard: leave quietly, and don't come back on its late-entry grants
	if (ms->call.group) {
		ms->left_dst = ms->call.dst;
	}
	return_to_tscc(ms, now);
}

void t3_ms_groups_changed(t3_ms_t *ms, uint32_t now)
{
	if (!(ms->cfg->opts & T3_OPT_REG_TG) || ms->deregistered) {
		return;
	}
	if (ms->ax.state != T3_AX_IDLE && ms->ax.svc == T3_SVC_REGISTER) {
		ms->reg_stale = true;
	} else if (ms->reg == T3_REG_OK) {
		ms->reg = T3_REG_NEEDED;
	}
	// The knob turned through several channels registers once, with the last
	ms->ax.retry_ms = (now + T3_REG_TG_DELAY_MS) | 1u;
}

bool t3_ms_deregister(t3_ms_t *ms, uint32_t now)
{
	if (!may_send(ms) || ms->state != T3_CAMPED || ms->reg != T3_REG_OK) {
		return false;
	}
	if (ms->ax.state != T3_AX_IDLE) {
		finish(ms, now, T3_RES_CANCELED);
	}
	start_access(ms, now, T3_SVC_DEREGISTER, 0, false);
	return true;
}

bool t3_ms_can_talk(const t3_ms_t *ms)
{
	// The PTT stays off on a broadcast or all-call the radio didn't make (6.6.2.3.2.4, 6.6.2.5) and while P_PROTECT
	// disables it
	return may_send(ms) && ms->state == T3_FOLLOW && !ms->protect_dis && ms->protect_other_ms == 0 &&
	       ms->tx.count == 0 && !ms->call.data &&
	       (ms->call.ours || (!is_allcall(ms->call.dst) && ms->call.op != T3_OP_BTV_GRANT));
}
