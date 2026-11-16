// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// P25 conventional voice on a channel (p25_engine.h).
#include "p25_engine.h"

#include <string.h>

#if CONFIG_P25_TRUNK
#include "p25_tsbk.h"
#endif

uint32_t p25_engine_fit(const hal_air_t *air)
{
	return air_session_fit(air, &p25_air_mode);
}

// ── Receive (provider's task) ──
static void view_begin(p25_engine_t *e) { e->seq++; }
static void view_end(p25_engine_t *e) { e->seq++; }

// The squelch on top of the NAC: every call, or only the ones to our talk group, to everyone or to us
static bool wanted(const p25_engine_t *e, const p25_call_t *c)
{
	if (!e->cfg.rx_tgid)
		return true;
	if (c->have_lc && c->lc.lcf == P25_LCF_UNIT)
		return c->lc.dst == e->cfg.unit_id;
	// Before the link control is in, the header's talk group decides; a late entry waits for its first LDU1
	if (!c->have_lc && c->late)
		return false;
	return c->lc.tgid == e->cfg.rx_tgid || c->lc.tgid == P25_TGID_EVERYONE;
}

static void rx_fields(p25_engine_t *e, const p25_call_t *c, bool start)
{
	bool play = wanted(e, c) && c->algid == P25_ALGID_CLEAR;

#if CONFIG_P25_TRUNK
	if (e->hooks.rx_call)
		e->hooks.rx_call(c, e->ctx);
#endif
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

static void on_start(const p25_call_t *c, void *ctx)
{
	p25_engine_t *e = ctx;

	e->played = false;
	rx_fields(e, c, true);
}

static void on_update(const p25_call_t *c, void *ctx)
{
	rx_fields(ctx, c, false);
}

static void on_voice(const uint8_t imbe[P25_IMBE_BYTES], uint8_t errs, void *ctx)
{
	p25_engine_t *e = ctx;

	// The squelch settings changed inside this call: it is judged again with the fields it has
	if (e->recheck) {
		e->recheck = false;
		rx_fields(e, &e->rx.call, false);
	}
	if (e->played && e->hooks.rx_voice)
		e->hooks.rx_voice(imbe, errs, e->ctx);
}

static void on_end(void *ctx)
{
	p25_engine_t *e = ctx;

	view_begin(e);
	e->view.rx_active = false;
	e->view.rx_played = false;
	view_end(e);
	if (e->played && e->hooks.rx_end)
		e->hooks.rx_end(e->ctx);
	e->played = false;
}

#if CONFIG_P25_TRUNK
static void on_unit(const p25_rx_unit_t *u, void *ctx)
{
	p25_engine_t *e = ctx;

	if (e->hooks.rx_unit)
		e->hooks.rx_unit(u, e->ctx);
}

static const p25_call_sink_t sink = { on_start, on_update, on_voice, on_end, on_unit };
#else
static const p25_call_sink_t sink = { on_start, on_update, on_voice, on_end };
#endif

// ── Transmit ──
// Symbols to the session while its queue has room. A voice frame the encoder has not made yet is waited for as
// long as the air can spare: once less than the provider's lead plus one more chunk is queued, silence goes instead
static void tx_fill(void *ctx)
{
	p25_engine_t *e = ctx;
	const hal_air_t *air = e->session.air;
	uint16_t lead = (uint16_t)(((uint32_t)air->tx_lead_ms * p25_air_mode.profile.sym_rate) / 1000 + air->tx_chunk_syms);

	for (;;) {
		uint16_t first, n = p25_call_tx_ready(&e->tx, &first), space;

		if (p25_call_tx_done(&e->tx)) {
			air_session_tx_finish(&e->session);
			return;
		}
		if (e->tx_stop && !e->tx_ended) {
			e->tx_ended = true;
			p25_call_tx_end(&e->tx, e->cfg.silence);
			continue;
		}
		if (!n) {
			uint8_t imbe[P25_IMBE_BYTES];

			// (not done and nothing ready: an LDU waits for its next voice frame)
			if (e->hooks.tx_voice && e->hooks.tx_voice(imbe, e->ctx)) {
				p25_call_tx_voice(&e->tx, imbe);
			} else if (!e->hooks.tx_voice ||
				   (air_session_tx_state(&e->session) != AIR_TX_PRIMING && air_session_tx_queued(&e->session) < lead)) {
				// (before the transmitter is on nothing is lost by waiting: the header is queued, that is enough)
				if (e->hooks.tx_voice)
					e->view.tx_filler++;
				p25_call_tx_voice(&e->tx, e->cfg.silence);
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
		if (!air_session_tx(&e->session, e->tx.unit, first, n))
			return;
		p25_call_tx_sent(&e->tx, n);
	}
}

static const air_session_ops_t ops = { tx_fill, NULL };

bool p25_engine_begin(p25_engine_t *e, const hal_air_t *air, const p25_engine_cfg_t *cfg, const p25_engine_hooks_t *hooks,
		      void *ctx)
{
	memset(e, 0, sizeof *e);
	e->cfg = *cfg;
	e->hooks = *hooks;
	e->ctx = ctx;
	p25_call_rx_init(&e->rx, cfg->rx_nac, &sink, e);
#if CONFIG_P25_TRUNK
	e->rx.rx.trunk = cfg->trunk;
#endif
	return air_session_open(&e->session, air, &p25_air_mode, &e->rx.rx, e->frame, NULL, P25_UNIT_SYMS_MAX, &ops, e);
}

void p25_engine_end(p25_engine_t *e)
{
	air_session_close(&e->session);
}

void p25_engine_retuned(p25_engine_t *e)
{
	air_session_retuned(&e->session);
#if CONFIG_P25_TRUNK
	// A trunked receiver moves between a voice channel and its control channel: the call in hand was on the old
	// frequency and ends here, as a lost signal does (from the task that retunes, as the framer's reset above)
	if (e->cfg.trunk) {
		e->rx.rx.last_duid = 0xFF;
		e->rx.rx.lost(e->rx.rx.ctx);
	}
#endif
}

void p25_engine_set(p25_engine_t *e, const p25_engine_cfg_t *cfg)
{
	e->cfg = *cfg;
	e->rx.rx.nac = cfg->rx_nac;
#if CONFIG_P25_TRUNK
	e->rx.rx.trunk = cfg->trunk;
#endif
	e->recheck = true;
}

bool p25_engine_tx_prepare(p25_engine_t *e)
{
	p25_lc_t lc = e->cfg.tx_lc;

	if (!air_session_is_open(&e->session) || !(e->session.fit & AIR_FIT_TX) || !e->cfg.silence)
		return false;
	if (air_session_tx_state(&e->session) == AIR_TX_IDLE) {
		lc.src = e->cfg.unit_id;
		e->tx_stop = e->tx_ended = false;
		p25_call_tx_begin(&e->tx, e->cfg.tx_nac, &lc, e->cfg.talkaround ? P25_SS_TALKAROUND : P25_SS_UNKNOWN);
	}
	return air_session_tx_prime(&e->session);
}

#if CONFIG_P25_TRUNK
bool p25_engine_tx_burst(p25_engine_t *e, const uint8_t blk[P25_TSBK_BYTES])
{
	if (!air_session_is_open(&e->session) || !(e->session.fit & AIR_FIT_TX) ||
	    air_session_tx_state(&e->session) != AIR_TX_IDLE)
		return false;
	e->tx_stop = e->tx_ended = true;	// one unit: nothing to end
	p25_call_tx_single(&e->tx, p25_isp_unit(e->tx.unit, e->cfg.tx_nac, blk));
	return air_session_tx_prime(&e->session);
}
#endif

bool p25_engine_tx_go(p25_engine_t *e)
{
	if (!air_session_tx_go(&e->session))
		return false;
	if (!e->tx_ended)	// (a trunking burst is not a call)
		e->view.tx_calls++;
	return true;
}

void p25_engine_tx_stop(p25_engine_t *e)
{
	e->tx_stop = true;
}

void p25_engine_tx_abort(p25_engine_t *e)
{
	air_session_tx_abort(&e->session);
}

air_tx_state_t p25_engine_tx_state(const p25_engine_t *e)
{
	return air_session_tx_state(&e->session);
}

void p25_engine_tx_clear(p25_engine_t *e)
{
	air_session_tx_clear(&e->session);
}

void p25_engine_view(const p25_engine_t *e, p25_engine_view_t *out)
{
	uint8_t seq;

	// The provider's task writes the view in a few instructions: read again if it did meanwhile
	do {
		seq = e->seq;
		*out = e->view;
	} while ((seq & 1) || seq != e->seq);
	out->tx = (uint8_t)air_session_tx_state(&e->session);
}
