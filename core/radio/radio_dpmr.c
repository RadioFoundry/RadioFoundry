// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// dPMR on a channel, the radio core's side (radio_dpmr.h). Its shape is NXDN's (radio_nxdn.c): the same engine shape,
// the same voice path, the same memory; what differs is where the parameters come from
#include "config.h"
#if CONFIG_DPMR	// the whole file
#include <stddef.h>
#include <string.h>
#include "board.h"
#include "radio/radio_dpmr.h"
#include "radio/radio_bands.h"
#include "dpmr/dpmr_engine.h"
#include "dpmr/dpmr_codeplug.h"
#include "codeplug/codeplug_modes.h"
#include "dmr/ambe/ambeVoice.h"
#include "dmr/call/dmr_call.h"
#if CONFIG_P25 && !defined(HAL_VOCODER_LEND_BYTES)
#include "radio/radio_p25.h"
#endif

// dPMR's TCH is the 72-bit voice frame the DMR side's voice path takes and gives (dpmr.md 2)
_Static_assert(DPMR_AMBE_BYTES == DMR_AMBE_FRAME_BYTES, "one voice frame size");

#if CONFIG_P25 && !defined(HAL_VOCODER_LEND_BYTES)
// P25's memory is idle while dPMR runs (one air mode at a time, NXDN's included): no copy of our own
#define DPMR_MEM_SHARED	1
#else
#define DPMR_MEM_SHARED	0
static dpmr_engine_t own_mem;
#endif

// Header frames ahead of the voice: an extended wake-up header and the normal one. dPMR446 radios send a run of them
// (the one recorded off air sent nine, dpmr.md 10) so a receiver that was asleep or scanning still finds a header
// before the voice; one more costs 80 ms of key-up and is the least that leaves a receiver a second chance
#define RADIO_DPMR_HEADERS	2u

static const radio_dpmr_settings_t defaults = {
	.called = DPMR_ID_ALL, .own = 0, .cc = DPMR_CP_CC_AUTO, .rx_mine = false,
};
static radio_dpmr_settings_t set;
static dpmr_engine_t *eng;		// while running
static radio_dpmr_stats_t stats;
static dpmr_call_stats_t call_base;	// the call layer's counters already counted (they restart with the engine)
static air_session_stats_t air_base;
static uint32_t rx_calls_base, tx_calls_base, filler_base, rejects_base;

void radio_dpmr_init(void)
{
	set = defaults;
	eng = NULL;
}

void radio_dpmr_settings(radio_dpmr_settings_t *out)
{
	*out = set;
}

void radio_dpmr_set_settings(const radio_dpmr_settings_t *s)
{
	set = *s;
	set.called &= RADIO_DPMR_ID_MAX;
	if (set.called == 0)
		set.called = DPMR_ID_ALL;	// common ID 0 is reserved: the all call, as a zeroed channel block
	set.own &= RADIO_DPMR_ID_MAX;
	if (set.own == RADIO_DPMR_ID_MAX)
		set.own = 0;			// erased, as the radio-wide record reads it: none
	if (set.cc > RADIO_DPMR_CC_MAX)
		set.cc = DPMR_CP_CC_AUTO;
}

uint32_t radio_dpmr_fit(void)
{
	return board.air != NULL ? dpmr_engine_fit(board.air) : 0;
}

bool radio_dpmr_fits(void)
{
	return (radio_dpmr_fit() & (AIR_FIT_RX | AIR_FIT_TX)) != 0;
}

bool radio_dpmr_take(channel_t *ch)
{
	if (ch->kind != CODEPLUG_KIND_DPMR || !radio_dpmr_fits() || radio_listen_band(ch->rx_hz))
		return false;
	ch->mode = RADIO_MODE_DPMR;
	ch->bw = RADIO_BW_12K5;		// the radios' narrowest: dPMR's 6.25 kHz channel has no setting of its own
	return true;
}

bool radio_dpmr_receives(void)
{
	return (radio_dpmr_fit() & AIR_FIT_RX) != 0;	// one profile: no channel record to read
}

// The engine's configuration for channel ch at number (0: the VFO): the one place dPMR parameters come from. A live
// stored dPMR channel's tag gives them (dpmr.md 6), the radio-wide record our own ID; else the settings. With no own ID
// the called ID is ours too, as an initial-services radio on a common ID sends (TS 102 490 8.1.1)
static void params(const channel_t *ch, uint16_t number, dpmr_engine_cfg_t *c)
{
	dpmr_chan_t dc;
	uint32_t own;

	memset(c, 0, sizeof(*c));
	c->silence = dmr_ambe_silence;
	c->headers = RADIO_DPMR_HEADERS;
	if (number != 0 && dpmr_chan_load(number, ch, &dc)) {
		own = dpmr_own_load();
		if (own == DPMR_CP_ID_NONE)
			own = 0;
	} else {
		dc.called = set.called;
		dc.cc = set.cc;
		dc.flags = set.rx_mine ? DPMR_CP_RX_MINE : 0u;
		own = set.own;
	}
	c->called = dc.called;
	c->own = own != 0 ? own : dc.called;
	c->cc = dpmr_chan_cc(&dc, ch->tx_hz);
	c->rx_mine = (dc.flags & DPMR_CP_RX_MINE) != 0;
}

