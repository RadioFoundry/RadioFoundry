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
	ms->tuned_slot = slot;
	ms->tuned_ms = now;
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
	return i < c->nhunt && chan_lookup(ms, c->hunt[i], out);
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
	ms->asleep = false;
	ms->massreg = false;	// another TSCC cancels it (6.4.5.1)
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
	case T3_OP_PV_GRANT_DX:
		// Either party: a call to the radio, or one it made
		*group = false;
		return is_me(ms, g->src) || ((ms->cfg->opts & T3_OPT_PRIVATE) && is_me(ms, g->dst));
	case T3_OP_PD_GRANT:
		// 6.6.3.2.7 a: packet data to the radio, or that it sends
		*group = false;
		return is_me(ms, g->src) || is_me(ms, g->dst);
	case T3_OP_TD_GRANT:
		// 6.6.3.2.7 b: a talkgroup the radio listens to, or its own data to one
		*group = true;
		return is_me(ms, g->src) || t3_ms_wants_group(ms, g->dst, g->emergency);
	default:
		// Multi-item and duplex data grants aren't asked for; a grant to a gateway or another MS is not ours
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
	ms->call.ours = is_me(ms, g->src);
	ms->call.data = p->op == T3_OP_PD_GRANT || p->op == T3_OP_TD_GRANT;
	ms->protect_dis = false;
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
	} else if (just_left(ms, now, &p->u.grant, &c)) {
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
	       !(ms->massreg && (int32_t)(now - ms->massreg_at) >= 0) && (int32_t)(now - ms->awake_until) >= 0 && !ps_frame(ms, now);
}

bool t3_ms_asleep(const t3_ms_t *ms)
{
	return ms->asleep;
}

static void return_to_tscc(t3_ms_t *ms, uint32_t now)
{
	ms->cleared = ms->call;
	ms->cleared_ms = now != 0 ? now : 1;
	ms->protect_dis = false;
	ms->state = T3_CAMPED;
	ms->last_valid_ms = now;
	ms->st.returns++;
	ps_wake(ms, now);	// T_Awake after the call (6.4.7.2.1)
	tune(ms, now, &ms->tscc, 0, false);
}

// ---- The control channel ----

