// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// System Fusion DN on a channel, the radio core's side (radio_ysf.h)
#include "config.h"
#if CONFIG_YSF	// the whole file
#include <stddef.h>
#include <string.h>
#include "board.h"
#include "radio/radio_ysf.h"
#include "radio/radio_bands.h"
#include "ysf/ysf_engine.h"
#include "ysf/ysf_codeplug.h"
#include "ysf/ysf_id.h"
#include "codeplug/codeplug_modes.h"
#include "dmr/ambe/ambeVoice.h"
#include "dmr/call/dmr_call.h"

// The voice frame YSF hands on is the one the DMR side's voice path takes and gives (ysf.h)
_Static_assert(YSF_VOICE_BYTES == DMR_AMBE_FRAME_BYTES, "one voice frame size");
_Static_assert(RADIO_YSF_RID_LEN == YSF_RID_LEN && RADIO_YSF_CALL_LEN == YSF_CS_LEN, "one field size");


static radio_ysf_settings_t set;
static char rid[RADIO_YSF_RID_LEN + 1];	// this radio's RID ("" until worked out) and where it came from
static bool rid_weak;

static void rid_reset(void)
{
	rid[0] = '\0';
}
static ysf_engine_t *eng;		// while running
static radio_ysf_stats_t stats;
static ysf_call_stats_t call_base;
static air_session_stats_t air_base;
static uint32_t rx_calls_base, tx_calls_base, filler_base;

void radio_ysf_init(void)
{
	memset(&set, 0, sizeof set);
	eng = NULL;
	rid_reset();		// worked out at first use: the codeplug may not be read yet
}

void radio_ysf_settings(radio_ysf_settings_t *out)
{
	*out = set;
}

void radio_ysf_set_settings(const radio_ysf_settings_t *s)
{
	set = *s;
	if (set.tx_dgid > RADIO_YSF_DGID_MAX)
		set.tx_dgid = 0;
	if (set.rx_dgid > RADIO_YSF_DGID_MAX)
		set.rx_dgid = 0;
}

uint32_t radio_ysf_fit(void)
{
	return board.air != NULL ? ysf_engine_fit(board.air) : 0;
}

bool radio_ysf_fits(void)
{
	return (radio_ysf_fit() & (AIR_FIT_RX | AIR_FIT_TX)) != 0;
}

bool radio_ysf_take(channel_t *ch)
{
	if (ch->kind != CODEPLUG_KIND_YSF || !radio_ysf_fits() || radio_listen_band(ch->rx_hz))
		return false;
	ch->mode = RADIO_MODE_YSF;
	ch->bw = RADIO_BW_12K5;
	return true;
}

// ── Who we are on air ──
// The RID: from the MCU's unique ID, the one source that differs from radio to radio by construction. Only a board
// without one falls back to the DMR ID and callsign, which a copied codeplug would share: the view says so
// (RADIO_YSF_RID_WEAK) and the screen that shows the RID marks it. Worked out at first use and, for the fallback, at
// each engine start (it follows a codeplug change); read by any task

static void rid_update(void)
{
	hal_chip_t chip;
	char r[RADIO_YSF_RID_LEN + 1];
	bool have = board.system != NULL && board.system->chip != NULL && board.system->chip(&chip) &&
		    ysf_uid_usable(chip.uid);

	if (have) {
		ysf_rid_from_uid(chip.uid, r);
	} else {
		char name[CODEPLUG_NAME_LEN + 1];

		codeplug_radio_name(name);
		ysf_rid_fallback(codeplug_dmr_id(), name, r);
	}
	rid_weak = !have;
	memcpy(rid, r, sizeof rid);
}

void radio_ysf_rid(char out[RADIO_YSF_RID_LEN + 1], bool *weak)
{
	if (rid[0] == '\0')
		rid_update();
	memcpy(out, rid, sizeof rid);
	if (weak)
		*weak = rid_weak;
}

void radio_ysf_callsign(const char *radio_name, char out[RADIO_YSF_CALL_LEN + 1])
{
	ysf_radio_t r;

	ysf_radio_load(&r);
	if (r.callsign[0] != '\0') {
		memcpy(out, r.callsign, sizeof r.callsign);
		return;
	}
	ysf_callsign_from_name(radio_name, out);
}

