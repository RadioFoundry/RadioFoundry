// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// NXDN conventional on a channel, the radio core's side (radio_nxdn.h)
#include "config.h"
#if CONFIG_NXDN	// the whole file
#include <stddef.h>
#include <string.h>
#include "board.h"
#include "radio/radio_nxdn.h"
#include "radio/radio_bands.h"
#include "nxdn/nxdn_engine.h"
#include "nxdn/nxdn_codeplug.h"
#include "codeplug/codeplug_modes.h"
#include "dmr/ambe/ambeVoice.h"
#include "dmr/call/dmr_call.h"
#if CONFIG_P25 && !defined(HAL_VOCODER_LEND_BYTES)
#include "radio/radio_p25.h"
#endif

// The 72-bit voice frame NXDN carries is the one the DMR side's voice path takes and gives (nxdn.md 5): the
// standard's second FEC test frame (7.2.1) is the DMR side's AMBE+2 silence frame
_Static_assert(NXDN_EHR_BYTES == DMR_AMBE_FRAME_BYTES, "one voice frame size");

#if CONFIG_P25 && !defined(HAL_VOCODER_LEND_BYTES)
// P25's memory is idle while NXDN runs (one air mode at a time): no copy of our own
#define NXDN_MEM_SHARED	1
#else
#define NXDN_MEM_SHARED	0
static nxdn_engine_t own_mem;
#endif

static const radio_nxdn_settings_t defaults = {
	.rate9600 = true, .rx_ran = NXDN_RAN_ANY, .tx_ran = 1, .rx_gid = 0, .unit_id = 1, .unit_call = false,
	.tx_dst = 1, .emergency = false,
};
static radio_nxdn_settings_t set;
static nxdn_engine_t *eng;		// while running
static radio_nxdn_stats_t stats;
static nxdn_call_stats_t call_base;	// the call layer's counters already counted (they restart with the engine)
static air_session_stats_t air_base;
static uint32_t rx_calls_base, tx_calls_base, filler_base, rejects_base;

void radio_nxdn_init(void)
{
	set = defaults;
	eng = NULL;
}

void radio_nxdn_settings(radio_nxdn_settings_t *out)
{
	*out = set;
}

void radio_nxdn_set_settings(const radio_nxdn_settings_t *s)
{
	set = *s;
	if (set.rx_ran > RADIO_NXDN_RAN_MAX)
		set.rx_ran = NXDN_RAN_ANY;
	if (set.tx_ran > RADIO_NXDN_RAN_MAX)
		set.tx_ran = 1;
	if (set.unit_id == 0 || set.unit_id > RADIO_NXDN_UNIT_MAX)
		set.unit_id = defaults.unit_id;
}

uint32_t radio_nxdn_fit(bool rate9600)
{
	return board.air != NULL ? nxdn_engine_fit(board.air, rate9600) : 0;
}

bool radio_nxdn_fits(void)
{
	return ((radio_nxdn_fit(false) | radio_nxdn_fit(true)) & (AIR_FIT_RX | AIR_FIT_TX)) != 0;
}

bool radio_nxdn_take(channel_t *ch)
{
	if (ch->kind != CODEPLUG_KIND_NXDN || !radio_nxdn_fits() || radio_listen_band(ch->rx_hz))
		return false;
	ch->mode = RADIO_MODE_NXDN;
	ch->bw = RADIO_BW_12K5;
	return true;
}

bool radio_nxdn_receives(const channel_t *ch, uint16_t number)
{
	uint32_t slow = radio_nxdn_fit(false) & AIR_FIT_RX, fast = radio_nxdn_fit(true) & AIR_FIT_RX;
	nxdn_conv_cfg_t cc;

	if (slow == fast)
		return slow != 0;	// the rate doesn't matter: no channel record to read
	return (number != 0 && nxdn_conv_load(number, ch, &cc) ? cc.conv.rate9600 : set.rate9600) ? fast != 0 : slow != 0;
}

