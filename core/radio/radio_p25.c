// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// P25 conventional on a channel, the radio core's side (radio_p25.h)
#include "config.h"
#if CONFIG_P25	// the whole file
#include <stddef.h>
#include <string.h>
#include "board.h"
#include "radio/radio_p25.h"
#include "p25/p25_codeplug.h"
#include "codeplug/codeplug_modes.h"
#include "radio/radio_bands.h"
#if CONFIG_IMBE
#include "p25/p25Voice.h"
#endif

// What runs while P25 is on: the engine and, with the decoder, the voice path's queue and decoder memory
typedef struct {
	p25_engine_t eng;
#if CONFIG_IMBE
	p25_voice_mem_t voice;
#endif
} p25_mem_t;

#ifdef HAL_VOCODER_LEND_BYTES
// The platform's DMR vocoder lends its memory while P25 runs (hal_vocoder_t.lend): no copy here
_Static_assert(sizeof(p25_mem_t) <= HAL_VOCODER_LEND_BYTES, "P25 does not fit the memory the vocoder lends");
#else
static p25_mem_t own_mem;
#endif

#if CONFIG_IMBE
#define SILENCE		imbe_silence
#else
// No decoder in the build: the silence frame (pitch code 216) for what the engine sends
static const uint8_t silence_frame[P25_IMBE_BYTES] = { 0xD8 };
#define SILENCE		silence_frame
#endif

static const radio_p25_settings_t defaults = {
	.rx_nac = P25_NAC_DEFAULT, .tx_nac = P25_NAC_DEFAULT, .rx_tgid = 0, .talkaround = RADIO_P25_TA_AUTO,
	.unit_call = false, .tx_dst = 1, .unit_id = 0,
};
static radio_p25_settings_t set;
static p25_mem_t *mem;			// while running
static radio_p25_stats_t stats;
static p25_call_stats_t call_base;	// the call layer's counters when the engine started (they restart with it)
static air_session_stats_t air_base;
static uint32_t rx_calls_base, tx_calls_base, filler_base;

void radio_p25_init(const hal_audio_t *audio)
{
	set = defaults;
	mem = NULL;
#if CONFIG_IMBE
	p25VoiceInit(audio);
#else
	(void)audio;
#endif
}

void radio_p25_settings(radio_p25_settings_t *out)
{
	*out = set;
}

void radio_p25_set_settings(const radio_p25_settings_t *s)
{
	set = *s;
	set.rx_nac &= 0xFFFu;
	set.tx_nac &= 0xFFFu;
	set.unit_id &= 0xFFFFFFu;
	set.tx_dst &= set.unit_call ? 0xFFFFFFu : 0xFFFFu;
	if (set.talkaround > RADIO_P25_TA_OFF)
		set.talkaround = RADIO_P25_TA_AUTO;
}

// What PTT calls: talk group or unit dst
static void talk_lc(p25_lc_t *lc, bool unit, uint32_t dst)
{
	lc->lcf = unit ? P25_LCF_UNIT : P25_LCF_GROUP;
	if (unit)
		lc->dst = dst & 0xFFFFFFu;
	else
		lc->tgid = (uint16_t)dst;
}

void radio_p25_params(const channel_t *ch, uint16_t number, uint32_t own_id, p25_engine_cfg_t *cfg)
{
	p25_conv_cfg_t cc;

	memset(cfg, 0, sizeof(*cfg));
	cfg->silence = SILENCE;
	if (number != 0 && p25_conv_load(number, ch, &cc)) {
		// A stored P25 channel (docs/spec/p25-codeplug.md 2). Selective: its talk group, everyone and unit calls to
		// us; a channel that calls a unit or everyone keeps only the last two
		cfg->rx_nac = cc.conv.rx_nac;
		cfg->tx_nac = cc.conv.tx_nac;
		if (cc.conv.squelch == P25_SQUELCH_SELECTIVE)
			cfg->rx_tgid = cc.target_unit ? (uint16_t)P25_TGID_EVERYONE : (uint16_t)cc.target;
		cfg->unit_id = (set.unit_id != 0 ? set.unit_id : cc.unit_id) & 0xFFFFFFu;
		talk_lc(&cfg->tx_lc, cc.target_unit, cc.target);
		cfg->talkaround = cc.talkaround;
		return;
	}
	cfg->rx_nac = set.rx_nac;
	cfg->tx_nac = set.tx_nac;
	cfg->rx_tgid = set.rx_tgid;
	cfg->unit_id = (set.unit_id != 0 ? set.unit_id : p25_radio_unit_id(own_id)) & 0xFFFFFFu;
	talk_lc(&cfg->tx_lc, set.unit_call, set.tx_dst);
	cfg->talkaround = set.talkaround == RADIO_P25_TA_ON ||
			  (set.talkaround == RADIO_P25_TA_AUTO && (ch->tx_hz == ch->rx_hz || ch->tx_hz == 0));
}