// The engine's configuration for channel ch at number (0: the VFO): a live stored YSF channel's tag gives the
// DG-IDs, else the settings
static void params(const channel_t *ch, uint16_t number, const char *radio_name, ysf_engine_cfg_t *c)
{
	ysf_chan_t yc;
	bool weak;

	memset(c, 0, sizeof *c);
	c->silence = dmr_ambe_silence;
	if (number != 0 && ysf_chan_load(number, ch, &yc)) {
		c->rx_dgid = yc.rx_dgid;
		c->tx.dgid = yc.tx_dgid;
	} else {
		c->rx_dgid = set.rx_dgid;
		c->tx.dgid = set.tx_dgid;
	}
	c->tx.mr = YSF_MR_DIRECT;
	radio_ysf_callsign(radio_name, c->tx.callsign);
	radio_ysf_rid(c->tx.radio_id, &weak);
	c->tx_inhibit = c->tx.callsign[0] == '\0';
}

// ── Voice: the DMR side's voice path (ambeVoice.h), as radio_nxdn.c uses it ──
static void v_start(const ysf_call_t *c, void *ctx)
{
	const dmr_voice_sink_t *s = ambeVoiceSink();

	(void)c;
	(void)ctx;
	s->begin(NULL, s->ctx);
}

static void v_voice(const uint8_t v[YSF_VOICE_BYTES], void *ctx)
{
	const dmr_voice_sink_t *s = ambeVoiceSink();

	(void)ctx;
	s->frame(v, s->ctx);
}

static void v_end(void *ctx)
{
	const dmr_voice_sink_t *s = ambeVoiceSink();

	(void)ctx;
	s->end(s->ctx);
}

static bool v_tx(uint8_t v[YSF_VOICE_BYTES], void *ctx)
{
	const dmr_voice_source_t *s = ambeVoiceSource();

	(void)ctx;
	return s->next(v, s->ctx);
}

static const ysf_engine_hooks_t voice_hooks = { v_start, NULL, v_voice, v_end, v_tx };

static void voice_tx(bool on)
{
	const dmr_voice_source_t *s = ambeVoiceSource();

	if (on)
		s->begin(s->ctx);
	else
		s->end(s->ctx);
}

static ysf_engine_t *mem_get(void)
{
	size_t len = 0;
	void *m = radio_air_engine_mem(&len);

	if (m == NULL || len < sizeof(ysf_engine_t) || ((uintptr_t)m & 7u) != 0)
		return NULL;
	return m;
}

bool radio_ysf_begin(const channel_t *ch, uint16_t number, const char *radio_name)
{
	ysf_engine_cfg_t cfg;

	radio_ysf_end();
	stats.begins++;
	if (rid_weak)
		rid_update();	// the fallback follows the codeplug
	params(ch, number, radio_name, &cfg);
	eng = mem_get();
	if (eng == NULL || board.air == NULL || ysf_engine_fit(board.air) == 0) {
		eng = NULL;
		stats.begin_fails++;
		return false;
	}
	ambeVoiceStart();
	if (!ysf_engine_begin(eng, board.air, &cfg, &voice_hooks, NULL)) {
		ambeVoiceStop();
		eng = NULL;
		stats.begin_fails++;
		return false;
	}
	memset(&call_base, 0, sizeof call_base);
	memset(&air_base, 0, sizeof air_base);
	rx_calls_base = tx_calls_base = filler_base = 0;
	return true;
}

void radio_ysf_end(void)
{
	if (eng == NULL)
		return;
	ysf_engine_end(eng);
	ambeVoiceStop();
	eng = NULL;
}

bool radio_ysf_running(void)
{
	return eng != NULL;
}

void radio_ysf_retuned(void)
{
	if (eng != NULL)
		ysf_engine_retuned(eng);
}

void radio_ysf_apply(const channel_t *ch, uint16_t number, const char *radio_name)
{
	ysf_engine_cfg_t cfg;

	if (eng == NULL || ysf_engine_tx_state(eng) != AIR_TX_IDLE)
		return;
	params(ch, number, radio_name, &cfg);
	ysf_engine_set(eng, &cfg);
}