// The engine's configuration for channel ch at number (0: the VFO): the one place NXDN parameters come from. A live
// stored NXDN channel's tag and record give them (docs/spec/nxdn-codeplug.md 2), else the settings
static void params(const channel_t *ch, uint16_t number, nxdn_engine_cfg_t *c)
{
	nxdn_conv_cfg_t cc;

	memset(c, 0, sizeof(*c));
	c->silence = dmr_ambe_silence;
	if (number != 0 && nxdn_conv_load(number, ch, &cc)) {
		// Selective: the channel's group, all and unit calls to us; a channel that calls a unit or all keeps the
		// last two
		c->rate9600 = cc.conv.rate9600;
		c->rx_ran = cc.conv.rx_ran;
		c->tx_ran = cc.conv.tx_ran;
		if (cc.conv.squelch == NXDN_SQUELCH_SELECTIVE)
			c->rx_gid = cc.target_unit ? (uint16_t)NXDN_ID_ALL : cc.target;
		c->unit_id = cc.unit_id;
		c->tx_call_type = cc.target_unit ? NXDN_CALL_INDIVIDUAL : NXDN_CALL_GROUP;
		c->tx_dst = cc.target;
		return;
	}
	c->rate9600 = set.rate9600;
	c->rx_ran = set.rx_ran;
	c->tx_ran = set.tx_ran;
	c->rx_gid = set.rx_gid;
	c->unit_id = set.unit_id;
	c->tx_call_type = set.unit_call ? NXDN_CALL_INDIVIDUAL : NXDN_CALL_GROUP;
	c->tx_dst = set.tx_dst;
	c->emergency = set.emergency;
}

// ── Voice: the DMR side's voice path (ambeVoice.h), its sink on receive and its source on transmit. The hooks run in
// the provider's task, which is the only one feeding that path while NXDN runs (the DMR stack is stopped); the
// audio stream is started and stopped by the radio task (begin, end) ──
static void v_start(const nxdn_call_t *c, void *ctx)
{
	const dmr_voice_sink_t *s = ambeVoiceSink();

	(void)c;
	(void)ctx;
	s->begin(NULL, s->ctx);
}

static void v_voice(const uint8_t ehr[NXDN_EHR_BYTES], void *ctx)
{
	const dmr_voice_sink_t *s = ambeVoiceSink();

	(void)ctx;
	s->frame(ehr, s->ctx);
}

static void v_end(void *ctx)
{
	const dmr_voice_sink_t *s = ambeVoiceSink();

	(void)ctx;
	s->end(s->ctx);
}

static bool v_tx(uint8_t ehr[NXDN_EHR_BYTES], void *ctx)
{
	const dmr_voice_source_t *s = ambeVoiceSource();

	(void)ctx;
	return s->next(ehr, s->ctx);
}

static const nxdn_engine_hooks_t voice_hooks = { v_start, NULL, v_voice, v_end, v_tx };

// The encoder runs from the header on, so the first voice frames are ready when the engine asks
static void voice_tx(bool on)
{
	const dmr_voice_source_t *s = ambeVoiceSource();

	if (on)
		s->begin(s->ctx);
	else
		s->end(s->ctx);
}

static nxdn_engine_t *mem_get(void)
{
#if NXDN_MEM_SHARED
	size_t len = 0;
	void *m = radio_p25_idle_mem(&len);

	if (m == NULL || len < sizeof(nxdn_engine_t) || ((uintptr_t)m & 7u) != 0)
		return NULL;
	return m;
#else
	return &own_mem;
#endif
}

