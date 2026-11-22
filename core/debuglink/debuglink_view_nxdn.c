// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The nxdn section: NXDN conventional on the channel (docs/spec/nxdn.md 6, core/radio/radio_nxdn.h): the call heard,
// the settings, what the board's provider gives the rate, the engine's counters. The voice path's are the DMR side's
// (dmr_stats, ambe.*). "nxdn.built=0" without CONFIG_NXDN
#include "config.h"
#include "debuglink/debuglink.h"
#if CONFIG_NXDN
#include "radio/radio_nxdn.h"
#endif

void debuglink_view_nxdn(const radio_view_t *v, dl_text_t *t)
{
#if CONFIG_NXDN
	const radio_nxdn_view_t *n = &v->nxdn;
	radio_nxdn_settings_t s;
	radio_nxdn_stats_t c;

	// The call heard now, or the last one (on=0: the engine is not running on this channel)
	dl_text(t, "nxdn.built=1\nnxdn.on=%u\nnxdn.rx=%u\nnxdn.played=%u\nnxdn.enc=%u\nnxdn.unit=%u\nnxdn.late=%u\n",
		(n->flags & RADIO_NXDN_ON) != 0, (n->flags & RADIO_NXDN_RX) != 0, (n->flags & RADIO_NXDN_PLAYED) != 0,
		(n->flags & RADIO_NXDN_ENC) != 0, (n->flags & RADIO_NXDN_UNIT) != 0, (n->flags & RADIO_NXDN_LATE) != 0);
	dl_text(t, "nxdn.ran=%u\nnxdn.src=%u\nnxdn.dst=%u\nnxdn.emergency=%u\nnxdn.tx=%u\nnxdn.deny=%u\n", n->ran, n->src,
		n->dst, n->emergency, n->tx, v->tx_deny);
#if CONFIG_NXDN_OTA_ALIAS
	dl_text(t, "nxdn.alias=%s\n", n->alias);
#endif
	// What the board's provider gives the rate in force (AIR_FIT_*: 1 receive, 2 transmit, 4 soft values, 32 rate
	// doubled, 32768 untested), each rate's too
	dl_text(t, "nxdn.fit=%u\nnxdn.fit4800=%u\nnxdn.fit9600=%u\n", n->fit, (unsigned)radio_nxdn_fit(false),
		(unsigned)radio_nxdn_fit(true));
	radio_nxdn_settings(&s);
	dl_text(t, "nxdn.rate=%u\nnxdn.rx_ran=%u\nnxdn.tx_ran=%u\nnxdn.rx_gid=%u\nnxdn.unit_id=%u\n",
		s.rate9600 ? 9600u : 4800u, s.rx_ran, s.tx_ran, s.rx_gid, s.unit_id);
	dl_text(t, "nxdn.unit_call=%u\nnxdn.tx_dst=%u\nnxdn.tx_emergency=%u\n", s.unit_call, s.tx_dst, s.emergency);
	if (radio_nxdn_in_force(&s)) {
		// What the engine runs on: a stored NXDN channel's own, else the settings above
		dl_text(t, "nxdn.run.rate=%u\nnxdn.run.rx_ran=%u\nnxdn.run.tx_ran=%u\nnxdn.run.rx_gid=%u\n",
			s.rate9600 ? 9600u : 4800u, s.rx_ran, s.tx_ran, s.rx_gid);
		dl_text(t, "nxdn.run.unit_id=%u\nnxdn.run.unit_call=%u\nnxdn.run.tx_dst=%u\n", s.unit_id, s.unit_call,
			s.tx_dst);
	}
	radio_nxdn_stats(&c);
	dl_text(t, "nxdn.begins=%u\nnxdn.begin_fails=%u\nnxdn.rx_calls=%u\nnxdn.tx_calls=%u\nnxdn.tx_filler=%u\n",
		(unsigned)c.begins, (unsigned)c.begin_fails, (unsigned)c.rx_calls, (unsigned)c.tx_calls,
		(unsigned)c.tx_filler);
	dl_text(t, "nxdn.frames=%u\nnxdn.lich_fail=%u\nnxdn.sacch_fail=%u\nnxdn.facch_fail=%u\nnxdn.voice=%u\n",
		(unsigned)c.frames, (unsigned)c.lich_fail, (unsigned)c.sacch_fail, (unsigned)c.facch_fail,
		(unsigned)c.voice);
	dl_text(t, "nxdn.voice_held=%u\nnxdn.fec_bits=%u\nnxdn.ran_rejects=%u\nnxdn.rx_chunks=%u\n",
		(unsigned)c.voice_held, (unsigned)c.fec_bits, (unsigned)c.ran_rejects, (unsigned)c.rx_chunks);
	dl_text(t, "nxdn.tx_underruns=%u\nnxdn.tx_syms=%u\n", (unsigned)c.tx_underruns, (unsigned)c.tx_syms);
#else
	(void)v;
	dl_text(t, "nxdn.built=0\n");
#endif
}

void debuglink_nxdn_reset(void)
{
#if CONFIG_NXDN
	radio_nxdn_reset_stats();
#endif
}