// The registration area of a system code: MODEL, NET and the DMRLA most significant bits of SITE (6.4.4). A new
// area needs a new registration
static uint16_t reg_area(const t3_ms_t *ms, uint16_t code)
{
	t3_syscode_t s;
	t3_syscode_split(code, &s);
	uint8_t sb = t3_site_bits(s.model);
	uint8_t la = ms->cfg->dmrla < sb ? ms->cfg->dmrla : sb;
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
	} else if (ms->cfg->opts & T3_OPT_LISTEN_ONLY) {
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
	case T3_RES_CANCELLED: return "cancelled";
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

static void finish(t3_ms_t *ms, uint32_t now, t3_res_t r)
{
	t3_svc_t svc = ms->ax.svc;
	ms->ax.state = T3_AX_IDLE;
	ms->ax.result = r;
	if (svc == T3_SVC_REGISTER && r != T3_RES_DONE && ms->reg != T3_REG_DENIED) {
		// No record was made: try again later (6.4.4.1: no answer leaves the record as it was)
		ms->ax.retry_ms = (now + T3_REG_RETRY_MS) | 1u;
	}
}

static void start_access(t3_ms_t *ms, uint32_t now, t3_svc_t svc, uint32_t dst, bool emergency)
{
	ps_wake(ms, now);
	ms->ax.state = T3_AX_WAIT_SLOT;
	ms->ax.svc = svc;
	ms->ax.dst = dst;
	ms->ax.emergency = emergency;
	// The first slot the TSCC uses; a dedicated TSCC on both slots is reached on TS1
	ms->ax.slot = (ms->tscc_slots & 1u) || ms->tscc_slots == 0 ? 1 : 2;
	ms->ax.frames = 0;
	ms->ax.tries = 0;
	ms->ax.tc_deadline = now + or_default(ms->cfg->trand_tc_ms, T3_TRAND_TC_MS);
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
		return a->service_fn == 0 || a->service_fn == 1;
	default:
		return a->service_fn == 0 || a->service_fn == 2;
	}
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
		ms->ps_rq_sent = 0;
		if ((ms->cfg->opts & T3_OPT_REG_TG) && ms->ax.svc == T3_SVC_REGISTER && n >= 2u) {
			// A list (6.4.4.1.13.2): to TATTSI, with the blocks the TSCC's C_AHOY will ask for
			q->extra = (uint8_t)((T3_TRG_TATTSI << 2) | (reg_list_blocks(n) - 1u));
			q->dst = T3_ID_TATTSI;
			ms->reg_tgs = n;
		} else if ((ms->cfg->opts & T3_OPT_REG_TG) && tg != NULL) {
			q->extra = T3_TRG_TALKGROUP << 2;
			q->dst = tg->id;
		} else {
			q->extra = T3_TRG_SYSCODE << 2;
			q->dst = ms->syscode;
		}
		if (ms->ax.svc == T3_SVC_REGISTER && ms->reg_tgs == 0 && ms->cfg->ps_rq != 0) {
			// Power save (6.4.7.2.1). Not with a list: its answer's Response_Info is the list's
			ms->ps_rq_sent = ms->cfg->ps_rq & 7u;
			q->options |= (uint8_t)(ms->ps_rq_sent << 1);
		}
		break;
	}
	case T3_SVC_GROUP_VOICE:
		q->kind = T3_SK_TG_VOICE;
		q->options = (uint8_t)((ms->ax.emergency ? T3_SO_EMERGENCY : 0) |
				       (is_broadcast(ms, ms->ax.dst) ? T3_SO_BROADCAST : 0));
		break;
	case T3_SVC_IND_VOICE:
		q->kind = T3_SK_IND_VOICE;
		q->options = ms->ax.emergency ? T3_SO_EMERGENCY : 0;
		break;
	case T3_SVC_PDATA:
		// Table 6.50: single slot, single item, normal priority, no supplementary data
		q->kind = ms->ax.group ? T3_SK_TG_DATA : T3_SK_IND_DATA;
		q->options = ms->ax.emergency ? T3_SO_EMERGENCY : 0;
		break;
	case T3_SVC_SDATA:
		// Table 6.55: no options; the 4 service-dependent bits are SUPED_VAL 0 and the blocks to upload less one
		q->kind = ms->ax.group ? T3_SK_TG_UDT : T3_SK_IND_UDT;
		q->options = 0;
		q->extra = (uint8_t)(((ms->sd_tx.len + 2u + T3_UDT_BLOCK - 1u) / T3_UDT_BLOCK - 1u) & 3u);
		break;
	case T3_SVC_DIVERT:
		// Table 6.61: no emergency or privacy; DIVONOFF and the services; SDATA_VAL 0 (the address fits one
		// appended block)
		q->kind = T3_SK_DIVERT;
		q->options = ms->ax.div & 0x1Fu;
		q->extra = 0;
		break;
	case T3_SVC_STATUS:
		// Tables 6.57, 6.58: G/I, SUPED_SV 0 and the status's 5 most significant bits; its 2 least significant
		// where a registration has Appended_UDT_Short_Data, SUPED_VAL 0 above them
		q->kind = T3_SK_STATUS;
		q->options = (uint8_t)((ms->ax.group ? 0x40u : 0u) | ((ms->ax.status >> 2) & 0x1Fu));
		q->extra = (uint8_t)(ms->ax.status & 3u);
		break;
	default:
		q->kind = T3_SK_CANCEL;
		break;
	}
	t3_csbk_build(&p, T3_CRC_MASK_CSBK, out);
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
		ms->ax.state = T3_AX_WAIT_RESP;
		ms->ax.frames = (uint16_t)(t3_nwait_frames(ms->aloha.nrand_wait) + 1u);
		if (ms->ax.svc == T3_SVC_REGISTER) {
			// 6.4.4.1: the old record goes as soon as the request is sent
			ms->reg = T3_REG_NEEDED;
			ms->reg_area = 0;
			ms->reg_tg_ok = 0;
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
		ms->ax.ack_deadline = now + (is_call(svc) ? or_default(ms->cfg->tp_ms, T3_TP_MS) :
							    or_default(ms->cfg->tnp_ms, T3_TNP_MS));
		break;
	case T3_ACK:
		if (svc == T3_SVC_REGISTER) {
			ms->reg = T3_REG_OK;
			ms->reg_area = reg_area(ms, ms->syscode);
			// Reg_Accepted or Reg_Subscription/Attachment: Response_Info says which of a list were taken. Only
			// the first reg_tgs bits mean anything: all taken is 1111111b however many went (6.4.4.1.13.2)
			uint8_t sent_bits = (uint8_t)(0x7Fu & ~((1u << (T3_REG_TG_MAX - ms->reg_tgs)) - 1u));
			ms->reg_tg_ok = ms->reg_tgs != 0 ? (uint8_t)(a->info & sent_bits) : 0u;
			// PowerSave_Offset: the wake frame; 0 = registered, no power save (table 6.13)
			ms->ps_rq = ms->ps_rq_sent;
			ms->ps_offset = ms->ps_rq != 0 ? (uint8_t)(a->info & ((1u << ms->ps_rq) - 1u)) : 0u;
			ms->awake_until = now + or_default(ms->cfg->awake_ms, T3_AWAKE_MS);
		} else if (svc == T3_SVC_DEREGISTER) {
			ms->reg = T3_REG_NEEDED;
			ms->deregistered = true;
		} else if (is_voice(svc) && a->reason == T3_RC_MS_ALERTING) {
			// The called radio rings: the call goes on
			ms->ax.state = T3_AX_ACKED;
			ms->ax.ack_deadline = now + or_default(ms->cfg->tp_ms, T3_TP_MS);
			break;
		}
		finish(ms, now, T3_RES_DONE);
		break;
	case T3_NACK:
		if (svc == T3_SVC_REGISTER && a->reason == T3_RC_REG_DENIED) {
			// Barred from this area until power off; find another TSCC (6.4.4.1)
			uint16_t area = reg_area(ms, ms->syscode);
			uint8_t i = 0;
			while (i < T3_DENIED_MAX - 1 && ms->denied[i] != 0 && ms->denied[i] != (uint16_t)(area + 1u)) {
				i++;
			}
			ms->denied[i] = (uint16_t)(area + 1u);
			ms->reg = T3_REG_DENIED;
			finish(ms, now, T3_RES_REFUSED);
			enter_hunt(ms, now, false);
		} else if (svc == T3_SVC_REGISTER && a->reason == T3_RC_REG_REFUSED) {
			// Look for another TSCC; this one is tried again when the hunt comes back to it
			finish(ms, now, T3_RES_REFUSED);
			enter_hunt(ms, now, false);
		} else {
			finish(ms, now, T3_RES_REFUSED);
		}
		break;
	default:
		break;
	}
}

