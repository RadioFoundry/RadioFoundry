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
#include "dsp/miclimit.h"
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
		if (board.air->rx_load(&work, &span)) {
			dl_text(t, "p25.rx_work_us=%u\np25.rx_span_us=%u\n", (unsigned)work, (unsigned)span);
		}
	}
	radio_p25_settings(&s);
	dl_text(t, "p25.rx_nac=%u\np25.tx_nac=%u\np25.rx_tgid=%u\np25.talkaround=%u\np25.unit_call=%u\n", s.rx_nac,
		s.tx_nac, s.rx_tgid, s.talkaround, s.unit_call);
	dl_text(t, "p25.tx_dst=%u\np25.unit_id=%u\n", (unsigned)s.tx_dst, (unsigned)s.unit_id);
	if (radio_p25_in_force(&s)) {
		// What the engine runs on: a stored P25 channel's own, else the settings above (talkaround 1 on, 2 off)
		dl_text(t, "p25.run.rx_nac=%u\np25.run.tx_nac=%u\np25.run.rx_tgid=%u\np25.run.talkaround=%u\n", s.rx_nac,
			s.tx_nac, s.rx_tgid, s.talkaround);
		dl_text(t, "p25.run.unit_call=%u\np25.run.tx_dst=%u\np25.run.unit_id=%u\n", s.unit_call,
			(unsigned)s.tx_dst, (unsigned)s.unit_id);
	}
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
	dl_text(t, "p25v.tx_mic_low_db=%d\n", miclimit_q8_db((uint16_t)vs.tx_mic_low_q8));
#else
	dl_text(t, "p25v.built=0\n");
#endif
#else
	(void)v;
	dl_text(t, "p25.built=0\n");
#endif
}

// noinline: its copy of the view would otherwise sit in the section dispatcher's frame, under every section's path
__attribute__((noinline)) void debuglink_view_p25_trunk(dl_text_t *t)
{
#if CONFIG_P25_TRUNK
	radio_p25_trunk_view_t tv;
	const p25_follow_view_t *f = &tv.v;

	// The follower as the radio task last saw it (p25-trunking.md 4): on=0 when no trunked channel runs, the rest
	// then the last one's. wacn, sysid, rfss, site all ones until heard; cc 255 none
	radio_p25_trunk_view(&tv);
	dl_text(t, "p25t.built=1\np25t.on=%u\np25t.state=%u\np25t.cc=%u\np25t.cc_hz=%u\np25t.voice_hz=%u\n", tv.on,
		f->state, f->cc, (unsigned)f->cc_hz, (unsigned)f->voice_hz);
	dl_text(t, "p25t.nac=%u\np25t.wacn=%u\np25t.sysid=%u\np25t.rfss=%u\np25t.site=%u\n", f->nac,
		(unsigned)f->wacn, f->sysid, f->rfss, f->site);
	dl_text(t, "p25t.tg=%u\np25t.src=%u\np25t.dst=%u\np25t.unit_call=%u\np25t.enc=%u\n", f->tg,
		(unsigned)f->src, (unsigned)f->dst, f->unit_call, f->enc);
	dl_text(t, "p25t.idens=%u\np25t.learned=%u\np25t.refused=%u\np25t.refused_tg=%u\np25t.fallback=%u\n",
		f->idens, f->learned, f->refused, f->refused_tg, f->fallback);
	dl_text(t, "p25t.blocks=%u\np25t.crc_fail=%u\np25t.crc_taken=%u\np25t.unverified=%u\np25t.mfr=%u\np25t.mbt=%u\n"
		"p25t.mbt_op=%u\np25t.pdu_other=%u\n", (unsigned)f->blocks, (unsigned)f->crc_fail, f->crc_taken, f->unverified,
		f->mfr, f->mbt, f->mbt_op, f->pdu_other);
	dl_text(t, "p25t.grants=%u\np25t.follows=%u\np25t.enc_skip=%u\np25t.tdma=%u\np25t.no_iden=%u\n", f->grants,
		f->follows, f->enc_skip, f->tdma, f->no_iden);
	dl_text(t, "p25t.wrong_sys=%u\np25t.hunts=%u\np25t.cc_lost=%u\np25t.voice_lost=%u\np25t.wrong_call=%u\n",
		f->wrong_sys, f->hunts, f->cc_lost, f->voice_lost, f->wrong_call);
	dl_text(t, "p25t.retunes=%u\np25t.untuned=%u\np25t.skips=%u\n", (unsigned)tv.retunes, (unsigned)tv.untuned,
		(unsigned)tv.skips);
	// The transmit side (p25-trunking.md 5)
	dl_text(t, "p25t.tx=%u\np25t.reg=%u\np25t.req=%u\np25t.aff_tg=%u\np25t.talk_hz=%u\np25t.deny_svc=%u\np25t.deny_reason=%u\n",
		(unsigned)tv.tx, f->reg, f->req, f->aff_tg, (unsigned)f->talk_hz, f->deny_svc, f->deny_reason);
	dl_text(t, "p25t.reg_tx=%u\np25t.reg_ok=%u\np25t.aff_tx=%u\np25t.aff_ok=%u\np25t.req_tx=%u\np25t.req_grant=%u\n",
		f->reg_tx, f->reg_ok, f->aff_tx, f->aff_ok, f->req_tx, f->req_grant);
	dl_text(t, "p25t.denials=%u\np25t.queued=%u\np25t.timeouts=%u\np25t.send_fail=%u\np25t.inb_busy=%u\n",
		f->denials, f->queued, f->timeouts, f->send_fail, f->inb_busy);
	dl_text(t, "p25t.bursts=%u\np25t.burst_refused=%u\n", (unsigned)tv.bursts, (unsigned)tv.burst_refused);
#else
	dl_text(t, "p25t.built=0\n");
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
