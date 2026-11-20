// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// D-STAR DV on a channel, the radio core's side (radio_dstar.h)
#include "config.h"
#if CONFIG_DSTAR	// the whole file
#include <stddef.h>
#include <string.h>
#include "board.h"
#include "radio/radio_dstar.h"
#include "dstar/dstar_engine.h"
#include "dstar/dstarVoice.h"
#include "mbe/ambe.h"
#if CONFIG_P25 && !defined(HAL_VOCODER_LEND_BYTES)
#include "radio/radio_p25.h"
#endif

_Static_assert(RADIO_DSTAR_CALL == DSTAR_CALL_LEN && RADIO_DSTAR_SUFFIX == DSTAR_SUFFIX_LEN &&
	       RADIO_DSTAR_MSG == DSTAR_MSG_LEN, "one size of each field");
_Static_assert(DSTAR_VOICE_BYTES == AMBE_BYTES, "one voice frame size");

// What runs while D-STAR is on: the engine, and the voice path's queues, decoder and encoder
typedef struct {
	dstar_engine_t eng;
	dstar_voice_mem_t voice;
} dstar_mem_t;

#ifdef HAL_VOCODER_LEND_BYTES
// The platform's DMR vocoder lends its memory while D-STAR runs: D-STAR's voice is not the vocoder's
_Static_assert(sizeof(dstar_mem_t) <= HAL_VOCODER_LEND_BYTES, "D-STAR does not fit the memory the vocoder lends");
#elif !CONFIG_P25
static dstar_mem_t own_mem;	// else P25's memory, idle while D-STAR runs (one air mode at a time)
#endif

static radio_dstar_settings_t set;
static char own_call[RADIO_DSTAR_CALL + 1];	// the radio's callsign, from its name
static dstar_mem_t *mem;			// while running
static radio_dstar_stats_t stats;
static dstar_call_stats_t call_base;		// the call layer's counters already counted (they restart with the engine)
static air_session_stats_t air_base;
static uint32_t rx_calls_base, tx_calls_base, filler_base;

void radio_dstar_init(const hal_audio_t *audio)
{
	memset(&set, 0, sizeof set);
	mem = NULL;
	dstarVoiceInit(audio);
}

void radio_dstar_settings(radio_dstar_settings_t *out)
{
	*out = set;
}

static void text_copy(char *dst, const char *src, size_t cap)
{
	size_t i = 0;

	for (; src[i] != '\0' && i + 1 < cap; i++)
		dst[i] = src[i];
	dst[i] = '\0';
}

void radio_dstar_set_settings(const radio_dstar_settings_t *s)
{
	text_copy(set.my, s->my, sizeof set.my);
	text_copy(set.sfx, s->sfx, sizeof set.sfx);
	text_copy(set.ur, s->ur, sizeof set.ur);
	text_copy(set.rpt1, s->rpt1, sizeof set.rpt1);
	text_copy(set.rpt2, s->rpt2, sizeof set.rpt2);
	text_copy(set.msg, s->msg, sizeof set.msg);
}

uint32_t radio_dstar_fit(void)
{
	return board.air != NULL ? dstar_engine_fit(board.air) : 0;
}

bool radio_dstar_fits(void)
{
	return (radio_dstar_fit() & (AIR_FIT_RX | AIR_FIT_TX)) != 0;
}

// The engine's configuration: the one place what we send comes from (docs/spec/dstar.md 6.2). Simplex: both repeater
// fields DIRECT. Through a repeater: RPT1 is the repeater with its module, RPT2 its gateway, the flag says so
static void params(dstar_engine_cfg_t *c)
{
	dstar_header_t *h = &c->tx_hdr;
	const char *my = set.my[0] != '\0' ? set.my : own_call;

	memset(c, 0, sizeof(*c));
	c->silence = ambe_silence;
	dstar_call_set(h->my, DSTAR_CALL_LEN, my);
	dstar_call_set(h->sfx, DSTAR_SUFFIX_LEN, set.sfx);
	dstar_call_set(h->ur, DSTAR_CALL_LEN, set.ur[0] != '\0' ? set.ur : "CQCQCQ");
	if (set.rpt1[0] == '\0') {
		dstar_call_set(h->rpt1, DSTAR_CALL_LEN, "DIRECT");
		dstar_call_set(h->rpt2, DSTAR_CALL_LEN, "DIRECT");
	} else {
		h->flag[0] = DSTAR_F1_REPEATER;
		dstar_call_set(h->rpt1, DSTAR_CALL_LEN, set.rpt1);
		if (set.rpt2[0] != '\0') {
			dstar_call_set(h->rpt2, DSTAR_CALL_LEN, set.rpt2);
		} else {
			memcpy(h->rpt2, h->rpt1, DSTAR_CALL_LEN);
			h->rpt2[DSTAR_CALL_LEN - 1] = 'G';
		}
	}
	text_copy(c->tx_msg, set.msg, sizeof c->tx_msg);
	// Nothing goes on air without our callsign
	c->tx_inhibit = my[0] == '\0';
}

