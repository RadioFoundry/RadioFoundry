// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// DL_CMD_T3 and the t3 view section (docs/spec/debuglink.md 3.26, docs/spec/dmr-trunking.md 11): a Tier III trunked
// system staged in RAM, selected and left, and the engine's state as the radio core last published it.
#include <string.h>
#include "config.h"
#include "debuglink/debuglink.h"
#include "debuglink/debuglink_proto.h"
#include "radio/radio.h"
#include "radio/radio_trunk.h"
#include "hal_time.h"

#if CONFIG_DMR_TRUNK
static uint16_t rd16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)rd16(p) | ((uint32_t)rd16(p + 2) << 16);
}

// lcn u16, rx Hz u32, tx Hz u32, cc u8 (0xFF: the control channel's)
static bool chan_of(const uint8_t *a, uint16_t len, t3_chan_t *c)
{
	if (len < 11u) {
		return false;
	}
	c->lcn = rd16(a);
	c->rx_hz = rd32(a + 2);
	c->tx_hz = rd32(a + 6);
	c->cc = a[10];
	return c->lcn != 0 && c->lcn < 0xFFFu && c->rx_hz != 0 && c->tx_hz != 0 && (c->cc <= 15u || c->cc == T3_CC_ANY);
}

static bool add_chan(radio_t3_setup_t *s, const t3_chan_t *c)
{
	for (uint8_t i = 0; i < s->nchans; i++) {
		if (s->chans[i].lcn == c->lcn) {
			s->chans[i] = *c;
			return true;
		}
	}
	if (s->nchans >= RADIO_T3_CHANS) {
		return false;
	}
	s->chans[s->nchans++] = *c;
	return true;
}
#endif