bool radio_nxdn_begin(const channel_t *ch, uint16_t number)
{
	nxdn_engine_cfg_t cfg;

	radio_nxdn_end();
	stats.begins++;
	params(ch, number, &cfg);
	eng = mem_get();
	if (eng == NULL || board.air == NULL || nxdn_engine_fit(board.air, cfg.rate9600) == 0) {
		eng = NULL;
		stats.begin_fails++;
		return false;
	}
	ambeVoiceStart();	// before the engine: its first call may come at once
	if (!nxdn_engine_begin(eng, board.air, &cfg, &voice_hooks, NULL)) {
		ambeVoiceStop();
		eng = NULL;
		stats.begin_fails++;
		return false;
	}
	memset(&call_base, 0, sizeof(call_base));
	memset(&air_base, 0, sizeof(air_base));
	rx_calls_base = tx_calls_base = filler_base = rejects_base = 0;
	return true;
}

void radio_nxdn_end(void)
{
	if (eng == NULL)
		return;
	nxdn_engine_end(eng);	// the provider closed: no hook runs after this
	ambeVoiceStop();
	eng = NULL;
}

bool radio_nxdn_running(void)
{
	return eng != NULL;
}

void radio_nxdn_retuned(void)
{
	if (eng != NULL)
		nxdn_engine_retuned(eng);
}

void radio_nxdn_apply(const channel_t *ch, uint16_t number)
{
	nxdn_engine_cfg_t cfg;

	if (eng == NULL || nxdn_engine_tx_state(eng) != AIR_TX_IDLE)
		return;
	params(ch, number, &cfg);
	if (cfg.rate9600 != eng->cfg.rate9600) {
		radio_nxdn_begin(ch, number);	// the mode is the session's: a new rate is a new session
		return;
	}
	nxdn_engine_set(eng, &cfg);
}

// The engine's counters into ours; they restart with each engine, so ours add what moved since the last look
static void count(const nxdn_engine_view_t *ev)
{
	const nxdn_call_stats_t *c = &eng->rx.stats;
	const air_session_stats_t *a = &eng->session.stats;

	stats.rx_calls += ev->rx_calls - rx_calls_base;
	stats.tx_calls += ev->tx_calls - tx_calls_base;
	stats.tx_filler += ev->tx_filler - filler_base;
	rx_calls_base = ev->rx_calls;
	tx_calls_base = ev->tx_calls;
	filler_base = ev->tx_filler;
	stats.frames += c->frames - call_base.frames;
	stats.lich_fail += c->lich_fail - call_base.lich_fail;
	stats.sacch_fail += c->sacch_fail - call_base.sacch_fail;
	stats.facch_fail += c->facch_fail - call_base.facch_fail;
	stats.voice += c->voice - call_base.voice;
	stats.voice_held += c->voice_held - call_base.voice_held;
	stats.fec_bits += c->fec_bits - call_base.fec_bits;
	call_base = *c;
	stats.ran_rejects += eng->rx.rx.ran_rejects - rejects_base;
	rejects_base = eng->rx.rx.ran_rejects;
	stats.rx_chunks += a->rx_chunks - air_base.rx_chunks;
	stats.tx_underruns += a->tx_underruns - air_base.tx_underruns;
	stats.tx_syms += a->tx_syms - air_base.tx_syms;
	air_base = *a;
}

// The engine's configuration as settings: what the screen, the view and the debug link show
static void cfg_settings(const nxdn_engine_cfg_t *c, radio_nxdn_settings_t *out)
{
	out->rate9600 = c->rate9600;
	out->rx_ran = c->rx_ran;
	out->tx_ran = c->tx_ran;
	out->rx_gid = c->rx_gid;
	out->unit_id = c->unit_id;
	out->unit_call = c->tx_call_type == NXDN_CALL_INDIVIDUAL;
	out->tx_dst = c->tx_dst;
	out->emergency = c->emergency;
}

bool radio_nxdn_in_force(radio_nxdn_settings_t *out)
{
	nxdn_engine_t *e = eng;

	if (e == NULL)
		return false;
	cfg_settings(&e->cfg, out);
	return true;
}

