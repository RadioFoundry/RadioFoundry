// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// dPMR voice on a channel (dpmr_engine.h). Its shape is NXDN's engine (nxdn_engine.c), on the same session: what
// differs is the call layer it runs and which calls it plays.
#include "dpmr_engine.h"

#include <string.h>

uint32_t dpmr_engine_fit(const hal_air_t *air)
{
	return air_session_fit(air, &dpmr_air_mode);
}

// ── Receive (provider's task) ──
static void view_begin(dpmr_engine_t *e) { e->seq++; }
static void view_end(dpmr_engine_t *e) { e->seq++; }

static bool wanted(const dpmr_engine_t *e, const dpmr_call_t *c)
{
	uint32_t to = c->called;

	if (!e->cfg.rx_mine || !(c->have & DPMR_HAVE_CALLED))
		return true;
	return to == e->cfg.called || to == DPMR_ID_ALL || to == e->cfg.own;
}

static void rx_fields(dpmr_engine_t *e, const dpmr_call_t *c, bool start)
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
		// An ID that came in late says the call is not for us after all
		e->played = false;
		if (e->hooks.rx_end)
			e->hooks.rx_end(e->ctx);
	}
}

static void on_start(const dpmr_call_t *c, void *ctx)
{
	dpmr_engine_t *e = ctx;

	e->played = false;
	rx_fields(e, c, true);
}

static void on_update(const dpmr_call_t *c, void *ctx)
{
	rx_fields(ctx, c, false);
}

static void on_voice(const uint8_t ambe[DPMR_AMBE_BYTES], void *ctx)
{
	dpmr_engine_t *e = ctx;

	// The filter changed inside this call: it is judged again with the fields it has
	if (e->recheck) {
		e->recheck = false;
		rx_fields(e, &e->rx.call, false);
	}
	if (e->played && e->hooks.rx_voice)
		e->hooks.rx_voice(ambe, e->ctx);
}

static void on_end(const dpmr_call_t *c, void *ctx)
{
	dpmr_engine_t *e = ctx;

	view_begin(e);
	e->view.rx = *c;
	e->view.rx_active = false;
	e->view.rx_played = false;
	view_end(e);
	if (e->played && e->hooks.rx_end)
		e->hooks.rx_end(e->ctx);
	e->played = false;
}

static const dpmr_call_sink_t sink = { on_start, on_update, on_voice, on_end };

// ── Transmit ──
// Symbols to the session while its queue has room. A voice frame the encoder has not made yet is waited for as
// long as the air can spare: once less than the provider's lead plus one more chunk is queued, silence goes instead
static void tx_fill(void *ctx)
{
	dpmr_engine_t *e = ctx;
	const hal_air_t *air = e->session.air;
	uint16_t lead = (uint16_t)(((uint32_t)air->tx_lead_ms * dpmr_air_mode.profile.sym_rate) / 1000 + air->tx_chunk_syms);

	for (;;) {
		uint16_t first, n = dpmr_call_tx_ready(&e->tx, &first), space;

		if (dpmr_call_tx_done(&e->tx)) {
			air_session_tx_finish(&e->session);
			return;
		}
		if (e->tx_stop && !e->tx_ended && !n) {
			uint8_t ambe[DPMR_AMBE_BYTES];

			// What the encoder made before the key went up still goes; then the superframe is finished with
			// silence and the end frame follows
			if (e->hooks.tx_voice && e->hooks.tx_voice(ambe, e->ctx) && dpmr_call_tx_voice(&e->tx, ambe))
				continue;
			e->tx_ended = true;
			dpmr_call_tx_end(&e->tx, e->cfg.silence);
			continue;
		}
		if (!n) {
			uint8_t ambe[DPMR_AMBE_BYTES];

			// (not done and nothing ready: a payload frame waits for its next voice frame)
			if (e->hooks.tx_voice && e->hooks.tx_voice(ambe, e->ctx) && dpmr_call_tx_voice(&e->tx, ambe))
				continue;
			if (!e->hooks.tx_voice ||
			    (air_session_tx_state(&e->session) != AIR_TX_PRIMING && air_session_tx_queued(&e->session) < lead)) {
				// (before the transmitter is on nothing is lost by waiting: the headers are queued)
				if (e->hooks.tx_voice)
					e->view.tx_filler++;
				if (!dpmr_call_tx_voice(&e->tx, e->cfg.silence))
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
		if (!air_session_tx(&e->session, dpmr_call_tx_syms(&e->tx), first, n))
			return;
		dpmr_call_tx_sent(&e->tx, n);
	}
}

static const air_session_ops_t ops = { tx_fill, NULL };

bool dpmr_engine_begin(dpmr_engine_t *e, const hal_air_t *air, const dpmr_engine_cfg_t *cfg,
		       const dpmr_engine_hooks_t *hooks, void *ctx)
{
	memset(e, 0, sizeof *e);
	e->cfg = *cfg;
	e->hooks = *hooks;
	e->ctx = ctx;
	dpmr_call_rx_init(&e->rx, &sink, e);
	return air_session_open(&e->session, air, &dpmr_air_mode, &e->rx.rx, e->frame, NULL, DPMR_FRAME_SYMS, &ops, e);
}

void dpmr_engine_end(dpmr_engine_t *e)
{
	air_session_close(&e->session);
}

void dpmr_engine_retuned(dpmr_engine_t *e)
{
	air_session_retuned(&e->session);
}

void dpmr_engine_set(dpmr_engine_t *e, const dpmr_engine_cfg_t *cfg)
{
	e->cfg = *cfg;
	e->recheck = true;
}

bool dpmr_engine_tx_prepare(dpmr_engine_t *e)
{
	if (e->cfg.tx_inhibit || !air_session_is_open(&e->session) || !(e->session.fit & AIR_FIT_TX) || !e->cfg.silence)
		return false;
	if (air_session_tx_state(&e->session) == AIR_TX_IDLE) {
		// What a dPMR446 radio sends for a voice call: peer to peer, even to a common ID's whole group
		dpmr_hi_t hi = {
			.ht = DPMR_HT_COMM_START,
			.called = e->cfg.called & 0xFFFFFFu,
			.own = e->cfg.own & 0xFFFFFFu,
			.mode = DPMR_M_VOICE,
			.format = DPMR_F_P2P,
		};

		e->tx_stop = e->tx_ended = false;
		dpmr_call_tx_begin(&e->tx, &hi, e->cfg.cc, e->cfg.headers);
	}
	return air_session_tx_prime(&e->session);
}

bool dpmr_engine_tx_go(dpmr_engine_t *e)
{
	if (!air_session_tx_go(&e->session))
		return false;
	e->view.tx_calls++;
	return true;
}

void dpmr_engine_tx_stop(dpmr_engine_t *e)
{
	e->tx_stop = true;
}

void dpmr_engine_tx_abort(dpmr_engine_t *e)
{
	air_session_tx_abort(&e->session);
}

air_tx_state_t dpmr_engine_tx_state(const dpmr_engine_t *e)
{
	return air_session_tx_state(&e->session);
}

void dpmr_engine_tx_clear(dpmr_engine_t *e)
{
	air_session_tx_clear(&e->session);
}

void dpmr_engine_view(const dpmr_engine_t *e, dpmr_engine_view_t *out)
{
	uint8_t seq;

	// The provider's task writes the view in a few instructions: read again if it did meanwhile
	do {
		seq = e->seq;
		*out = e->view;
	} while ((seq & 1) || seq != e->seq);
	out->tx = (uint8_t)air_session_tx_state(&e->session);
}