static void count(const ysf_engine_view_t *ev)
{
	const ysf_call_stats_t *c = &eng->rx.stats;
	const air_session_stats_t *a = &eng->session.stats;

	stats.rx_calls += ev->rx_calls - rx_calls_base;
	stats.tx_calls += ev->tx_calls - tx_calls_base;
	stats.tx_filler += ev->tx_filler - filler_base;
	rx_calls_base = ev->rx_calls;
	tx_calls_base = ev->tx_calls;
	filler_base = ev->tx_filler;
	stats.frames += c->frames - call_base.frames;
	stats.fich_fail += c->fich_fail - call_base.fich_fail;
	stats.csd_fail += c->csd_fail - call_base.csd_fail;
	stats.dch_fail += c->dch_fail - call_base.dch_fail;
	stats.voice += c->voice - call_base.voice;
	stats.voice_held += c->voice_held - call_base.voice_held;
	stats.fec_bits += c->fec_bits - call_base.fec_bits;
	stats.vch_fixed += c->vch_fixed - call_base.vch_fixed;
	call_base = *c;
	stats.rx_chunks += a->rx_chunks - air_base.rx_chunks;
	stats.tx_underruns += a->tx_underruns - air_base.tx_underruns;
	stats.tx_syms += a->tx_syms - air_base.tx_syms;
	air_base = *a;
}

bool radio_ysf_poll(radio_ysf_view_t *v)
{
	ysf_engine_view_t ev;
	radio_ysf_view_t n;
	bool weak;
	char me[RADIO_YSF_RID_LEN + 1];

	if (eng == NULL) {
		n = *v;		// the last call heard stays
		n.flags = 0;
		n.tx = AIR_TX_IDLE;
		n.rx_dgid = set.rx_dgid;
		n.tx_dgid = set.tx_dgid;
	} else {
		ysf_engine_view(eng, &ev);
		count(&ev);
		memset(&n, 0, sizeof n);
		n.flags = RADIO_YSF_ON;
		if (ev.rx_active) {
			n.flags |= RADIO_YSF_RX;
			if (ev.rx_played)
				n.flags |= RADIO_YSF_PLAYED;
			if (ev.rx.late)
				n.flags |= RADIO_YSF_LATE;
			if (ev.rx.dt != YSF_DT_VD2)
				n.flags |= RADIO_YSF_NOT_DN;
		}
		if (eng->cfg.tx_inhibit)
			n.flags |= RADIO_YSF_NO_CALL;
		n.tx = ev.tx;
		n.dgid = ev.rx.dgid;
		n.rx_dgid = eng->cfg.rx_dgid;
		n.tx_dgid = eng->cfg.tx.dgid;
		memcpy(n.src, ev.rx.src, sizeof n.src);
		memcpy(n.src_rid, ev.rx.src_id, sizeof n.src_rid);
	}
	radio_ysf_rid(me, &weak);
	if (weak)
		n.flags |= RADIO_YSF_RID_WEAK;
	n.fit = (uint16_t)radio_ysf_fit();
	bool changed = memcmp(&n, v, sizeof n) != 0;
	*v = n;
	return changed;
}

static bool ysf_no_tx(void)
{
	return eng != NULL && (eng->session.fit & AIR_FIT_TX) == 0;
}

static bool ysf_tx_prepare(void)
{
	if (eng == NULL || !ysf_engine_tx_prepare(eng))
		return false;
	voice_tx(true);
	return true;
}

static bool ysf_tx_go(void)
{
	return eng != NULL && ysf_engine_tx_go(eng);
}

static void ysf_tx_stop(void)
{
	if (eng != NULL) {
		voice_tx(false);
		ysf_engine_tx_stop(eng);
	}
}

static void ysf_tx_abort(void)
{
	if (eng != NULL) {
		voice_tx(false);
		ysf_engine_tx_abort(eng);
	}
}

static uint8_t ysf_tx_state(void)
{
	return eng != NULL ? (uint8_t)ysf_engine_tx_state(eng) : (uint8_t)AIR_TX_IDLE;
}

static void ysf_tx_clear(void)
{
	if (eng != NULL) {
		voice_tx(false);
		ysf_engine_tx_clear(eng);
	}
}

const radio_air_ops_t radio_ysf_air = {
	radio_ysf_running, ysf_no_tx, ysf_tx_prepare, ysf_tx_go, ysf_tx_stop, ysf_tx_abort, ysf_tx_state, ysf_tx_clear,
};

void radio_ysf_stats(radio_ysf_stats_t *out)
{
	*out = stats;
}

void radio_ysf_reset_stats(void)
{
	memset(&stats, 0, sizeof stats);
}

#endif	// CONFIG_YSF
