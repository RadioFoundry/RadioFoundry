// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The dstar section: D-STAR DV on the channel (docs/spec/dstar.md 6, core/radio/radio_dstar.h): the call heard, the
// settings and what we send, what the board's provider gives the mode, the engine's and the voice path's counters.
// "dstar.built=0" without CONFIG_DSTAR
#include "config.h"
#include "debuglink/debuglink.h"
#if CONFIG_DSTAR
#include "radio/radio_dstar.h"
#include "dstar/dstarVoice.h"
#endif

void debuglink_view_dstar(const radio_view_t *v, dl_text_t *t)
{
#if CONFIG_DSTAR
	const radio_dstar_view_t *n = &v->dstar;
	radio_dstar_settings_t s;
	radio_dstar_stats_t c;
	dstar_voice_stats_t vs;
	char my[RADIO_DSTAR_CALL + 1], ur[RADIO_DSTAR_CALL + 1], r1[RADIO_DSTAR_CALL + 1], r2[RADIO_DSTAR_CALL + 1];

	// The call heard now, or the last one (on=0: the engine is not running on this channel)
	dl_text(t, "dstar.built=1\ndstar.on=%u\ndstar.rx=%u\ndstar.header=%u\ndstar.late=%u\ndstar.repeater=%u\n",
		(n->flags & RADIO_DSTAR_ON) != 0, (n->flags & RADIO_DSTAR_RX) != 0, (n->flags & RADIO_DSTAR_HDR) != 0,
		(n->flags & RADIO_DSTAR_LATE) != 0, (n->flags & RADIO_DSTAR_RPT) != 0);
	dl_text(t, "dstar.my=%s\ndstar.sfx=%s\ndstar.ur=%s\ndstar.rpt1=%s\ndstar.msg=%s\ndstar.emergency=%u\n", n->my, n->sfx,
		n->ur, n->rpt1, n->msg, (n->flags & RADIO_DSTAR_EMERG) != 0);
	// What the board's provider gives D-STAR (AIR_FIT_*: 1 receive, 2 transmit, 4 soft values, 8 its own pulse
	// shape, 32768 untested)
	dl_text(t, "dstar.fit=%u\ndstar.tx=%u\ndstar.deny=%u\n", n->fit, n->tx, v->tx_deny);
	radio_dstar_settings(&s);
	dl_text(t, "dstar.set.my=%s\ndstar.set.sfx=%s\ndstar.set.ur=%s\ndstar.set.rpt1=%s\ndstar.set.rpt2=%s\n", s.my, s.sfx,
		s.ur, s.rpt1, s.rpt2);
	dl_text(t, "dstar.set.msg=%s\n", s.msg);
	if (radio_dstar_tx_header(my, ur, r1, r2)) {
		// The header we send now: the settings filled in (MY from the radio name, CQCQCQ, DIRECT)
		dl_text(t, "dstar.send.my=%s\ndstar.send.ur=%s\ndstar.send.rpt1=%s\ndstar.send.rpt2=%s\n", my, ur, r1, r2);
	}
	radio_dstar_stats(&c);
	dl_text(t, "dstar.begins=%u\ndstar.begin_fails=%u\ndstar.rx_calls=%u\ndstar.tx_calls=%u\ndstar.tx_filler=%u\n",
		(unsigned)c.begins, (unsigned)c.begin_fails, (unsigned)c.rx_calls, (unsigned)c.tx_calls,
		(unsigned)c.tx_filler);
	dl_text(t, "dstar.headers=%u\ndstar.header_fail=%u\ndstar.late_entries=%u\ndstar.frames=%u\ndstar.voice=%u\n",
		(unsigned)c.headers, (unsigned)c.header_fail, (unsigned)c.late, (unsigned)c.frames, (unsigned)c.voice);
	dl_text(t, "dstar.syncs=%u\ndstar.ends=%u\ndstar.lost=%u\ndstar.slow_msgs=%u\ndstar.slow_headers=%u\n",
		(unsigned)c.syncs, (unsigned)c.ends, (unsigned)c.lost, (unsigned)c.slow_msgs, (unsigned)c.slow_headers);
	dl_text(t, "dstar.slow_text=%u\ndstar.rx_chunks=%u\ndstar.tx_underruns=%u\ndstar.tx_syms=%u\n",
		(unsigned)c.slow_text, (unsigned)c.rx_chunks, (unsigned)c.tx_underruns, (unsigned)c.tx_syms);
	dstarVoiceGetStats(&vs);
	dl_text(t, "dstar.v.rx_in=%u\ndstar.v.rx_frames=%u\ndstar.v.rx_bad=%u\ndstar.v.rx_concealed=%u\n",
		(unsigned)vs.rx_in, (unsigned)vs.rx_frames, (unsigned)vs.rx_bad, (unsigned)vs.rx_concealed);
	dl_text(t, "dstar.v.rx_underruns=%u\ndstar.v.rx_bit_errors=%u\ndstar.v.rx_peak=%u\ndstar.v.tx_frames=%u\n",
		(unsigned)vs.rx_underruns, (unsigned)vs.rx_bit_errors, (unsigned)vs.rx_peak, (unsigned)vs.tx_frames);
	dl_text(t, "dstar.v.tx_encoded=%u\ndstar.v.tx_dropped=%u\ndstar.v.tx_silent=%u\ndstar.v.tx_mic_peak=%u\n",
		(unsigned)vs.tx_encoded, (unsigned)vs.tx_dropped, (unsigned)vs.tx_silent, (unsigned)vs.tx_mic_peak);
#else
	(void)v;
	dl_text(t, "dstar.built=0\n");
#endif
}

void debuglink_dstar_reset(void)
{
#if CONFIG_DSTAR
	radio_dstar_reset_stats();
	dstarVoiceResetStats();
#endif
}