// The engine's configuration as settings: what the screen, the view and the debug link show
static void cfg_settings(const p25_engine_cfg_t *c, radio_p25_settings_t *out)
{
	out->rx_nac = c->rx_nac;
	out->tx_nac = c->tx_nac;
	out->rx_tgid = c->rx_tgid;
	out->talkaround = c->talkaround ? RADIO_P25_TA_ON : RADIO_P25_TA_OFF;
	out->unit_call = c->tx_lc.lcf == P25_LCF_UNIT;
	out->tx_dst = out->unit_call ? c->tx_lc.dst : c->tx_lc.tgid;
	out->unit_id = c->unit_id;
}

bool radio_p25_in_force(radio_p25_settings_t *out)
{
	p25_mem_t *m = mem;

	if (m == NULL)
		return false;
	cfg_settings(&m->eng.cfg, out);
	return true;
}

bool radio_p25_fits(void)
{
	return board.air != NULL && (p25_engine_fit(board.air) & AIR_FIT_RX) != 0;
}

bool radio_p25_take(channel_t *ch)
{
	if (ch->kind != CODEPLUG_KIND_P25 || !radio_p25_fits() || radio_listen_band(ch->rx_hz))
		return false;
	ch->mode = RADIO_MODE_P25;
	ch->bw = RADIO_BW_12K5;
	return true;
}

static p25_mem_t *mem_get(void)
{
#ifdef HAL_VOCODER_LEND_BYTES
	size_t len = 0;
	void *m = board.vocoder != NULL && board.vocoder->lend != NULL ? board.vocoder->lend(&len) : NULL;
	if (m == NULL || len < sizeof(p25_mem_t) || ((uintptr_t)m & 7u) != 0) {
		return NULL;
	}
	stats.mem_lent++;
	return m;
#else
	return &own_mem;
#endif
}

bool radio_p25_begin(const channel_t *ch, uint16_t number, uint32_t own_id)
{
	p25_engine_cfg_t cfg;
	static const p25_engine_hooks_t no_voice;
	const p25_engine_hooks_t *hooks = &no_voice;

	radio_p25_end();
	stats.begins++;
	mem = mem_get();
	if (mem == NULL || board.air == NULL || p25_engine_fit(board.air) == 0) {
		mem = NULL;
		stats.begin_fails++;
		return false;
	}
	radio_p25_params(ch, number, own_id, &cfg);
#if CONFIG_IMBE
	p25VoiceBegin(&mem->voice);
	hooks = p25VoiceHooks();
#endif
	if (!p25_engine_begin(&mem->eng, board.air, &cfg, hooks, NULL)) {
#if CONFIG_IMBE
		p25VoiceEnd();
#endif
		mem = NULL;
		stats.begin_fails++;
		return false;
	}
	memset(&call_base, 0, sizeof(call_base));
	memset(&air_base, 0, sizeof(air_base));
	rx_calls_base = tx_calls_base = filler_base = 0;
	return true;
}

// The engine's counters into ours; they restart with each engine, so ours add what moved since the last look
static void count(const p25_engine_view_t *ev)
{
	const p25_call_stats_t *c = &mem->eng.rx.stats;
	const air_session_stats_t *a = &mem->eng.session.stats;

	stats.rx_calls += ev->rx_calls - rx_calls_base;
	stats.tx_calls += ev->tx_calls - tx_calls_base;
	stats.tx_filler += ev->tx_filler - filler_base;
	rx_calls_base = ev->rx_calls;
	tx_calls_base = ev->tx_calls;
	filler_base = ev->tx_filler;

	stats.units += c->units - call_base.units;
	stats.voice += c->voice - call_base.voice;
	stats.voice_bad += c->voice_bad - call_base.voice_bad;
	stats.voice_bits += c->voice_bits - call_base.voice_bits;
	stats.lc_fail += c->lc_fail - call_base.lc_fail;
	stats.es_fail += c->es_fail - call_base.es_fail;
	stats.rx_chunks += a->rx_chunks - air_base.rx_chunks;
	stats.tx_underruns += a->tx_underruns - air_base.tx_underruns;
	stats.tx_syms += a->tx_syms - air_base.tx_syms;
	call_base = *c;
	air_base = *a;
}