size_t debuglink_cmd_t3(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap)
{
	(void)cap;
	out[0] = DL_ST_OK;
#if CONFIG_DMR_TRUNK
	if (len < 1u) {
		out[0] = DL_ST_BAD_ARG;
		return 1u;
	}
	radio_t3_setup_t *s = radio_t3_staged();
	const uint8_t *a = req + 1;
	uint16_t n = (uint16_t)(len - 1u);
	t3_chan_t c;
	switch (req[0]) {
	case DL_T3_RESET:
		radio_t3_reset(s);
		break;
	case DL_T3_SYSTEM:
		// own ID u32 (0: the radio's), model u8, net u16, category u8 (1 A, 2 B), opts u16, cc u8, talk ID u32 (0:
		// the first talkgroup with the talk flag), talk private u8
		if (n < 16u || a[4] > 3u || (a[7] != T3_PAR_A && a[7] != T3_PAR_B) || a[10] > 15u) {
			out[0] = DL_ST_BAD_ARG;
			break;
		}
		s->cfg.own_id = rd32(a);
		s->cfg.auth.model = a[4];
		s->cfg.auth.net = rd16(a + 5);
		s->cfg.auth.category = a[7];
		s->cfg.opts = rd16(a + 8);
		s->cc = a[10];
		s->talk_id = rd32(a + 11) & 0xFFFFFFu;
		s->talk_private = a[15] != 0;
		// Optional, for roaming: DMRLA u8, the leave threshold dBm s16 (0: the radio's roaming threshold), the Vote
		// Now sample ms u16 (0: default); then flags u8 (bit 0: IP through the system's gateway)
		s->cfg.dmrla = n >= 17u ? a[16] : 0u;
		s->cfg.leave_dbm = n >= 19u ? (int16_t)rd16(a + 17) : 0;
		s->cfg.vote_ms = n >= 21u ? rd16(a + 19) : 0u;
		s->ip_gw = n >= 22u && (a[21] & 1u) != 0;
		break;
	case DL_T3_CTRL:	// a control channel candidate (also a channel of the plan)
		if (!chan_of(a, n, &c) || s->cfg.nctrl >= T3_CTRL_MAX || !add_chan(s, &c)) {
			out[0] = DL_ST_BAD_ARG;
			break;
		}
		s->cfg.ctrl[s->cfg.nctrl++] = c;
		break;
	case DL_T3_CHAN:	// a channel of the plan's table
		if (!chan_of(a, n, &c) || !add_chan(s, &c)) {
			out[0] = DL_ST_BAD_ARG;
		}
		break;
	case DL_T3_TG:		// id u32, flags u8 (T3_TG_LISTEN, T3_TG_TALK, T3_TG_BROADCAST)
		if (n < 5u || s->cfg.ntg >= T3_TG_MAX || rd32(a) == 0 || rd32(a) > 0xFFFFFFu) {
			out[0] = DL_ST_BAD_ARG;
			break;
		}
		s->cfg.tg[s->cfg.ntg].id = rd32(a);
		s->cfg.tg[s->cfg.ntg].flags = a[4];
		s->cfg.ntg++;
		break;
	case DL_T3_SITE:	// a SITE the radio may use (none: any)
		if (n < 2u || s->cfg.auth.nsites >= T3_SITES_MAX) {
			out[0] = DL_ST_BAD_ARG;
			break;
		}
		s->cfg.sites[s->cfg.auth.nsites++] = rd16(a);
		break;
	case DL_T3_PLAN:	// the fixed plan: base Hz u32, first u16, sep code u8, tx low u8, split Hz u32
		if (n < 12u || a[6] > 7u) {
			out[0] = DL_ST_BAD_ARG;
			break;
		}
		s->cfg.plan.fixed.base_hz = rd32(a);
		s->cfg.plan.fixed.first = rd16(a + 4);
		s->cfg.plan.fixed.sep = a[6];
		s->cfg.plan.fixed.tx_low = a[7] != 0;
		s->cfg.plan.fixed.split_hz = rd32(a + 8);
		break;
	case DL_T3_START:
		radio_req_t3(RADIO_T3_START);
		break;
	case DL_T3_STOP:
		radio_req_t3(RADIO_T3_STOP);
		break;
	case DL_T3_DEREG:
		radio_req_t3(RADIO_T3_DEREG);
		break;
	case DL_T3_END:
		radio_req_t3(RADIO_T3_END);
		break;
	case DL_T3_STATUS:	// to u32, group u8, status u8
		if (n < 6u || rd32(a) == 0 || rd32(a) > 0xFFFFFFu || a[5] > T3_STATUS_POLL) {
			out[0] = DL_ST_BAD_ARG;
			break;
		}
		s->sd_dst = rd32(a);
		s->sd_group = a[4] != 0;
		s->sd_status = a[5];
		radio_req_t3(RADIO_T3_STATUS);
		break;
	case DL_T3_SDATA:	// to u32, group u8, format u8, data
		if (n < 7u || n > 6u + T3_UDT_MAX || rd32(a) == 0 || rd32(a) > 0xFFFFFFu || a[5] > 15u) {
			out[0] = DL_ST_BAD_ARG;
			break;
		}
		s->sd_dst = rd32(a);
		s->sd_group = a[4] != 0;
		s->sd_format = a[5];
		s->sd_len = (uint8_t)(n - 6u);
		memcpy(s->sd_data, a + 6, s->sd_len);
		radio_req_t3(RADIO_T3_SDATA);
		break;
	case DL_T3_ALS:		// to u32, emergency u8, T_ALS s u16, T_ALS_E s u16
		if (n != 9u || rd32(a) > T3_ID_USER_MAX) {
			out[0] = DL_ST_BAD_ARG;
			break;
		}
		s->cfg.als_s = rd16(a + 5);
		s->cfg.als_e_s = rd16(a + 7);
		if (rd32(a) != 0) {
			s->sd_dst = rd32(a);
			s->sd_group = a[4] != 0;
			radio_req_t3(RADIO_T3_ALS);
		}
		break;
	case DL_T3_SDPOLL:	// to u32, format u8, blocks u8
		if (n != 6u || rd32(a) == 0 || rd32(a) > T3_ID_USER_MAX || a[4] > 15u || a[5] < 1u || a[5] > 4u) {
			out[0] = DL_ST_BAD_ARG;
			break;
		}
		s->sd_dst = rd32(a);
		s->sd_group = false;
		s->sd_format = a[4];
		s->sd_len = a[5];
		radio_req_t3(RADIO_T3_SDPOLL);
		break;
	case DL_T3_POLL_REPLY:	// format u8, data
		if (n < 1u || n > 1u + T3_UDT_MAX || a[0] > 15u) {
			out[0] = DL_ST_BAD_ARG;
			break;
		}
		s->cfg.poll_format = a[0];
		s->cfg.poll_len = (uint8_t)(n - 1u);
		memcpy(s->poll_reply, a + 1, s->cfg.poll_len);
		break;
#if CONFIG_DMR_TRUNK_GW
	case DL_T3_DIAL: {	// digits
		char d[T3_DIAL_MAX + 1];
		if (n > T3_DIAL_MAX) {
			out[0] = DL_ST_BAD_ARG;
			break;
		}
		memcpy(d, a, n);
		d[n] = '\0';
		uint8_t k = n != 0 ? t3_dial_pack(d, s->dial) : 0u;
		if (n != 0 && k == 0) {
			out[0] = DL_ST_BAD_ARG;
			break;
		}
		s->cfg.dial_n = k;
		break;
	}
#endif
#if CONFIG_DMR_TRUNK_SUPL
	case DL_T3_SUPL:	// format u8, data
		if (n < 1u || n > 1u + T3_UDT_MAX || a[0] > 15u) {
			out[0] = DL_ST_BAD_ARG;
			break;
		}
		s->cfg.supl_format = a[0];
		s->cfg.supl_len = (uint8_t)(n - 1u);
		memcpy(s->supl, a + 1, s->cfg.supl_len);
		break;
#endif
	case DL_T3_LOAD:	// system n u8
		if (n != 1u || !radio_t3_load(a[0], s)) {
			out[0] = DL_ST_BAD_ARG;
		}
		break;
	case DL_T3_INTERRUPT:	// end ALS u8
		if (n != 1u || a[0] > 1u) {
			out[0] = DL_ST_BAD_ARG;
			break;
		}
		radio_req_t3(a[0] != 0 ? RADIO_T3_ALS_END : RADIO_T3_INTERRUPT);
		break;
	case DL_T3_DIVERT:	// mode u8, services u8, to u32, group u8
		if (n != 7u || a[0] > RADIO_T3_DIV_CLEAR_IN || (a[1] & T3_DIV_ALL) == 0 || a[1] > T3_DIV_ALL ||
		    (a[0] == RADIO_T3_DIV_SET && (rd32(a + 2) == 0 || rd32(a + 2) > T3_ID_USER_MAX))) {
			out[0] = DL_ST_BAD_ARG;
			break;
		}
		s->div_mode = a[0];
		s->div_svcs = a[1];
		s->sd_dst = rd32(a + 2);
		s->sd_group = a[6] != 0;
		radio_req_t3(RADIO_T3_DIVERT);
		break;
	case DL_T3_PS:		// PowerSave_RQ u8, T_Awake ms u16
		if (n != 3u || a[0] > 7u) {
			out[0] = DL_ST_BAD_ARG;
			break;
		}
		s->cfg.ps_rq = a[0];
		s->cfg.awake_ms = rd16(a + 1);
		break;
	case DL_T3_AUTH:	// K (16 octets), PSN u32; nothing: no key. Write only: the view says whether one is set
		if (n == 0u) {
			s->cfg.auth_key_set = false;
			memset(s->cfg.auth_key, 0, sizeof(s->cfg.auth_key));
			s->cfg.psn = 0;
			break;
		}
		if (n != T3_AUTH_KEY_LEN && n != T3_AUTH_KEY_LEN + 4u) {
			out[0] = DL_ST_BAD_ARG;
			break;
		}
		memcpy(s->cfg.auth_key, a, T3_AUTH_KEY_LEN);
		s->cfg.psn = n > T3_AUTH_KEY_LEN ? rd32(a + T3_AUTH_KEY_LEN) & 0xFFFFFFu : 0u;
		s->cfg.auth_key_set = true;
		break;
	default:
		out[0] = DL_ST_BAD_ARG;
		break;
	}
	radio_t3_fix(s);
	return 1u;
#else
	(void)req;
	(void)len;
	out[0] = DL_ST_UNAVAILABLE;
	return 1u;
#endif
}

