// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// D-STAR DV on a channel (dstar_engine.h).
#include "dstar_engine.h"

#include <string.h>

uint32_t dstar_engine_fit(const hal_air_t *air)
{
	return air_session_fit(air, &dstar_air_mode);
}

// ── Receive (provider's task) ──
static void view_begin(dstar_engine_t *e) { e->seq++; }
static void view_end(dstar_engine_t *e) { e->seq++; }

static void rx_fields(dstar_engine_t *e, const dstar_call_t *c, bool start)
{
	view_begin(e);
	e->view.rx = *c;
	e->view.rx_active = true;
	if (start)
		e->view.rx_calls++;
	view_end(e);
}

// Every call is played: D-STAR has no squelch of its own on a simplex channel, and a repeater's calls are for whoever
// listens to it
static void on_start(const dstar_call_t *c, void *ctx)
{
	dstar_engine_t *e = ctx;

	rx_fields(e, c, true);
	if (e->hooks.rx_start)
		e->hooks.rx_start(c, e->ctx);
}

static void on_update(const dstar_call_t *c, void *ctx)
{
	dstar_engine_t *e = ctx;

	rx_fields(e, c, false);
	if (e->hooks.rx_update)
		e->hooks.rx_update(c, e->ctx);
}

static void on_voice(const uint8_t voice[DSTAR_VOICE_BYTES], void *ctx)
{
	dstar_engine_t *e = ctx;

	if (e->hooks.rx_voice)
		e->hooks.rx_voice(voice, e->ctx);
}

static void on_end(void *ctx)
{
	dstar_engine_t *e = ctx;

	view_begin(e);
	e->view.rx_active = false;
	view_end(e);
	if (e->hooks.rx_end)
		e->hooks.rx_end(e->ctx);
}

static const dstar_call_sink_t sink = { on_start, on_update, on_voice, on_end };

// ── Transmit ──
// Symbols to the session while its queue has room. A voice frame the encoder has not made yet is waited for as long
// as the air can spare: once less than the provider's lead plus one more chunk is queued, silence goes instead
static void tx_fill(void *ctx)
{
	dstar_engine_t *e = ctx;
	const hal_air_t *air = e->session.air;
	uint16_t lead = (uint16_t)(((uint32_t)air->tx_lead_ms * dstar_air_mode.profile.sym_rate) / 1000 + air->tx_chunk_syms);

	for (;;) {
		uint16_t first, n = dstar_call_tx_ready(&e->tx, &first), space;

		if (dstar_call_tx_done(&e->tx)) {
			air_session_tx_finish(&e->session);
			return;
		}
		if (e->tx_stop && !e->tx_ended) {
			e->tx_ended = true;
			dstar_call_tx_end(&e->tx);
			continue;
		}
		if (!n) {
			uint8_t v[DSTAR_VOICE_BYTES];

			if (!dstar_call_tx_wants_voice(&e->tx))
				return;
			if (e->hooks.tx_voice && e->hooks.tx_voice(v, e->ctx) && dstar_call_tx_voice(&e->tx, v))
				continue;
			if (!e->hooks.tx_voice ||
			    (air_session_tx_state(&e->session) != AIR_TX_PRIMING && air_session_tx_queued(&e->session) < lead)) {
				// (before the transmitter is on nothing is lost by waiting: the preamble and header are queued)
				if (e->hooks.tx_voice)
					e->view.tx_filler++;
				if (!dstar_call_tx_voice(&e->tx, e->cfg.silence))
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
		if (!air_session_tx(&e->session, dstar_call_tx_syms(&e->tx), first, n))
			return;
		dstar_call_tx_sent(&e->tx, n);
	}
}

static const air_session_ops_t ops = { tx_fill, NULL };

bool dstar_engine_begin(dstar_engine_t *e, const hal_air_t *air, const dstar_engine_cfg_t *cfg,
			const dstar_engine_hooks_t *hooks, void *ctx)
{
	memset(e, 0, sizeof *e);
	e->cfg = *cfg;
	e->hooks = *hooks;
	e->ctx = ctx;
	dstar_call_rx_init(&e->rx, &sink, e);
	return air_session_open(&e->session, air, &dstar_air_mode, &e->rx, e->frame, e->soft, sizeof e->soft, &ops, e);
}

void dstar_engine_end(dstar_engine_t *e)
{
	air_session_close(&e->session);
}

void dstar_engine_retuned(dstar_engine_t *e)
{
	air_session_retuned(&e->session);
}

void dstar_engine_set(dstar_engine_t *e, const dstar_engine_cfg_t *cfg)
{
	e->cfg = *cfg;
}

bool dstar_engine_tx_prepare(dstar_engine_t *e)
{
	if (e->cfg.tx_inhibit || !air_session_is_open(&e->session) || !(e->session.fit & AIR_FIT_TX) || !e->cfg.silence)
		return false;
	if (air_session_tx_state(&e->session) == AIR_TX_IDLE) {
		e->tx_stop = e->tx_ended = false;
		dstar_call_tx_begin(&e->tx, &e->cfg.tx_hdr, e->cfg.tx_msg);
	}
	return air_session_tx_prime(&e->session);
}

bool dstar_engine_tx_go(dstar_engine_t *e)
{
	if (!air_session_tx_go(&e->session))
		return false;
	e->view.tx_calls++;
	return true;
}

void dstar_engine_tx_stop(dstar_engine_t *e)
{
	e->tx_stop = true;
}

void dstar_engine_tx_abort(dstar_engine_t *e)
{
	air_session_tx_abort(&e->session);
}

air_tx_state_t dstar_engine_tx_state(const dstar_engine_t *e)
{
	return air_session_tx_state(&e->session);
}

void dstar_engine_tx_clear(dstar_engine_t *e)
{
	air_session_tx_clear(&e->session);
}

void dstar_engine_view(const dstar_engine_t *e, dstar_engine_view_t *out)
{
	uint8_t seq;

	// The provider's task writes the view in a few instructions: read again if it did meanwhile
	do {
		seq = e->seq;
		*out = e->view;
	} while ((seq & 1) || seq != e->seq);
	out->tx = (uint8_t)air_session_tx_state(&e->session);
}
