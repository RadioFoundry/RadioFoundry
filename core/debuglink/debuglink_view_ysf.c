// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The ysf section: System Fusion DN on the channel (docs/spec/ysf.md 5, core/radio/radio_ysf.h): the call heard, who
// we are on air (the callsign and the radio ID, with where the ID came from), the DG-IDs, what the board's provider
// gives YSF, the engine's counters. The voice path's are the DMR side's (dmr_stats, ambe.*). "ysf.built=0" without
// CONFIG_YSF
#include "config.h"
#include "debuglink/debuglink.h"
#if CONFIG_YSF
#include "radio/radio_ysf.h"
#include "codeplug/codeplug.h"
#endif
#if CONFIG_AIR_RX_ADAPT
#include "air/air_softmodem.h"
#endif

void debuglink_view_ysf(const radio_view_t *v, dl_text_t *t)
{
#if CONFIG_YSF
	const radio_ysf_view_t *y = &v->ysf;
	radio_ysf_settings_t s;
	radio_ysf_stats_t c;
	char rid[RADIO_YSF_RID_LEN + 1], call[RADIO_YSF_CALL_LEN + 1], name[CODEPLUG_NAME_LEN + 1];
	bool weak;

	// The call heard now, or the last one (on=0: the engine is not running on this channel)
	dl_text(t, "ysf.built=1\nysf.on=%u\nysf.rx=%u\nysf.played=%u\nysf.late=%u\nysf.not_dn=%u\n",
		(y->flags & RADIO_YSF_ON) != 0, (y->flags & RADIO_YSF_RX) != 0, (y->flags & RADIO_YSF_PLAYED) != 0,
		(y->flags & RADIO_YSF_LATE) != 0, (y->flags & RADIO_YSF_NOT_DN) != 0);
	dl_text(t, "ysf.src=%s\nysf.src_rid=%s\nysf.dgid=%u\nysf.tx=%u\nysf.deny=%u\n", y->src, y->src_rid, y->dgid, y->tx,
		v->tx_deny);
	// Who we are on air. rid_weak=1: no usable MCU unique ID, the RID is the fallback and may not be unique
	radio_ysf_rid(rid, &weak);
	codeplug_radio_name(name);
	radio_ysf_callsign(name, call);
	dl_text(t, "ysf.rid=%s\nysf.rid_weak=%u\nysf.callsign=%s\n", rid, weak, call);
	// What the board's provider gives YSF (AIR_FIT_*: 1 receive, 2 transmit, 4 soft values, 32768 untested)
	dl_text(t, "ysf.fit=%u\n", (unsigned)radio_ysf_fit());
	radio_ysf_settings(&s);
	dl_text(t, "ysf.tx_dgid=%u\nysf.rx_dgid=%u\nysf.run.tx_dgid=%u\nysf.run.rx_dgid=%u\n", s.tx_dgid, s.rx_dgid,
		y->tx_dgid, y->rx_dgid);
	radio_ysf_stats(&c);
	dl_text(t, "ysf.begins=%u\nysf.begin_fails=%u\nysf.rx_calls=%u\nysf.tx_calls=%u\nysf.tx_filler=%u\n",
		(unsigned)c.begins, (unsigned)c.begin_fails, (unsigned)c.rx_calls, (unsigned)c.tx_calls,
		(unsigned)c.tx_filler);
	dl_text(t, "ysf.frames=%u\nysf.fich_fail=%u\nysf.csd_fail=%u\nysf.dch_fail=%u\nysf.voice=%u\n",
		(unsigned)c.frames, (unsigned)c.fich_fail, (unsigned)c.csd_fail, (unsigned)c.dch_fail, (unsigned)c.voice);
	// fec_bits: bits the FICH, CSD and DCH decoders corrected; vch_fixed: voice bits the three copies' vote fixed
	dl_text(t, "ysf.voice_held=%u\nysf.fec_bits=%u\nysf.vch_fixed=%u\nysf.rx_chunks=%u\n", (unsigned)c.voice_held,
		(unsigned)c.fec_bits, (unsigned)c.vch_fixed, (unsigned)c.rx_chunks);
	dl_text(t, "ysf.tx_underruns=%u\nysf.tx_syms=%u\n", (unsigned)c.tx_underruns, (unsigned)c.tx_syms);
	// Wires-X (ysf.md 9): what was asked (wx_asked counts commands sent), the node's last reply (wx_serial counts
	// them), the page of rooms held
	radio_ysf_wx_t w;

	radio_ysf_wx(&w);
	dl_text(t, "ysf.wx_asked=%u\nysf.wx_asked_cmd=%u\nysf.wx_serial=%u\nysf.wx_kind=%u\nysf.wx_connected=%u\n",
		(unsigned)w.asked, w.asked_cmd, (unsigned)w.serial, w.kind, w.connected);
	dl_text(t, "ysf.wx_node=%s\nysf.wx_node_call=%s\nysf.wx_room=%s %s\nysf.wx_rooms=%u\nysf.wx_rooms_total=%u\n",
		w.node_name, w.node_call, w.room_id, w.room_name, w.rooms, w.rooms_total);
#if CONFIG_AIR_RX_ADAPT
	// The soft modem's equalizer and tone notch (ysf.md 5.4): running now (rx_eq, rx_tone_on), the tone notched in
	// Hz (0.0 none), the center tap, and what the bench turned off (op 44's third byte)
	air_softmodem_adapt_view_t a;

	air_softmodem_adapt_get(&a);
	dl_text(t, "ysf.rx_eq=%u\nysf.rx_tone_on=%u\nysf.rx_tone=%u.%u\nysf.rx_eq_center=%d\nysf.rx_eq_resets=%u\n",
		(a.opts & AIR_RX_EQ) != 0, (a.opts & AIR_RX_TONE) != 0, a.tone_dhz / 10u, a.tone_dhz % 10u, a.eq_center,
		a.eq_resets);
	dl_text(t, "ysf.rx_tone_blocks=%u\nysf.rx_tone_picks=%u\nysf.rx_bench_off=%u\n", a.tone_blocks, a.tone_picks,
		air_softmodem_adapt_off);
#endif
#else
	(void)v;
	dl_text(t, "ysf.built=0\n");
#endif
}

void debuglink_ysf_reset(void)
{
#if CONFIG_YSF
	radio_ysf_reset_stats();
#endif
}