#if CONFIG_DMR_TRUNK
static const char *const t3_states[] = { "off", "hunt", "camped", "follow" };
static const char *const t3_regs[] = { "unknown", "not_needed", "listen", "needed", "ok", "denied" };
static const char *const t3_svcs[] = { "none", "register", "deregister", "group", "individual", "cancel",
				       "status", "sdata", "pdata", "divert", "sdpoll" };
static const char *const t3_results[] = { "none", "pending", "granted", "done", "refused", "no_answer", "timeout",
					  "not_allowed", "lost", "canceled", "diverted" };
static const char *const t3_axs[] = { "idle", "wait_slot", "wait_resp", "acked" };

static const char *word(uint8_t v, const char *const *names, uint8_t count)
{
	return v < count ? names[v] : "?";
}
#endif

#if CONFIG_DMR_TRUNK
static void t3_main(const radio_t3_view_t *x, void *ctx)
{
	dl_text_t *t = ctx;
	dl_text(t, "t3.built=1\nt3.tx_built=%u\nt3.on=%u\nt3.state=%s\nt3.reg=%s\nt3.confirmed=%u\nt3.syscode=0x%04x\n",
		CONFIG_DMR_TRUNK_TX ? 1u : 0u, x->on, word(x->state, t3_states, 4), word(x->reg, t3_regs, 6), x->confirmed,
		x->syscode);
	dl_text(t, "t3.own_id=%u\nt3.tscc_lcn=%u\nt3.rx_hz=%u\nt3.tx_hz=%u\nt3.slot=%u\nt3.cc=%u\n", (unsigned)x->own_id,
		x->tscc_lcn, (unsigned)x->rx_hz, (unsigned)x->tx_hz, x->slot, x->cc);
	dl_text(t, "t3.aloha_mask=%u\nt3.aloha_sf=%u\nt3.aloha_nrand=%u\nt3.aloha_backoff=%u\nt3.aloha_reg=%u\n",
		x->aloha_mask, x->aloha_sf, x->aloha_nrand, x->aloha_backoff, x->aloha_reg);
	dl_text(t, "t3.call_dst=%u\nt3.call_src=%u\nt3.call_group=%u\nt3.call_ours=%u\n", (unsigned)x->call_dst,
		(unsigned)x->call_src, x->call_group, x->call_ours);
	dl_text(t, "t3.ax_svc=%s\nt3.ax_state=%s\nt3.ax_result=%s\nt3.ax_reason=0x%02x\nt3.ptt_wait=%u\nt3.can_talk=%u\n",
		word(x->ax_svc, t3_svcs, sizeof(t3_svcs) / sizeof(t3_svcs[0])), word(x->ax_state, t3_axs, 4),
		word(x->ax_result, t3_results, sizeof(t3_results) / sizeof(t3_results[0])),
		x->ax_reason, x->ptt_wait, x->can_talk);
	// The last CSBK heard and sent: opcode, slot, age in ms (-1: none)
	uint32_t now = hal_millis();
	dl_text(t, "t3.last_rx_op=0x%02x\nt3.last_rx_slot=%u\nt3.last_rx_age_ms=%d\nt3.last_tx_op=0x%02x\n"
		"t3.last_tx_age_ms=%d\n", x->last_rx_op, x->last_rx_slot,
		x->last_rx_ms != 0 ? (int)(now - x->last_rx_ms) : -1, x->last_tx_op,
		x->last_tx_ms != 0 ? (int)(now - x->last_tx_ms) : -1);
	char hex[2 * T3_CSBK_BYTES + 1];
	for (unsigned i = 0; i < T3_CSBK_BYTES; i++) {
		static const char digits[] = "0123456789abcdef";
		hex[2 * i] = digits[x->last_tx[i] >> 4];
		hex[2 * i + 1] = digits[x->last_tx[i] & 15u];
	}
	hex[2 * T3_CSBK_BYTES] = '\0';
	dl_text(t, "t3.last_tx=%s\n", hex);
	dl_text(t, "t3.ticks=%u\nt3.csbk=%u\nt3.crc_bad=%u\nt3.grants=%u\nt3.followed=%u\nt3.unmapped=%u\n"
		"t3.last_unmapped=%u\n", (unsigned)x->ticks, (unsigned)x->csbk, (unsigned)x->crc_bad,
		(unsigned)x->grants, (unsigned)x->followed, (unsigned)x->unmapped, x->last_unmapped);
	dl_text(t, "t3.returns=%u\nt3.hunts=%u\nt3.foreign=%u\nt3.learned=%u\nt3.sent=%u\nt3.ra_tries=%u\n"
		"t3.send_fail=%u\nt3.at_known=%u\nt3.at_busy=%u\n", (unsigned)x->returns, (unsigned)x->hunts,
		(unsigned)x->foreign, (unsigned)x->learned, (unsigned)x->sent, (unsigned)x->ra_tries,
		(unsigned)x->send_fail, (unsigned)x->at_known, (unsigned)x->at_busy);
	dl_text(t, "t3.lent=%u\nt3.lent_calls=%u\n", (unsigned)x->lent, (unsigned)x->lent_calls);
}
#endif