static dstar_mem_t *mem_get(void)
{
#ifdef HAL_VOCODER_LEND_BYTES
	size_t len = 0;
	void *m = board.vocoder != NULL && board.vocoder->lend != NULL ? board.vocoder->lend(&len) : NULL;

	if (m == NULL || len < sizeof(dstar_mem_t) || ((uintptr_t)m & 7u) != 0)
		return NULL;
	return m;
#elif CONFIG_P25
	size_t len = 0;
	void *m = radio_p25_idle_mem(&len);

	if (m == NULL || len < sizeof(dstar_mem_t) || ((uintptr_t)m & 7u) != 0)
		return NULL;
	return m;
#else
	return &own_mem;
#endif
}

bool radio_dstar_begin(const channel_t *ch, const char *callsign)
{
	dstar_engine_cfg_t cfg;

	(void)ch;
	radio_dstar_end();
	stats.begins++;
	text_copy(own_call, callsign != NULL ? callsign : "", sizeof own_call);
	params(&cfg);
	mem = mem_get();
	if (mem == NULL || board.air == NULL || dstar_engine_fit(board.air) == 0) {
		mem = NULL;
		stats.begin_fails++;
		return false;
	}
	dstarVoiceBegin(&mem->voice);	// before the engine: its first call may come at once
	if (!dstar_engine_begin(&mem->eng, board.air, &cfg, dstarVoiceHooks(), NULL)) {
		dstarVoiceEnd();
		mem = NULL;
		stats.begin_fails++;
		return false;
	}
	memset(&call_base, 0, sizeof(call_base));
	memset(&air_base, 0, sizeof(air_base));
	rx_calls_base = tx_calls_base = filler_base = 0;
	return true;
}

void radio_dstar_end(void)
{
	if (mem == NULL)
		return;
	dstar_engine_end(&mem->eng);	// the provider closed: no hook runs after this
	dstarVoiceEnd();
	mem = NULL;			// the memory goes back (a lending vocoder rebuilds its state at its next call)
}

bool radio_dstar_running(void)
{
	return mem != NULL;
}

void radio_dstar_retuned(void)
{
	if (mem != NULL)
		dstar_engine_retuned(&mem->eng);
}

void radio_dstar_apply(const char *callsign)
{
	dstar_engine_cfg_t cfg;

	if (callsign != NULL)
		text_copy(own_call, callsign, sizeof own_call);
	if (mem == NULL || dstar_engine_tx_state(&mem->eng) != AIR_TX_IDLE)
		return;
	params(&cfg);
	dstar_engine_set(&mem->eng, &cfg);
}

// The engine's counters into ours; they restart with each engine, so ours add what moved since the last look
static void count(const dstar_engine_view_t *ev)
{
	const dstar_call_stats_t *c = &mem->eng.rx.stats;
	const air_session_stats_t *a = &mem->eng.session.stats;

	stats.rx_calls += ev->rx_calls - rx_calls_base;
	stats.tx_calls += ev->tx_calls - tx_calls_base;
	stats.tx_filler += ev->tx_filler - filler_base;
	rx_calls_base = ev->rx_calls;
	tx_calls_base = ev->tx_calls;
	filler_base = ev->tx_filler;
#define ADD(f) stats.f += c->f - call_base.f
	ADD(headers);
	ADD(header_fail);
	ADD(late);
	ADD(frames);
	ADD(voice);
	ADD(syncs);
	ADD(ends);
	ADD(lost);
	ADD(slow_msgs);
	ADD(slow_headers);
	ADD(slow_text);
#undef ADD
	call_base = *c;
	stats.rx_chunks += a->rx_chunks - air_base.rx_chunks;
	stats.tx_underruns += a->tx_underruns - air_base.tx_underruns;
	stats.tx_syms += a->tx_syms - air_base.tx_syms;
	air_base = *a;
}

// A header field as text: the trailing spaces dropped
static void field(char *out, const char *f, uint8_t len)
{
	while (len > 0 && (f[len - 1] == ' ' || f[len - 1] == '\0'))
		len--;
	memcpy(out, f, len);
	out[len] = '\0';
}