// What the screen shows of the parameters: the RANs, the rate and what PTT calls. Those in force while the engine runs
// (a stored channel's own), else the settings
static void settings_view(radio_nxdn_view_t *n)
{
	radio_nxdn_settings_t s = set;

	if (eng != NULL)
		cfg_settings(&eng->cfg, &s);
	n->rx_ran = s.rx_ran;
	n->tx_ran = s.tx_ran;
	n->talk = s.tx_dst;
	n->fit = (uint16_t)radio_nxdn_fit(s.rate9600);
	if (s.unit_call)
		n->flags |= RADIO_NXDN_TALK_UNIT;
	if (s.rate9600)
		n->flags |= RADIO_NXDN_9600;
}

bool radio_nxdn_poll(radio_nxdn_view_t *v)
{
	nxdn_engine_view_t ev;
	radio_nxdn_view_t n;

	if (eng == NULL) {
		n = *v;		// the last call heard stays
		n.flags = 0;
		n.tx = AIR_TX_IDLE;
	} else {
		nxdn_engine_view(eng, &ev);
		count(&ev);
		memset(&n, 0, sizeof(n));
		n.flags = RADIO_NXDN_ON;
		if (ev.rx_active) {
			n.flags |= RADIO_NXDN_RX;
			if (ev.rx_played)
				n.flags |= RADIO_NXDN_PLAYED;
		}
		if (ev.rx.iv || (ev.rx.have_vcall && ev.rx.vcall.cipher != NXDN_CIPHER_NONE))
			n.flags |= RADIO_NXDN_ENC;
		if (ev.rx.late)
			n.flags |= RADIO_NXDN_LATE;
		if (ev.rx.have_vcall && ev.rx.vcall.call_type == NXDN_CALL_INDIVIDUAL)
			n.flags |= RADIO_NXDN_UNIT;
		n.tx = ev.tx;
		n.ran = ev.rx.ran;
		n.src = ev.rx.have_vcall ? ev.rx.vcall.src : 0;
		n.dst = ev.rx.have_vcall ? ev.rx.vcall.dst : 0;
		n.emergency = ev.rx_active && ev.rx.have_vcall && (ev.rx.vcall.cc_opt & NXDN_CC_EMERGENCY) != 0;
	}
	settings_view(&n);
	bool changed = memcmp(&n, v, sizeof(n)) != 0;
	*v = n;
	return changed;
}

static bool nxdn_no_tx(void)
{
	return eng != NULL && (eng->session.fit & AIR_FIT_TX) == 0;
}

static bool nxdn_tx_prepare(void)
{
	if (eng == NULL || !nxdn_engine_tx_prepare(eng))
		return false;
	voice_tx(true);
	return true;
}

static bool nxdn_tx_go(void)
{
	return eng != NULL && nxdn_engine_tx_go(eng);
}

static void nxdn_tx_stop(void)
{
	if (eng != NULL) {
		voice_tx(false);	// the engine fills the frame on air with silence
		nxdn_engine_tx_stop(eng);
	}
}

static void nxdn_tx_abort(void)
{
	if (eng != NULL) {
		voice_tx(false);
		nxdn_engine_tx_abort(eng);
	}
}

static uint8_t nxdn_tx_state(void)
{
	return eng != NULL ? (uint8_t)nxdn_engine_tx_state(eng) : (uint8_t)AIR_TX_IDLE;
}

static void nxdn_tx_clear(void)
{
	if (eng != NULL) {
		voice_tx(false);
		nxdn_engine_tx_clear(eng);
	}
}

const radio_air_ops_t radio_nxdn_air = {
	radio_nxdn_running, nxdn_no_tx, nxdn_tx_prepare, nxdn_tx_go, nxdn_tx_stop, nxdn_tx_abort, nxdn_tx_state,
	nxdn_tx_clear,
};

void radio_nxdn_stats(radio_nxdn_stats_t *out)
{
	*out = stats;
}

void radio_nxdn_reset_stats(void)
{
	memset(&stats, 0, sizeof(stats));
}

#endif	// CONFIG_NXDN