void debuglink_view_t3(const radio_view_t *v, dl_text_t *t)
{
	(void)v;
#if CONFIG_DMR_TRUNK
	radio_with_t3_view(t3_main, t);	// formatted under the view's lock: no copy of the view here
#else
	dl_text(t, "t3.built=0\n");
#endif
}

// Roaming: the level, the adjacent sites, Vote Now and the weak-TSCC leave (dmr-trunking.md 7.18)
#if CONFIG_DMR_TRUNK
static void t3_roam(const radio_t3_view_t *x, void *ctx)
{
	dl_text_t *t = ctx;
	uint32_t now = hal_millis();
	dl_text(t, "t3.level_dbm=%d\nt3.leave_dbm=%d\nt3.dmrla=%u\nt3.reg_area=0x%04x\nt3.vote_on=%u\nt3.hunt_strict=%u\n",
		x->level_dbm, x->leave_dbm, x->dmrla, x->reg_area, x->vote_on, x->hunt_strict);
	dl_text(t, "t3.votes=%u\nt3.vote_moves=%u\nt3.weak_leaves=%u\nt3.adj_refused=%u\nt3.adjacent=%u\n",
		(unsigned)x->votes, (unsigned)x->vote_moves, (unsigned)x->weak_leaves, (unsigned)x->adj_refused, x->nadj);
	// The codeplug system the channel belongs to (n+1, 0: none) and its fallback (12.5): 1 on the channel, 2 looking
	dl_text(t, "t3.sys=%u\nt3.unavail=%u\nt3.fallback=%u\nt3.fallbacks=%u\nt3.backs=%u\n", x->sys, x->unavail,
		x->fallback, (unsigned)x->fallbacks, (unsigned)x->backs);
	// Each: RX Hz, the 14 system code bits, confirmed and adjacent priority, the level and age of its last sample
	for (uint8_t i = 0; i < x->nadj && i < sizeof x->adj / sizeof x->adj[0]; i++) {
		dl_text(t, "t3.adj%u=%u,0x%04x,%u,%u,%d,%d\n", i, (unsigned)x->adj[i].rx_hz, x->adj[i].sys14,
			x->adj[i].prio >> 4, x->adj[i].prio & 15u, x->adj[i].dbm,
			x->adj[i].sampled_ms != 0 ? (int)(now - x->adj[i].sampled_ms) : -1);
	}
}
#endif

