// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The p25 section: P25 conventional on the channel (docs/spec/p25.md 7, core/radio/radio_p25.h): the call heard, the
// settings in force, the engine's and the voice path's counters. "p25.built=0" without CONFIG_P25
#include "config.h"
#include "debuglink/debuglink.h"
#if CONFIG_P25
#include "board.h"
#include "radio/radio_p25.h"
#if CONFIG_IMBE
#include "p25/p25Voice.h"
#endif
#endif

void debuglink_view_p25(const radio_view_t *v, dl_text_t *t)
{
#if CONFIG_P25
	const radio_p25_view_t *p = &v->p25;
	radio_p25_settings_t s;
	radio_p25_stats_t c;

	// The call heard now, or the last one (on=0: the engine is not running on this channel)
	dl_text(t, "p25.built=1\np25.on=%u\np25.rx=%u\np25.played=%u\np25.enc=%u\np25.unit=%u\np25.late=%u\n",
		(p->flags & RADIO_P25_ON) != 0, (p->flags & RADIO_P25_RX) != 0, (p->flags & RADIO_P25_PLAYED) != 0,
		(p->flags & RADIO_P25_ENC) != 0, (p->flags & RADIO_P25_UNIT) != 0, (p->flags & RADIO_P25_LATE) != 0);
	dl_text(t, "p25.nac=%u\np25.src=%u\np25.dst=%u\np25.tx=%u\n", p->nac, (unsigned)p->src, (unsigned)p->dst,
		p->tx);
	if ((p->flags & RADIO_P25_ON) != 0 && board.air != NULL && board.air->rx_load != NULL) {
		uint32_t work, span;

		// What receive costs the MCU since the engine opened the provider: work / span is the share of the CPU.
		// Early in the section: a reply cut short loses counters, not this
		board.air->rx_load(&work, &span);
		dl_text(t, "p25.rx_work_us=%u\np25.rx_span_us=%u\n", (unsigned)work, (unsigned)span);
	}
	radio_p25_settings(&s);
	dl_text(t, "p25.rx_nac=%u\np25.tx_nac=%u\np25.rx_tgid=%u\np25.talkaround=%u\np25.unit_call=%u\n", s.rx_nac,
		s.tx_nac, s.rx_tgid, s.talkaround, s.unit_call);
	dl_text(t, "p25.tx_dst=%u\np25.unit_id=%u\n", (unsigned)s.tx_dst, (unsigned)s.unit_id);
	radio_p25_stats(&c);
	dl_text(t, "p25.begins=%u\np25.begin_fails=%u\np25.mem_lent=%u\np25.rx_calls=%u\np25.tx_calls=%u\n",
		(unsigned)c.begins, (unsigned)c.begin_fails, (unsigned)c.mem_lent, (unsigned)c.rx_calls,
		(unsigned)c.tx_calls);
	dl_text(t, "p25.tx_filler=%u\np25.units=%u\np25.voice=%u\np25.voice_bad=%u\np25.voice_bits=%u\n",
		(unsigned)c.tx_filler, (unsigned)c.units, (unsigned)c.voice, (unsigned)c.voice_bad,
		(unsigned)c.voice_bits);
	dl_text(t, "p25.lc_fail=%u\np25.es_fail=%u\np25.rx_chunks=%u\np25.tx_underruns=%u\np25.tx_syms=%u\n",
		(unsigned)c.lc_fail, (unsigned)c.es_fail, (unsigned)c.rx_chunks, (unsigned)c.tx_underruns,
		(unsigned)c.tx_syms);
#if CONFIG_IMBE
	p25_voice_stats_t vs;

	p25VoiceGetStats(&vs);
	dl_text(t, "p25v.built=1\np25v.in=%u\np25v.frames=%u\np25v.bad=%u\np25v.concealed=%u\np25v.calls=%u\n",
		(unsigned)vs.rx_in, (unsigned)vs.rx_frames, (unsigned)vs.rx_bad, (unsigned)vs.rx_concealed,
		(unsigned)vs.rx_calls);
	dl_text(t, "p25v.underruns=%u\np25v.underrun_at=%u\np25v.overflows=%u\np25v.bit_errors=%u\n",
		(unsigned)vs.rx_underruns, (unsigned)vs.rx_underrun_at, (unsigned)vs.rx_overflows,
		(unsigned)vs.rx_bit_errors);
	dl_text(t, "p25v.queue_max=%u\np25v.peak=%u\np25v.tx_frames=%u\n", (unsigned)vs.rx_queue_max,
		(unsigned)vs.rx_peak, (unsigned)vs.tx_frames);
	dl_text(t, "p25v.tx_encoded=%u\np25v.tx_dropped=%u\np25v.tx_silent=%u\np25v.tx_mic_peak=%u\n",
		(unsigned)vs.tx_encoded, (unsigned)vs.tx_dropped, (unsigned)vs.tx_silent, (unsigned)vs.tx_mic_peak);
#else
	dl_text(t, "p25v.built=0\n");
#endif
#else
	(void)v;
	dl_text(t, "p25.built=0\n");
#endif
}

void debuglink_p25_reset(void)
{
#if CONFIG_P25
	radio_p25_reset_stats();
#if CONFIG_IMBE
	p25VoiceResetStats();
#endif
#endif
}