// C_AHOY on the TSCC or P_AHOY on the payload channel addressed to the radio: radio check, availability check (OACSU),
// call cancel, selective clear. Authentication isn't supported: it is refused. The answer goes on the inbound slot
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
		      uint8_t fill, bool count_pad)
{
	uint8_t pad = 0;
	u->op = T3_OP_C_UDTHU;
	u->src = my_id(ms);
	u->blocks = t3_udt_pack(data, len, fill, ms->up, &pad);
	u->pad = count_pad ? pad : 0u;
	t3_udt_build(u, ms->tx.burst);
	ms->tx.count = 1;
	ms->tx.slot = slot;
	ms->tx.payload = false;
	ms->tx.then_return = false;
	ms->tx.udt = true;
	ms->tx.nblocks = u->blocks;
	ms->ax.state = T3_AX_ACKED;
	ms->ax.ack_deadline = now + or_default(ms->cfg->tnp_ms, T3_TNP_MS);
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
		return true;	// nothing to upload (6.6.4.4 d, supplementary data, isn't sent)
	}
	t3_udt_t u;
	memset(&u, 0, sizeof(u));
	u.group = ms->ax.group;
	u.format = ms->sd_tx.format;
	u.dst = ms->ax.dst;
	// ISO 8-bit text pads with 1111b nibbles (B.3.5); every other format here is binary-like, padded with zeros
	uint8_t fill = u.format == T3_UDTF_ISO8 ? 0xFFu : 0u;
	queue_udt(ms, now, slot, &u, ms->sd_tx.data, ms->sd_tx.len, fill,
		  u.format == T3_UDTF_ISO8 || u.format == T3_UDTF_ISO7 || u.format == T3_UDTF_UTF16);
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
	queue_udt(ms, now, slot, &u, d, sizeof(d), 0, false);
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
	queue_udt(ms, now, slot, &u, d, (uint8_t)(1u + 3u * slots), 0, false);
	return true;
}

