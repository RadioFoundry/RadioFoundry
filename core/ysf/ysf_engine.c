// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// System Fusion DN voice on a channel (ysf_engine.h).
#include "ysf_engine.h"

#include <string.h>

uint32_t ysf_engine_fit(const hal_air_t *air)
{
	return air_session_fit(air, &ysf_air_mode);
}

// ── Receive (provider's task) ──
static void view_begin(ysf_engine_t *e) { e->seq++; }
static void view_end(ysf_engine_t *e) { e->seq++; }

// DN voice whose DG-ID is ours, or every call with ours at 0 (ysf.md 5.1)
static bool wanted(const ysf_engine_t *e, const ysf_call_t *c)
{
	return c->dt == YSF_DT_VD2 && (e->cfg.rx_dgid == YSF_DGID_ALL || c->dgid == e->cfg.rx_dgid);
}

static void rx_fields(ysf_engine_t *e, const ysf_call_t *c, bool start)
{
	bool play = wanted(e, c);

	view_begin(e);
	e->view.rx = *c;
	e->view.rx_active = true;
	e->view.rx_played = play;
	if (start)
		e->view.rx_calls++;
	view_end(e);
	if (play && !e->played) {
		e->played = true;
		if (e->hooks.rx_start)
			e->hooks.rx_start(c, e->ctx);
	} else if (play) {
		if (e->hooks.rx_update)
			e->hooks.rx_update(c, e->ctx);
	} else if (e->played) {
		e->played = false;
		if (e->hooks.rx_end)
			e->hooks.rx_end(e->ctx);
	}
}

static void on_start(const ysf_call_t *c, void *ctx)
{
	ysf_engine_t *e = ctx;

	e->played = false;
	rx_fields(e, c, true);
}

static void on_update(const ysf_call_t *c, void *ctx)
{
	rx_fields(ctx, c, false);
}

static void on_voice(const uint8_t v[YSF_VOICE_BYTES], void *ctx)
{
	ysf_engine_t *e = ctx;

	if (e->recheck) {
		e->recheck = false;
		rx_fields(e, &e->rx.call, false);
	}
	if (e->played && e->hooks.rx_voice)
		e->hooks.rx_voice(v, e->ctx);
}

static void on_end(void *ctx)
{
	ysf_engine_t *e = ctx;

	view_begin(e);
	e->view.rx_active = false;
	e->view.rx_played = false;
	view_end(e);
	if (e->played && e->hooks.rx_end)
		e->hooks.rx_end(e->ctx);
	e->played = false;
}

static const ysf_call_sink_t sink = { on_start, on_update, on_voice, on_end };

// ── Transmit ──
// Symbols to the session while its queue has room. A voice frame the encoder has not made yet is waited for as
// long as the air can spare: once less than the provider's lead plus one more chunk is queued, silence goes instead
static void tx_fill(void *ctx)
{
	ysf_engine_t *e = ctx;
	const hal_air_t *air = e->session.air;
	uint16_t lead = (uint16_t)(((uint32_t)air->tx_lead_ms * ysf_air_mode.profile.sym_rate) / 1000 + air->tx_chunk_syms);

	for (;;) {
		uint16_t first, n = ysf_call_tx_ready(&e->tx, &first), space;

		if (ysf_call_tx_done(&e->tx)) {
			air_session_tx_finish(&e->session);
			return;
		}
		if (e->tx_stop && !e->tx_ended) {
			e->tx_ended = true;
			ysf_call_tx_end(&e->tx, e->cfg.silence);
			continue;
		}
		if (!n) {
			uint8_t v[YSF_VOICE_BYTES];

			if (e->hooks.tx_voice && e->hooks.tx_voice(v, e->ctx) && ysf_call_tx_voice(&e->tx, v))
				continue;
			if (!e->hooks.tx_voice ||
			    (air_session_tx_state(&e->session) != AIR_TX_PRIMING && air_session_tx_queued(&e->session) < lead)) {
				if (e->hooks.tx_voice)
					e->view.tx_filler++;
				if (!ysf_call_tx_voice(&e->tx, e->cfg.silence))
					return;
			} else {
				return;
			}
			continue;
		}
		space = air_session_tx_space(&e->session);
		if (!space)
			return;
		if (n > space)
			n = space;
		if (!air_session_tx(&e->session, ysf_call_tx_syms(&e->tx), first, n))
			return;
		ysf_call_tx_sent(&e->tx, n);
	}
}

static const air_session_ops_t ops = { tx_fill, NULL };

bool ysf_engine_begin(ysf_engine_t *e, const hal_air_t *air, const ysf_engine_cfg_t *cfg,
		      const ysf_engine_hooks_t *hooks, void *ctx)
{
	memset(e, 0, sizeof *e);
	e->cfg = *cfg;
	e->hooks = *hooks;
	e->ctx = ctx;
	ysf_call_rx_init(&e->rx, &sink, e);
	return air_session_open(&e->session, air, &ysf_air_mode, &e->rx.rx, e->frame, e->soft, YSF_FRAME_SYMS, &ops, e);
}

void ysf_engine_end(ysf_engine_t *e)
{
	air_session_close(&e->session);
}

void ysf_engine_retuned(ysf_engine_t *e)
{
	air_session_retuned(&e->session);
}

void ysf_engine_set(ysf_engine_t *e, const ysf_engine_cfg_t *cfg)
{
	e->cfg = *cfg;
	e->recheck = true;
}

bool ysf_engine_tx_prepare(ysf_engine_t *e)
{
	if (e->cfg.tx_inhibit)
		return false;
	if (!air_session_is_open(&e->session) || !(e->session.fit & AIR_FIT_TX) || !e->cfg.silence)
		return false;
	if (air_session_tx_state(&e->session) == AIR_TX_IDLE) {
		e->tx_stop = e->tx_ended = false;
		ysf_call_tx_begin(&e->tx, &e->cfg.tx);
	}
	return air_session_tx_prime(&e->session);
}

bool ysf_engine_tx_go(ysf_engine_t *e)
{
	if (!air_session_tx_go(&e->session))
		return false;
	e->view.tx_calls++;
	return true;
}

void ysf_engine_tx_stop(ysf_engine_t *e)
{
	e->tx_stop = true;
}

void ysf_engine_tx_abort(ysf_engine_t *e)
{
	air_session_tx_abort(&e->session);
}

air_tx_state_t ysf_engine_tx_state(const ysf_engine_t *e)
{
	return air_session_tx_state(&e->session);
}

void ysf_engine_tx_clear(ysf_engine_t *e)
{
	air_session_tx_clear(&e->session);
}

void ysf_engine_view(const ysf_engine_t *e, ysf_engine_view_t *out)
{
	uint8_t seq;

	do {
		seq = e->seq;
		*out = e->view;
	} while ((seq & 1) || seq != e->seq);
	out->tx = (uint8_t)air_session_tx_state(&e->session);
}
