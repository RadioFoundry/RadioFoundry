// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// NXDN conventional voice on a channel (nxdn_engine.h).
#include "nxdn_engine.h"

#include <string.h>

const air_mode_t *nxdn_engine_mode(bool rate9600)
{
	return rate9600 ? &nxdn_air_mode_9600 : &nxdn_air_mode;
}

uint32_t nxdn_engine_fit(const hal_air_t *air, bool rate9600)
{
	return air_session_fit(air, nxdn_engine_mode(rate9600));
}

// ── Receive (provider's task) ──
static void view_begin(nxdn_engine_t *e) { e->seq++; }
static void view_end(nxdn_engine_t *e) { e->seq++; }

// The squelch on top of the RAN (TS 1-B 4.1.7): every call, or only the ones to our group, to all or to us. Nothing
// is played before the VCALL is in (the call layer holds the voice too)
static bool wanted(const nxdn_engine_t *e, const nxdn_call_t *c)
{
	if (!c->have_vcall)
		return false;
	if (!e->cfg.rx_gid)
		return true;
	if (c->vcall.call_type == NXDN_CALL_INDIVIDUAL)
		return c->vcall.dst == e->cfg.unit_id;
	return c->vcall.dst == e->cfg.rx_gid || c->vcall.dst == NXDN_ID_ALL;
}

static bool clear(const nxdn_call_t *c)
{
	return c->vcall.cipher == NXDN_CIPHER_NONE && !c->iv;
}

static void rx_fields(nxdn_engine_t *e, const nxdn_call_t *c, bool start)
{
	bool play = wanted(e, c) && clear(c);

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
		// A field that came in late says the call is not for us after all (or it turned encrypted)
		e->played = false;
		if (e->hooks.rx_end)
			e->hooks.rx_end(e->ctx);
	}
}

static void on_start(const nxdn_call_t *c, void *ctx)
{
	nxdn_engine_t *e = ctx;

	e->played = false;
	rx_fields(e, c, true);
}

static void on_update(const nxdn_call_t *c, void *ctx)
{
	rx_fields(ctx, c, false);
}

static void on_voice(const uint8_t ehr[NXDN_EHR_BYTES], void *ctx)
{
	nxdn_engine_t *e = ctx;

	// The squelch settings changed inside this call: it is judged again with the fields it has
	if (e->recheck) {
		e->recheck = false;
		rx_fields(e, &e->rx.call, false);
	}
	if (e->played && e->hooks.rx_voice)
		e->hooks.rx_voice(ehr, e->ctx);
}

static void on_end(void *ctx)
{
	nxdn_engine_t *e = ctx;

	view_begin(e);
	e->view.rx_active = false;
	e->view.rx_played = false;
	view_end(e);
	if (e->played && e->hooks.rx_end)
		e->hooks.rx_end(e->ctx);
	e->played = false;
}

static const nxdn_call_sink_t sink = { on_start, on_update, on_voice, on_end };