void debuglink_view_t3_roam(const radio_view_t *v, dl_text_t *t)
{
	(void)v;
#if CONFIG_DMR_TRUNK
	radio_with_t3_view(t3_roam, t);	// formatted under the view's lock: no copy of the view here
#else
	dl_text(t, "t3.built=0\n");
#endif
}

// Its own section: with these keys the t3 section outgrew a reply (truncated=1 before t3.sd_*)
#if CONFIG_DMR_TRUNK
// len octets (at most 46) as lowercase hex
static void hex(char *out, const uint8_t *d, uint8_t len)
{
	static const char digits[] = "0123456789abcdef";
	if (len > 46u) {
		len = 46u;
	}
	for (unsigned i = 0; i < len; i++) {
		out[2 * i] = digits[d[i] >> 4];
		out[2 * i + 1] = digits[d[i] & 15u];
	}
	out[2 * len] = '\0';
}

static void t3_svc(const radio_t3_view_t *x, void *ctx)
{
	dl_text_t *t = ctx;
	uint32_t now = hal_millis();
	// Status (6.6.6): a poll's answer, the radio's own
	dl_text(t, "t3.ax_info=%u\nt3.own_status=%u\n", x->ax_info, x->own_status);
	// The registration's talkgroup list (6.4.4.1.13.2): how many went, which were taken (bit 6 the first)
	dl_text(t, "t3.reg_tgs=%u\nt3.reg_tg_ok=0x%02x\n", x->reg_tgs, x->reg_tg_ok);
	dl_text(t, "t3.call_data=%u\nt3.pd_wait=%u\nt3.stunned=%u\nt3.stuns=%u\nt3.revives=%u\n", x->call_data, x->pd_wait,
		x->stunned, (unsigned)x->stuns, (unsigned)x->revives);
	dl_text(t, "t3.auth_key=%u\nt3.auths=%u\nt3.massregs=%u\nt3.moves=%u\n", x->auth_key, (unsigned)x->auths,
		(unsigned)x->massregs, (unsigned)x->moves);
	dl_text(t, "t3.ps_rq=%u\nt3.ps_offset=%u\nt3.asleep=%u\nt3.sleeps=%u\n", x->ps_rq, x->ps_offset, x->asleep,
		(unsigned)x->sleeps);
	dl_text(t, "t3.csc_valid=%u\nt3.csc=%u\nt3.slc_ok=%u\nt3.slc_bad=%u\nt3.rx_off=%u\nt3.rx_offs=%u\nt3.rx_off_ms=%u\n",
		x->csc_valid, x->csc, (unsigned)x->slc_ok, (unsigned)x->slc_bad, x->rx_off, (unsigned)x->rx_offs,
		(unsigned)x->rx_off_ms);
	dl_text(t, "t3.ahoys=%u\nt3.ahoy_kind=%u\nt3.ahoy_src=%u\nt3.ahoy_age_ms=%d\n", (unsigned)x->ahoys, x->ahoy_kind,
		(unsigned)x->ahoy_src, x->ahoy_ms != 0 ? (int)(now - x->ahoy_ms) : -1);
	dl_text(t, "t3.divert_to=%u\nt3.divert_group=%u\nt3.als_calls=%u\nt3.call_als=%u\n", (unsigned)x->divert_to,
		x->divert_group, (unsigned)x->als_calls, x->call_als);
	dl_text(t, "t3.killed=%u\nt3.kills=%u\nt3.tscc_auth_fails=%u\n", x->killed, (unsigned)x->kills,
		(unsigned)x->tscc_auth_fails);
	dl_text(t, "t3.dgnas=%u\nt3.dgna_onekey=%u\nt3.dgna_alias_tg=%u\nt3.dgna=", (unsigned)x->dgnas,
		(unsigned)x->dgna_onekey, (unsigned)x->dgna_alias_tg);
	for (uint8_t i = 0; i < x->ndgna && i < RADIO_T3_DGNA; i++) {
		dl_text(t, i ? ",%u" : "%u", (unsigned)x->dgna[i]);
	}
	dl_text(t, "\n");
	dl_text(t, "t3.polls=%u\nt3.poll_fails=%u\nt3.ip_gw=%u\nt3.ip_advised=%u\n", x->polls, x->poll_fails, x->ip_gw,
		x->ip_advised);
}