// ── Voice: the DMR side's voice path (ambeVoice.h), its sink on receive and its source on transmit. The hooks run in
// the provider's task, which is the only one feeding that path while dPMR runs (the DMR stack is stopped); the audio
// stream is started and stopped by the radio task (begin, end) ──
static void v_start(const dpmr_call_t *c, void *ctx)
{
	const dmr_voice_sink_t *s = ambeVoiceSink();

	(void)c;
	(void)ctx;
	s->begin(NULL, s->ctx);
}

static void v_voice(const uint8_t ambe[DPMR_AMBE_BYTES], void *ctx)
{
	const dmr_voice_sink_t *s = ambeVoiceSink();

	(void)ctx;
	s->frame(ambe, s->ctx);
}

static void v_end(void *ctx)
{
	const dmr_voice_sink_t *s = ambeVoiceSink();

	(void)ctx;
	s->end(s->ctx);
}

static bool v_tx(uint8_t ambe[DPMR_AMBE_BYTES], void *ctx)
{
	const dmr_voice_source_t *s = ambeVoiceSource();

	(void)ctx;
	return s->next(ambe, s->ctx);
}

static const dpmr_engine_hooks_t voice_hooks = { v_start, NULL, v_voice, v_end, v_tx };

// The encoder runs from the headers on, so the first voice frames are ready when the engine asks
static void voice_tx(bool on)
{
	const dmr_voice_source_t *s = ambeVoiceSource();

	if (on)
		s->begin(s->ctx);
	else
		s->end(s->ctx);
}

static dpmr_engine_t *mem_get(void)
{
#if DPMR_MEM_SHARED
	size_t len = 0;
	void *m = radio_p25_idle_mem(&len);

	if (m == NULL || len < sizeof(dpmr_engine_t) || ((uintptr_t)m & 7u) != 0)
		return NULL;
	return m;
#else
	return &own_mem;
#endif
}