bool radio_dstar_poll(radio_dstar_view_t *v)
{
	dstar_engine_view_t ev;
	radio_dstar_view_t n;

	if (mem == NULL) {
		n = *v;		// the last call heard stays
		n.flags = 0;
		n.tx = AIR_TX_IDLE;
	} else {
		dstarVoicePoll();
		dstar_engine_view(&mem->eng, &ev);
		count(&ev);
		memset(&n, 0, sizeof(n));
		n.flags = RADIO_DSTAR_ON;
		if (ev.rx_active)
			n.flags |= RADIO_DSTAR_RX;
		if (ev.rx.late)
			n.flags |= RADIO_DSTAR_LATE;
		if (ev.rx.have_header) {
			n.flags |= RADIO_DSTAR_HDR;
			if (ev.rx.hdr.flag[0] & DSTAR_F1_REPEATER)
				n.flags |= RADIO_DSTAR_RPT;
			if (ev.rx.hdr.flag[0] & DSTAR_F1_EMERGENCY)
				n.flags |= RADIO_DSTAR_EMERG;
			field(n.my, ev.rx.hdr.my, DSTAR_CALL_LEN);
			field(n.sfx, ev.rx.hdr.sfx, DSTAR_SUFFIX_LEN);
			field(n.ur, ev.rx.hdr.ur, DSTAR_CALL_LEN);
			field(n.rpt1, ev.rx.hdr.rpt1, DSTAR_CALL_LEN);
		}
		if (ev.rx.have_msg) {
			n.flags |= RADIO_DSTAR_HAS_MSG;
			field(n.msg, ev.rx.msg, DSTAR_MSG_LEN);
		}
		n.tx = ev.tx;
	}
	n.fit = (uint16_t)radio_dstar_fit();
	{
		dstar_engine_cfg_t c;

		params(&c);
		field(n.talk, c.tx_hdr.ur, DSTAR_CALL_LEN);
		if (set.rpt1[0] != '\0')
			field(n.talk_rpt, c.tx_hdr.rpt1, DSTAR_CALL_LEN);
		else
			n.talk_rpt[0] = '\0';
	}
	bool changed = memcmp(&n, v, sizeof(n)) != 0;
	*v = n;
	return changed;
}

bool radio_dstar_tx_header(char my[RADIO_DSTAR_CALL + 1], char ur[RADIO_DSTAR_CALL + 1], char rpt1[RADIO_DSTAR_CALL + 1],
			   char rpt2[RADIO_DSTAR_CALL + 1])
{
	dstar_engine_cfg_t c;

	params(&c);
	field(my, c.tx_hdr.my, DSTAR_CALL_LEN);
	field(ur, c.tx_hdr.ur, DSTAR_CALL_LEN);
	field(rpt1, c.tx_hdr.rpt1, DSTAR_CALL_LEN);
	field(rpt2, c.tx_hdr.rpt2, DSTAR_CALL_LEN);
	return mem != NULL;
}

static bool dstar_no_tx(void)
{
	return mem != NULL && (mem->eng.session.fit & AIR_FIT_TX) == 0;
}

static bool dstar_tx_prepare(void)
{
	if (mem == NULL || !dstar_engine_tx_prepare(&mem->eng))
		return false;
	dstarVoiceTx(true);	// the encoder runs from the header on, so the first voice frames are ready when asked
	return true;
}

static bool dstar_tx_go(void)
{
	return mem != NULL && dstar_engine_tx_go(&mem->eng);
}

static void dstar_tx_stop(void)
{
	if (mem != NULL) {
		dstarVoiceTx(false);
		dstar_engine_tx_stop(&mem->eng);
	}
}

static void dstar_tx_abort(void)
{
	if (mem != NULL) {
		dstarVoiceTx(false);
		dstar_engine_tx_abort(&mem->eng);
	}
}

static uint8_t dstar_tx_state(void)
{
	return mem != NULL ? (uint8_t)dstar_engine_tx_state(&mem->eng) : (uint8_t)AIR_TX_IDLE;
}

static void dstar_tx_clear(void)
{
	if (mem != NULL) {
		dstarVoiceTx(false);
		dstar_engine_tx_clear(&mem->eng);
	}
}

const radio_air_ops_t radio_dstar_air = {
	radio_dstar_running, dstar_no_tx, dstar_tx_prepare, dstar_tx_go, dstar_tx_stop, dstar_tx_abort, dstar_tx_state,
	dstar_tx_clear,
};

void radio_dstar_stats(radio_dstar_stats_t *out)
{
	*out = stats;
}

void radio_dstar_reset_stats(void)
{
	memset(&stats, 0, sizeof(stats));
}

#endif	// CONFIG_DSTAR