static void on_ahoy(t3_ms_t *ms, uint32_t now, uint8_t slot, const t3_ahoy_t *h, bool payload)
{
	if (!payload && (on_upload(ms, now, slot, h) || on_reg_list(ms, now, slot, h) ||
			  on_divert_ask(ms, now, slot, h))) {
		return;
	}
	if (!payload && h->kind == T3_SK_STATUS && is_me(ms, h->src)) {
		// Our status going to the called party or talkgroup (6.6.6.1.2.1.2 c): it is under way, the final answer
		// follows. Not a status heard, though the talkgroup's may be ours
		if (ms->ax.svc == T3_SVC_STATUS && (ms->ax.state == T3_AX_WAIT_RESP || ms->ax.state == T3_AX_ACKED)) {
			ms->ax.state = T3_AX_ACKED;
			ms->ax.ack_deadline = now + or_default(ms->cfg->tnp_ms, T3_TNP_MS);
		}
		return;
	}
	if (!payload && on_status(ms, now, slot, h)) {
		return;
	}
	bool to_me = !h->group && is_me(ms, h->dst);
	bool to_call = payload && h->group && ms->call.group && h->dst == ms->call.dst;
	if (!to_me && !to_call) {
		return;
	}
	ps_wake(ms, now);
	if (h->kind == T3_SK_CANCEL && payload) {
		// Selective clear (6.6.2.3.1.8): answer, then leave the channel
		if (may_send(ms)) {
			answer(ms, slot, true, T3_RC_MESSAGE_ACCEPTED, h->src, true);
		} else {
			return_to_tscc(ms, now);
		}
		return;
	}
	if (!may_send(ms)) {
		return;
	}
	if (to_me && !payload && h->kind == T3_SK_SUPPLEMENTARY && h->src == T3_ID_STUNI) {
		// Stun (Service_Kind_Flag 0) or revive (1), without authentication (6.4.9.1.2)
		ms->stunned = !h->kind_flag;
		if (ms->stunned) {
			ms->stuns++;
		} else {
			ms->revives++;
		}
		answer(ms, slot, false, T3_RC_MESSAGE_ACCEPTED, h->src, false);
	} else if (to_me && !payload && h->kind == T3_SK_REGISTRATION && h->src <= T3_AUTH_CHALLENGE_MAX &&
		   ms->cfg->auth_key_set) {
		// An authentication challenge (6.4.8.3): the source is the challenge, the answer's target the response. One
		// during an access (registration, a call) is progress: the final answer follows within TNP_Timer
		answer(ms, slot, false, T3_RC_MS_AUTH_RESPONSE, t3_auth_response(ms->cfg->auth_key, h->src, ms->cfg->psn),
		       false);
		ms->auths++;
		if (ms->ax.state == T3_AX_WAIT_RESP || ms->ax.state == T3_AX_ACKED) {
			ms->ax.state = T3_AX_ACKED;
			ms->ax.ack_deadline = now + or_default(ms->cfg->tnp_ms, T3_TNP_MS);
		}
	} else if (h->kind == T3_SK_REGISTRATION && h->src == T3_ID_TSI) {
		answer(ms, slot, payload, T3_RC_MS_ACCEPTED, h->src, false);	// radio check
	} else if (ms->stunned) {
		return;		// stunned: no user services to answer for
	} else if (to_me && !payload && h->kind == T3_SK_CANCEL) {
		answer(ms, slot, false, T3_RC_MS_ACCEPTED, h->src, false);	// the caller gave up (6.6.1.2.2)
	} else if (to_me && !payload && h->kind == T3_SK_IND_VOICE) {
		answer(ms, slot, false, T3_RC_MS_ACCEPTED, h->src, false);	// availability check: OACSU
	} else if (to_me && !payload && h->kind == T3_SK_IND_DATA) {
		// Packet data to the radio (6.6.3.2.6): the data service takes it on the payload channel
		answer(ms, slot, false, T3_RC_MESSAGE_ACCEPTED, h->src, false);
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
	ms->aloha_here = true;
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
		c = (t3_chan_t){ cdef->chan, cdef->rx_hz, cdef->tx_hz, cdef->cc };
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
			bool to = false;
			if (p->u.clear.lpcn == T3_LPCN_ABSOLUTE && cdef != NULL) {
				// An MBC: the appended block gives the TSCC's frequencies
				c = (t3_chan_t){ cdef->chan, cdef->rx_hz, cdef->tx_hz, cdef->cc };
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
		// 6.6.2.3.2.3: addressed to the radio, the call's talkgroup or every radio
		const t3_protect_t *pr = &p->u.protect;
		bool ours = pr->dst == T3_ID_ALLMSID || is_me(ms, pr->dst) || for_call(ms, pr->group, pr->dst);
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
			// Only the addressed radio may talk
			ms->protect_dis = !is_me(ms, pr->dst);
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

// A UDT short data message whole (6.6.4.7): kept when it is to the radio or a talkgroup it listens to; one to the
// radio that asks for a response is answered MS_Accepted. A bad CRC gets no answer: the TSCC sends it again
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
	if (diverted(ms, now, h)) {
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
	if (to_me && h->a && may_send(ms)) {
		answer(ms, slot, false, T3_RC_MS_ACCEPTED, h->src, false);
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
	if (ms->state == T3_OFF || slot < 1 || slot > 2 || !since(now, ms->tuned_ms, T3_SETTLE_MS)) {
		return;
	}
	if (dt != T3_DT_CSBK && dt != T3_DT_MBC_HDR && dt != T3_DT_MBC_CONT && dt != T3_DT_DATA_HDR && dt != T3_DT_R12) {
		return;
	}
	if (!cc_ok(ms, cc)) {
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
		ms->csc_ms = now;
		ms->csc_valid = true;
	}
}

void t3_ms_slot(t3_ms_t *ms, uint32_t now, uint8_t slot, bool at_known, bool at_busy)
{
	if (!may_send(ms) || ms->state == T3_OFF || ms->state == T3_HUNT || slot < 1 || slot > 2 ||
	    !since(now, ms->tuned_ms, T3_SETTLE_MS)) {
		return;
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
		if (ms->nq == 0 && (int32_t)(now - ms->hunt_deadline) >= 0) {
			hunt_next(ms, now);
		}
		break;
	case T3_CAMPED: {
		// Asleep, nothing can be heard: T_Nosig counts only the time awake
		bool sleep = ps_may_sleep(ms, now);
		if (ms->asleep) {
			ms->last_valid_ms += now - ms->ps_tick_ms;
		}
		if (sleep && !ms->asleep) {
			ms->sleeps++;
		}
		ms->asleep = sleep;
		ms->ps_tick_ms = now;
		if (since(now, ms->last_valid_ms, or_default(ms->cfg->nosig_ms, T3_NOSIG_MS))) {
			enter_hunt(ms, now, true);
			break;
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
		if (since(now, ms->last_act_ms, or_default(ms->cfg->inactive_ms, T3_INACTIVE_MS)) ||
		    since(now, ms->call.start_ms, T3_CALL_MAX_MS)) {
			return_to_tscc(ms, now);
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
	if (!may_send(ms) || ms->stunned || ms->state != T3_CAMPED || !ms->confirmed ||
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

bool t3_ms_request(t3_ms_t *ms, uint32_t now, uint32_t dst, bool group, bool emergency)
{
	if (dst == T3_ID_NULL || ms->ax.state != T3_AX_IDLE || !may_ask(ms)) {
		if (dst == T3_ID_NULL) {
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
	bool bad = dst == T3_ID_NULL || len == 0 || len > T3_UDT_MAX || format > 0x0Fu;
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
	ms->sd_tx.len = len;
	memcpy(ms->sd_tx.data, data, len);
	return true;
}

bool t3_ms_pdata(t3_ms_t *ms, uint32_t now, uint32_t dst, bool group)
{
	if (dst == T3_ID_NULL || ms->ax.state != T3_AX_IDLE || !may_ask(ms)) {
		if (dst == T3_ID_NULL) {
			ms->ax.result = T3_RES_NOT_ALLOWED;
		}
		return false;
	}
	start_access(ms, now, T3_SVC_PDATA, dst, false);
	ms->ax.group = group;
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
		finish(ms, now, T3_RES_CANCELLED);
		return;
	}
	// The TSCC has it: ask it to cancel; it answers with C_ACKD (Message_Accepted)
	start_access(ms, now, T3_SVC_CANCEL, ms->ax.dst, ms->ax.emergency);
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

bool t3_ms_deregister(t3_ms_t *ms, uint32_t now)
{
	if (!may_send(ms) || ms->state != T3_CAMPED || ms->reg != T3_REG_OK) {
		return false;
	}
	if (ms->ax.state != T3_AX_IDLE) {
		finish(ms, now, T3_RES_CANCELLED);
	}
	start_access(ms, now, T3_SVC_DEREGISTER, 0, false);
	return true;
}

bool t3_ms_can_talk(const t3_ms_t *ms)
{
	// The PTT stays off on a broadcast or all-call the radio didn't make (6.6.2.3.2.4, 6.6.2.5) and while P_PROTECT
	// disables it
	return may_send(ms) && ms->state == T3_FOLLOW && !ms->protect_dis && ms->tx.count == 0 && !ms->call.data &&
	       (ms->call.ours || (!is_allcall(ms->call.dst) && ms->call.op != T3_OP_BTV_GRANT));
}
