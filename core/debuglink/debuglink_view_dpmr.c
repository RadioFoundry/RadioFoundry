// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The dpmr section: dPMR on the channel (docs/spec/dpmr.md 7, core/radio/radio_dpmr.h): the call heard, what PTT sends,
// the settings, what the board's provider gives dPMR, the engine's counters. The voice path's are the DMR side's
// (dmr_stats, ambe.*). "dpmr.built=0" without CONFIG_DPMR. IDs and channel codes in hex, as the standard writes them
#include "config.h"
#include "debuglink/debuglink.h"
#if CONFIG_DPMR
#include "radio/radio_dpmr.h"
#endif

void debuglink_view_dpmr(const radio_view_t *v, dl_text_t *t)
{
#if CONFIG_DPMR
	const radio_dpmr_view_t *d = &v->dpmr;
	radio_dpmr_settings_t s;
	radio_dpmr_stats_t c;

	// The call heard now, or the last one (on=0: the engine is not running on this channel)
	dl_text(t, "dpmr.built=1\ndpmr.on=%u\ndpmr.rx=%u\ndpmr.played=%u\ndpmr.late=%u\ndpmr.ended=%u\n",
		(d->flags & RADIO_DPMR_ON) != 0, (d->flags & RADIO_DPMR_RX) != 0, (d->flags & RADIO_DPMR_PLAYED) != 0,
		(d->flags & RADIO_DPMR_LATE) != 0, (d->flags & RADIO_DPMR_ENDED) != 0);
	dl_text(t, "dpmr.have=%u\ndpmr.called=%06X\ndpmr.own=%06X\ndpmr.cc=%03X\ndpmr.tx=%u\ndpmr.deny=%u\n", d->have,
		(unsigned)d->called, (unsigned)d->own, d->cc, d->tx, v->tx_deny);
	// What PTT sends now (tx_cc 0: the frequency's, the engine not running), and what the provider gives dPMR
	// (AIR_FIT_*: 1 receive, 2 transmit, 4 soft values, 32768 untested)
	dl_text(t, "dpmr.talk=%06X\ndpmr.tx_own=%06X\ndpmr.tx_cc=%03X\ndpmr.mine=%u\ndpmr.fit=%u\n", (unsigned)d->talk,
		(unsigned)d->tx_own, d->tx_cc, (d->flags & RADIO_DPMR_MINE) != 0, d->fit);
	radio_dpmr_settings(&s);
	dl_text(t, "dpmr.set.called=%06X\ndpmr.set.own=%06X\ndpmr.set.cc=%u\ndpmr.set.rx_mine=%u\n", (unsigned)s.called,
		(unsigned)s.own, s.cc, s.rx_mine);
	if (radio_dpmr_in_force(&s)) {
		// What the engine runs on: a stored dPMR channel's own, else the settings above (cc: table 6.1's place)
		dl_text(t, "dpmr.run.called=%06X\ndpmr.run.own=%06X\ndpmr.run.cc=%u\ndpmr.run.rx_mine=%u\n",
			(unsigned)s.called, (unsigned)s.own, s.cc, s.rx_mine);
	}
	radio_dpmr_stats(&c);
	dl_text(t, "dpmr.begins=%u\ndpmr.begin_fails=%u\ndpmr.rx_calls=%u\ndpmr.tx_calls=%u\ndpmr.tx_filler=%u\n",
		(unsigned)c.begins, (unsigned)c.begin_fails, (unsigned)c.rx_calls, (unsigned)c.tx_calls,
		(unsigned)c.tx_filler);
	dl_text(t, "dpmr.headers=%u\ndpmr.frames=%u\ndpmr.ends=%u\ndpmr.hi_fail=%u\ndpmr.cch_fail=%u\ndpmr.end_fail=%u\n",
		(unsigned)c.headers, (unsigned)c.frames, (unsigned)c.ends, (unsigned)c.hi_fail, (unsigned)c.cch_fail,
		(unsigned)c.end_fail);
	dl_text(t, "dpmr.voice=%u\ndpmr.voice_held=%u\ndpmr.ham_fixed=%u\ndpmr.ham_bad=%u\ndpmr.rejects=%u\n",
		(unsigned)c.voice, (unsigned)c.voice_held, (unsigned)c.ham_fixed, (unsigned)c.ham_bad,
		(unsigned)c.rejects);
	dl_text(t, "dpmr.rx_chunks=%u\ndpmr.tx_underruns=%u\ndpmr.tx_syms=%u\n", (unsigned)c.rx_chunks,
		(unsigned)c.tx_underruns, (unsigned)c.tx_syms);
#else
	(void)v;
	dl_text(t, "dpmr.built=0\n");
#endif
}

void debuglink_dpmr_reset(void)
{
#if CONFIG_DPMR
	radio_dpmr_reset_stats();
#endif
}