// Random access timing, a section of its own: in t3 and t3_svc it outgrew a reply
static void t3_ra(const radio_t3_view_t *x, void *ctx)
{
	dl_text_t *t = ctx;
	uint32_t now = hal_millis();
	// The last random access bursts, oldest first: age ms, TX slot, answer after ms (0: none), frames after (255:
	// none), RX slot, the answer's opcode, on air after ms (0: not reported)
	dl_text(t, "t3.nra=%u\n", x->nra);
	for (uint8_t i = 0; i < x->nra && i < sizeof x->ra / sizeof x->ra[0]; i++) {
		dl_text(t, "t3.ra%u=%u,%u,%u,%u,%u,0x%02x,%u\n", i, (unsigned)(now - x->ra[i].tx_ms), x->ra[i].tx_slot,
			x->ra[i].rx_ms, x->ra[i].frames, x->ra[i].rx_slot, x->ra[i].op, x->ra[i].air_ms);
	}
	// The TX to RX turnaround
	dl_text(t, "t3.turns=%u\nt3.turn_ms=%u\nt3.turn_max_ms=%u\nt3.turn_slow=%u\n", (unsigned)x->turns, x->turn_ms,
		x->turn_max_ms, (unsigned)x->turn_slow);
}

// The messages heard, a section of their own: in t3_svc they outgrew a reply (truncated=1 before t3.supl_*)
static void t3_msg(const radio_t3_view_t *x, void *ctx)
{
	dl_text_t *t = ctx;
	uint32_t now = hal_millis();
	// The last status heard (6.6.6; age -1: none)
	dl_text(t, "t3.st_count=%u\nt3.st_src=%u\nt3.st_dst=%u\nt3.st_group=%u\nt3.st_value=%u\nt3.st_age_ms=%d\n",
		(unsigned)x->st_count, (unsigned)x->st_src, (unsigned)x->st_dst, x->st_group, x->st_value,
		x->st_ms != 0 ? (int)(now - x->st_ms) : -1);
	// The last short data heard: its octets in hex (sd_data), the format to read them by
	char sd[2 * sizeof(x->sd_data) + 1];
	hex(sd, x->sd_data, x->sd_len);
	dl_text(t, "t3.sd_count=%u\nt3.sd_src=%u\nt3.sd_dst=%u\nt3.sd_group=%u\nt3.sd_format=%u\nt3.sd_len=%u\n"
		"t3.sd_data=%s\nt3.sd_age_ms=%d\n", (unsigned)x->sd_count, (unsigned)x->sd_src, (unsigned)x->sd_dst,
		x->sd_group, x->sd_format, x->sd_len, sd, x->sd_ms != 0 ? (int)(now - x->sd_ms) : -1);
#if CONFIG_DMR_TRUNK_GW
	// The last caller's number from a gateway
	char cli[T3_DIAL_MAX + 1];
	t3_bcd_text(x->cli_bcd, x->cli_n, cli);
	dl_text(t, "t3.cli_count=%u\nt3.cli_gw=%u\nt3.cli_dst=%u\nt3.cli_group=%u\nt3.cli=%s\nt3.cli_age_ms=%d\n",
		(unsigned)x->cli_count, (unsigned)x->cli_gw, (unsigned)x->cli_dst, x->cli_group, cli,
		x->cli_ms != 0 ? (int)(now - x->cli_ms) : -1);
#endif
#if CONFIG_DMR_TRUNK_SUPL
	// The last supplementary data heard; supl_src 0 until a call names its sender
	hex(sd, x->supl_data, x->supl_len);
	dl_text(t, "t3.supl_count=%u\nt3.supl_src=%u\nt3.supl_dst=%u\nt3.supl_group=%u\nt3.supl_format=%u\n"
		"t3.supl_len=%u\nt3.supl_data=%s\nt3.supl_age_ms=%d\n", (unsigned)x->supl_count, (unsigned)x->supl_src,
		(unsigned)x->supl_dst, x->supl_group, x->supl_format, x->supl_len, sd,
		x->supl_ms != 0 ? (int)(now - x->supl_ms) : -1);
#endif
}
#endif

void debuglink_view_t3_svc(const radio_view_t *v, dl_text_t *t)
{
	(void)v;
#if CONFIG_DMR_TRUNK
	radio_with_t3_view(t3_svc, t);	// formatted under the view's lock: no copy of the view here
#else
	dl_text(t, "t3.built=0\n");
#endif
}

void debuglink_view_t3_msg(const radio_view_t *v, dl_text_t *t)
{
	(void)v;
#if CONFIG_DMR_TRUNK
	radio_with_t3_view(t3_msg, t);	// formatted under the view's lock: no copy of the view here
#else
	dl_text(t, "t3.built=0\n");
#endif
}

void debuglink_view_t3_ra(const radio_view_t *v, dl_text_t *t)
{
	(void)v;
#if CONFIG_DMR_TRUNK
	radio_with_t3_view(t3_ra, t);	// formatted under the view's lock: no copy of the view here
#else
	dl_text(t, "t3.built=0\n");
#endif
}