bool radio_dpmr_begin(const channel_t *ch, uint16_t number)
{
	dpmr_engine_cfg_t cfg;

	radio_dpmr_end();
	stats.begins++;
	params(ch, number, &cfg);
	eng = mem_get();
	if (eng == NULL || board.air == NULL || dpmr_engine_fit(board.air) == 0) {
		eng = NULL;
		stats.begin_fails++;
		return false;
	}
	ambeVoiceStart();	// before the engine: its first call may come at once
	if (!dpmr_engine_begin(eng, board.air, &cfg, &voice_hooks, NULL)) {
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

void radio_dpmr_end(void)
{
	if (eng == NULL)
		return;
	dpmr_engine_end(eng);	// the provider closed: no hook runs after this
	ambeVoiceStop();
	eng = NULL;
}

bool radio_dpmr_running(void)
{
	return eng != NULL;
}

void radio_dpmr_retuned(void)
{
	if (eng != NULL)
		dpmr_engine_retuned(eng);
}

void radio_dpmr_apply(const channel_t *ch, uint16_t number)
{
	dpmr_engine_cfg_t cfg;

	if (eng == NULL || dpmr_engine_tx_state(eng) != AIR_TX_IDLE)
		return;
	params(ch, number, &cfg);
	dpmr_engine_set(eng, &cfg);
}

// The engine's counters into ours; they restart with each engine, so ours add what moved since the last look
static void count(const dpmr_engine_view_t *ev)
{
	const dpmr_call_stats_t *c = &eng->rx.stats;
	const air_session_stats_t *a = &eng->session.stats;

	stats.rx_calls += ev->rx_calls - rx_calls_base;
	stats.tx_calls += ev->tx_calls - tx_calls_base;
	stats.tx_filler += ev->tx_filler - filler_base;
	rx_calls_base = ev->rx_calls;
	tx_calls_base = ev->tx_calls;
	filler_base = ev->tx_filler;
	stats.headers += c->headers - call_base.headers;
	stats.frames += c->frames - call_base.frames;
	stats.ends += c->ends - call_base.ends;
	stats.hi_fail += c->hi_fail - call_base.hi_fail;
	stats.cch_fail += c->cch_fail - call_base.cch_fail;
	stats.end_fail += c->end_fail - call_base.end_fail;
	stats.voice += c->voice - call_base.voice;
	stats.voice_held += c->voice_held - call_base.voice_held;
	stats.ham_fixed += c->ham_fixed - call_base.ham_fixed;
	stats.ham_bad += c->ham_bad - call_base.ham_bad;
	call_base = *c;
	stats.rejects += eng->rx.rx.rejects - rejects_base;
	rejects_base = eng->rx.rx.rejects;
	stats.rx_chunks += a->rx_chunks - air_base.rx_chunks;
	stats.tx_underruns += a->tx_underruns - air_base.tx_underruns;
	stats.tx_syms += a->tx_syms - air_base.tx_syms;
	air_base = *a;
}

// The engine's configuration as settings: what the view and the debug link show
static void cfg_settings(const dpmr_engine_cfg_t *c, radio_dpmr_settings_t *out)
{
	out->called = c->called;
	out->own = c->own;
	out->cc = dpmr_cc_index(c->cc);
	out->rx_mine = c->rx_mine;
}

bool radio_dpmr_in_force(radio_dpmr_settings_t *out)
{
	dpmr_engine_t *e = eng;

	if (e == NULL)
		return false;
	cfg_settings(&e->cfg, out);
	return true;
}

// What the screen shows of the parameters: who PTT calls, as whom, with which code. Those in force while the engine runs
// (a stored channel's own), else the settings, the automatic code and own ID left as the settings say
static void settings_view(radio_dpmr_view_t *n)
{
	n->fit = (uint16_t)radio_dpmr_fit();
	if (eng != NULL) {
		n->talk = eng->cfg.called;
		n->tx_own = eng->cfg.own;
		n->tx_cc = eng->cfg.cc;
		if (eng->cfg.rx_mine)
			n->flags |= RADIO_DPMR_MINE;
		return;
	}
	n->talk = set.called;
	n->tx_own = set.own != 0 ? set.own : set.called;
	dpmr_chan_t dc = { set.called, set.cc, 0 };
	n->tx_cc = set.cc != DPMR_CP_CC_AUTO ? dpmr_chan_cc(&dc, 0) : 0u;	// automatic: the frequency's, not known here
	if (set.rx_mine)
		n->flags |= RADIO_DPMR_MINE;
}

bool radio_dpmr_poll(radio_dpmr_view_t *v)
{
	dpmr_engine_view_t ev;
	radio_dpmr_view_t n;

	if (eng == NULL) {
		n = *v;		// the last call heard stays
		n.flags &= RADIO_DPMR_ENDED | RADIO_DPMR_KNOWN;
		n.tx = AIR_TX_IDLE;
	} else {
		dpmr_engine_view(eng, &ev);
		count(&ev);
		memset(&n, 0, sizeof(n));
		n.flags = RADIO_DPMR_ON;
		if (ev.rx_active) {
			n.flags |= RADIO_DPMR_RX;
			if (ev.rx_played)
				n.flags |= RADIO_DPMR_PLAYED;
		}
		if (ev.rx_calls != 0 && (ev.rx.have & DPMR_HAVE_HEADER) == 0)
			n.flags |= RADIO_DPMR_LATE;
		if (ev.rx.ended)
			n.flags |= RADIO_DPMR_ENDED;
		if (ev.rx.have & DPMR_HAVE_CALLED)
			n.flags |= RADIO_DPMR_KNOWN;
		n.tx = ev.tx;
		n.have = ev.rx.have;
		n.called = (ev.rx.have & DPMR_HAVE_CALLED) ? ev.rx.called : 0u;
		n.own = (ev.rx.have & DPMR_HAVE_OWN) ? ev.rx.own : 0u;
		n.cc = (ev.rx.have & DPMR_HAVE_CC) ? ev.rx.cc : 0u;
	}
	settings_view(&n);
	bool changed = memcmp(&n, v, sizeof(n)) != 0;
	*v = n;
	return changed;
}

static bool dpmr_no_tx(void)
{
	return eng != NULL && (eng->session.fit & AIR_FIT_TX) == 0;
}

static bool dpmr_tx_prepare(void)
{
	if (eng == NULL || !dpmr_engine_tx_prepare(eng))
		return false;
	voice_tx(true);
	return true;
}

static bool dpmr_tx_go(void)
{
	return eng != NULL && dpmr_engine_tx_go(eng);
}

static void dpmr_tx_stop(void)
{
	if (eng != NULL) {
		voice_tx(false);	// the engine fills the superframe on air with silence
		dpmr_engine_tx_stop(eng);
	}
}

static void dpmr_tx_abort(void)
{
	if (eng != NULL) {
		voice_tx(false);
		dpmr_engine_tx_abort(eng);
	}
}

static uint8_t dpmr_tx_state(void)
{
	return eng != NULL ? (uint8_t)dpmr_engine_tx_state(eng) : (uint8_t)AIR_TX_IDLE;
}

static void dpmr_tx_clear(void)
{
	if (eng != NULL) {
		voice_tx(false);
		dpmr_engine_tx_clear(eng);
	}
}

const radio_air_ops_t radio_dpmr_air = {
	radio_dpmr_running, dpmr_no_tx, dpmr_tx_prepare, dpmr_tx_go, dpmr_tx_stop, dpmr_tx_abort, dpmr_tx_state,
	dpmr_tx_clear,
};

void radio_dpmr_stats(radio_dpmr_stats_t *out)
{
	*out = stats;
}

void radio_dpmr_reset_stats(void)
{
	memset(&stats, 0, sizeof(stats));
}

#endif	// CONFIG_DPMR
