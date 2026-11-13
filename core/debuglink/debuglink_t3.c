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
	case DL_T3_TG:		// id u32, flags u8 (T3_TG_LISTEN, T3_TG_TALK)
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
				       "status", "sdata" };
static const char *const t3_results[] = { "none", "pending", "granted", "done", "refused", "no_answer", "timeout",
					  "not_allowed", "lost", "cancelled" };
static const char *const t3_axs[] = { "idle", "wait_slot", "wait_resp", "acked" };

static const char *word(uint8_t v, const char *const *names, uint8_t count)
{
	return v < count ? names[v] : "?";
}
#endif

void debuglink_view_t3(const radio_view_t *v, dl_text_t *t)
{
#if CONFIG_DMR_TRUNK
	const radio_t3_view_t *x = &v->t3;
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
		word(x->ax_svc, t3_svcs, 8), word(x->ax_state, t3_axs, 4), word(x->ax_result, t3_results, 10),
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
#else
	(void)v;
	dl_text(t, "t3.built=0\n");
#endif
}

// Its own section: with these keys the t3 section outgrew a reply (truncated=1 before t3.sd_*)
void debuglink_view_t3_svc(const radio_view_t *v, dl_text_t *t)
{
#if CONFIG_DMR_TRUNK
	const radio_t3_view_t *x = &v->t3;
	uint32_t now = hal_millis();
	// Status (6.6.6): a poll's answer, the radio's own, the last one heard (age -1: none)
	dl_text(t, "t3.ax_info=%u\nt3.own_status=%u\nt3.st_count=%u\nt3.st_src=%u\nt3.st_dst=%u\nt3.st_group=%u\n"
		"t3.st_value=%u\nt3.st_age_ms=%d\n", x->ax_info, x->own_status, (unsigned)x->st_count,
		(unsigned)x->st_src, (unsigned)x->st_dst, x->st_group, x->st_value,
		x->st_ms != 0 ? (int)(now - x->st_ms) : -1);
	// The registration's talkgroup list (6.4.4.1.13.2): how many went, which were taken (bit 6 the first)
	dl_text(t, "t3.reg_tgs=%u\nt3.reg_tg_ok=0x%02x\n", x->reg_tgs, x->reg_tg_ok);
	dl_text(t, "t3.call_data=%u\nt3.pd_wait=%u\n", x->call_data, x->pd_wait);
	// The last short data heard: its octets in hex (sd_data), the format to read them by
	char sd[2 * sizeof(x->sd_data) + 1];
	uint8_t len = x->sd_len < sizeof(x->sd_data) ? x->sd_len : (uint8_t)sizeof(x->sd_data);
	for (unsigned i = 0; i < len; i++) {
		static const char digits[] = "0123456789abcdef";
		sd[2 * i] = digits[x->sd_data[i] >> 4];
		sd[2 * i + 1] = digits[x->sd_data[i] & 15u];
	}
	sd[2 * len] = '\0';
	dl_text(t, "t3.sd_count=%u\nt3.sd_src=%u\nt3.sd_dst=%u\nt3.sd_group=%u\nt3.sd_format=%u\nt3.sd_len=%u\n"
		"t3.sd_data=%s\nt3.sd_age_ms=%d\n", (unsigned)x->sd_count, (unsigned)x->sd_src, (unsigned)x->sd_dst,
		x->sd_group, x->sd_format, x->sd_len, sd, x->sd_ms != 0 ? (int)(now - x->sd_ms) : -1);
#else
	(void)v;
	dl_text(t, "t3.built=0\n");
#endif
}