void radio_p25_end(void)
{
	p25_engine_view_t ev;

	if (mem == NULL) {
		return;
	}
	p25_engine_end(&mem->eng);	// the provider calls nothing after its close
	p25_engine_view(&mem->eng, &ev);
	count(&ev);
#if CONFIG_IMBE
	p25VoiceEnd();
#endif
	mem = NULL;			// the memory goes back (a lending vocoder rebuilds its state at its next call)
}

bool radio_p25_running(void)
{
	return mem != NULL;
}

void radio_p25_retuned(void)
{
	if (mem != NULL) {
		p25_engine_retuned(&mem->eng);
	}
}

void radio_p25_apply(const channel_t *ch, uint16_t number, uint32_t own_id)
{
	p25_engine_cfg_t cfg;

	if (mem == NULL || p25_engine_tx_state(&mem->eng) != AIR_TX_IDLE) {
		return;
	}
	radio_p25_params(ch, number, own_id, &cfg);
	p25_engine_set(&mem->eng, &cfg);
}

// What the screen shows of the parameters: the NAC heard and what PTT calls. Those in force while the engine runs (a
// stored channel's own), else the settings
static void settings_view(radio_p25_view_t *n)
{
	radio_p25_settings_t s = set;

	if (mem != NULL)
		cfg_settings(&mem->eng.cfg, &s);
	n->rx_nac = s.rx_nac;
	n->talk = s.tx_dst;
	if (s.unit_call)
		n->flags |= RADIO_P25_TALK_UNIT;
}

bool radio_p25_poll(radio_p25_view_t *v)
{
	p25_engine_view_t ev;
	radio_p25_view_t n;

	if (mem == NULL) {
		n = *v;		// the last call heard stays
		n.flags = 0;
		n.tx = AIR_TX_IDLE;
		settings_view(&n);
		bool changed = memcmp(&n, v, sizeof(n)) != 0;
		*v = n;
		return changed;
	}
#if CONFIG_IMBE
	p25VoicePoll();
#endif
	p25_engine_view(&mem->eng, &ev);
	count(&ev);
	memset(&n, 0, sizeof(n));
	n.flags = RADIO_P25_ON;
	if (ev.rx_active) {
		n.flags |= RADIO_P25_RX;
		if (ev.rx_played)
			n.flags |= RADIO_P25_PLAYED;
	}
	if (ev.rx.have_alg && ev.rx.algid != P25_ALGID_CLEAR)
		n.flags |= RADIO_P25_ENC;
	if (ev.rx.late)
		n.flags |= RADIO_P25_LATE;
	bool unit = ev.rx.have_lc && ev.rx.lc.lcf == P25_LCF_UNIT;
	if (unit)
		n.flags |= RADIO_P25_UNIT;
	n.tx = ev.tx;
	n.nac = ev.rx.nac;
	n.src = ev.rx.have_lc ? ev.rx.lc.src : 0;
	n.dst = unit ? ev.rx.lc.dst : ev.rx.lc.tgid;
	settings_view(&n);
	bool changed = memcmp(&n, v, sizeof(n)) != 0;
	*v = n;
	return changed;
}

bool radio_p25_tx_prepare(void)
{
	return mem != NULL && p25_engine_tx_prepare(&mem->eng);
}

bool radio_p25_tx_go(void)
{
	return mem != NULL && p25_engine_tx_go(&mem->eng);
}

void radio_p25_tx_stop(void)
{
	if (mem != NULL) {
		p25_engine_tx_stop(&mem->eng);
	}
}

void radio_p25_tx_abort(void)
{
	if (mem != NULL) {
		p25_engine_tx_abort(&mem->eng);
	}
}

uint8_t radio_p25_tx_state(void)
{
	return mem != NULL ? (uint8_t)p25_engine_tx_state(&mem->eng) : (uint8_t)AIR_TX_IDLE;
}

void radio_p25_tx_clear(void)
{
	if (mem != NULL) {
		p25_engine_tx_clear(&mem->eng);
	}
}

void radio_p25_stats(radio_p25_stats_t *out)
{
	*out = stats;
}

void radio_p25_reset_stats(void)
{
	memset(&stats, 0, sizeof(stats));
}

#endif	// CONFIG_P25