// ── Transmit ──
// Symbols to the session while its queue has room. A voice frame the encoder has not made yet is waited for as
// long as the air can spare: once less than the provider's lead plus one more chunk is queued, silence goes instead
static void tx_fill(void *ctx)
{
	nxdn_engine_t *e = ctx;
	const hal_air_t *air = e->session.air;
	uint16_t lead = (uint16_t)(((uint32_t)air->tx_lead_ms * e->mode->profile.sym_rate) / 1000 + air->tx_chunk_syms);

	for (;;) {
		uint16_t first, n = nxdn_call_tx_ready(&e->tx, &first), space;

		if (nxdn_call_tx_done(&e->tx)) {
			air_session_tx_finish(&e->session);
			return;
		}
		if (e->tx_stop && !e->tx_ended) {
			e->tx_ended = true;
			nxdn_call_tx_end(&e->tx, e->cfg.silence);
			continue;
		}
		if (!n) {
			uint8_t ehr[NXDN_EHR_BYTES];

			// (not done and nothing ready: a frame waits for its next voice frame)
			if (e->hooks.tx_voice && e->hooks.tx_voice(ehr, e->ctx)) {
				nxdn_call_tx_voice(&e->tx, ehr);
			} else if (!e->hooks.tx_voice ||
				   (air_session_tx_state(&e->session) != AIR_TX_PRIMING && air_session_tx_queued(&e->session) < lead)) {
				// (before the transmitter is on nothing is lost by waiting: the preamble and header are queued)
				if (e->hooks.tx_voice)
					e->view.tx_filler++;
				nxdn_call_tx_voice(&e->tx, e->cfg.silence);
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
		if (!air_session_tx(&e->session, nxdn_call_tx_syms(&e->tx), first, n))
			return;
		nxdn_call_tx_sent(&e->tx, n);
	}
}

static const air_session_ops_t ops = { tx_fill, NULL };

bool nxdn_engine_begin(nxdn_engine_t *e, const hal_air_t *air, const nxdn_engine_cfg_t *cfg,
		       const nxdn_engine_hooks_t *hooks, void *ctx)
{
	memset(e, 0, sizeof *e);
	e->cfg = *cfg;
	e->hooks = *hooks;
	e->ctx = ctx;
	e->mode = nxdn_engine_mode(cfg->rate9600);
	nxdn_call_rx_init(&e->rx, cfg->rx_ran, &sink, e);
	return air_session_open(&e->session, air, e->mode, &e->rx.rx, e->frame, e->soft, NXDN_FRAME_SYMS, &ops, e);
}

void nxdn_engine_end(nxdn_engine_t *e)
{
	air_session_close(&e->session);
}

void nxdn_engine_retuned(nxdn_engine_t *e)
{
	air_session_retuned(&e->session);
}

void nxdn_engine_set(nxdn_engine_t *e, const nxdn_engine_cfg_t *cfg)
{
	bool rate = e->cfg.rate9600;

	e->cfg = *cfg;
	e->cfg.rate9600 = rate;		// the mode is the session's: a new rate is a new session
	e->rx.rx.ran = cfg->rx_ran;
	e->recheck = true;
}

bool nxdn_engine_tx_prepare(nxdn_engine_t *e)
{
	if (!air_session_is_open(&e->session) || !(e->session.fit & AIR_FIT_TX) || !e->cfg.silence)
		return false;
	if (air_session_tx_state(&e->session) == AIR_TX_IDLE) {
		nxdn_vcall_t v = {
			.msg = NXDN_MSG_VCALL,
			.cc_opt = e->cfg.emergency ? NXDN_CC_EMERGENCY : 0,
			.call_type = e->cfg.tx_call_type,
			.voice_opt = e->cfg.rate9600 ? NXDN_VOICE_9600_EHR : NXDN_VOICE_4800_EHR,
			.src = e->cfg.unit_id,
			.dst = e->cfg.tx_dst,
			.cipher = NXDN_CIPHER_NONE,
		};

		e->tx_stop = e->tx_ended = false;
		// A subscriber unit sends inbound, direct or through a repeater (table 5.2-2)
		nxdn_call_tx_begin(&e->tx, &v, e->cfg.tx_ran, NXDN_DIR_INBOUND, e->cfg.rate9600);
	}
	return air_session_tx_prime(&e->session);
}

bool nxdn_engine_tx_go(nxdn_engine_t *e)
{
	if (!air_session_tx_go(&e->session))
		return false;
	e->view.tx_calls++;
	return true;
}

void nxdn_engine_tx_stop(nxdn_engine_t *e)
{
	e->tx_stop = true;
}

void nxdn_engine_tx_abort(nxdn_engine_t *e)
{
	air_session_tx_abort(&e->session);
}

air_tx_state_t nxdn_engine_tx_state(const nxdn_engine_t *e)
{
	return air_session_tx_state(&e->session);
}

void nxdn_engine_tx_clear(nxdn_engine_t *e)
{
	air_session_tx_clear(&e->session);
}

void nxdn_engine_view(const nxdn_engine_t *e, nxdn_engine_view_t *out)
{
	uint8_t seq;

	// The provider's task writes the view in a few instructions: read again if it did meanwhile
	do {
		seq = e->seq;
		*out = e->view;
	} while ((seq & 1) || seq != e->seq);
	out->tx = (uint8_t)air_session_tx_state(&e->session);
}
