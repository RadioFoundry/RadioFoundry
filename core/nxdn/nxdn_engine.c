// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// NXDN conventional voice on a channel (nxdn_engine.h), on the shared voice engine (air_voice.h)
#include "nxdn_engine.h"
#include "config.h"

#include <string.h>

_Static_assert(NXDN_EHR_BYTES <= AIR_VOICE_FRAME_MAX, "a voice frame fits air_voice's");

const air_mode_t *nxdn_engine_mode(bool rate9600)
{
	return rate9600 ? &nxdn_air_mode_9600 : &nxdn_air_mode;
}

uint32_t nxdn_engine_fit(const hal_air_t *air, bool rate9600)
{
	return air_session_fit(air, nxdn_engine_mode(rate9600));
}

// ── Receive (provider's task) ──
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

static uint16_t key_for(const nxdn_engine_t *e, uint8_t cipher, uint8_t id)
{
#if CONFIG_NXDN_SCRAMBLER
	if (cipher == NXDN_CIPHER_SCRAMBLE && id < NXDN_KEY_IDS && e->hooks.traffic_key) {
		uint16_t key = e->hooks.traffic_key(e->cfg.system, cipher, id, e->ctx);
		if (key <= NXDN_SCRAMBLE_KEY_MAX)
			return key;
	}
#else
	(void)e;
	(void)cipher;
	(void)id;
#endif
	return 0;
}

static void notify(nxdn_engine_t *e, const nxdn_call_t *c, air_voice_event_t ev)
{
	if (ev == AIR_VOICE_START && e->hooks.rx_start)
		e->hooks.rx_start(c, e->ctx);
	else if (ev == AIR_VOICE_UPDATE && e->hooks.rx_update)
		e->hooks.rx_update(c, e->ctx);
	else if (ev == AIR_VOICE_END && e->hooks.rx_end)
		e->hooks.rx_end(e->ctx);
}

static void rx_fields(nxdn_engine_t *e, const nxdn_call_t *c, bool start)
{
	if (start || c->vcall.cipher != e->heard.vcall.cipher || c->vcall.key_id != e->heard.vcall.key_id)
		e->rx.scramble_key = key_for(e, c->vcall.cipher, c->vcall.key_id);
	// A field that came in late may say the call is not for us after all, or that it turned encrypted
	bool play = wanted(e, c) && !c->iv && (c->vcall.cipher == NXDN_CIPHER_NONE || e->rx.scramble_key);

	notify(e, c, air_voice_rx_fields(&e->av, c, start, play));
}

static void on_start(const nxdn_call_t *c, void *ctx)
{
	rx_fields(ctx, c, true);
}

static void on_update(const nxdn_call_t *c, void *ctx)
{
	rx_fields(ctx, c, false);
}

static void on_voice(const uint8_t ehr[NXDN_EHR_BYTES], void *ctx)
{
	nxdn_engine_t *e = ctx;

	// The squelch settings changed inside this call: it is judged again with the fields it has
	if (air_voice_recheck_due(&e->av))
		rx_fields(e, &e->rx.call, false);
	if (air_voice_rx_voice(&e->av) && e->hooks.rx_voice)
		e->hooks.rx_voice(ehr, e->ctx);
}

static void on_end(void *ctx)
{
	nxdn_engine_t *e = ctx;

	notify(e, NULL, air_voice_rx_end(&e->av));
}

static const nxdn_call_sink_t sink = { on_start, on_update, on_voice, on_end };

// ── Transmit: the call layer behind air_voice's tx_fill ──
static uint16_t t_ready(void *t, uint16_t *first) { return nxdn_call_tx_ready(t, first); }
static const uint8_t *t_syms(void *t) { return nxdn_call_tx_syms(t); }
static void t_sent(void *t, uint16_t n) { nxdn_call_tx_sent(t, n); }
static bool t_done(void *t) { return nxdn_call_tx_done(t); }
static bool t_voice(void *t, const uint8_t *v) { return nxdn_call_tx_voice(t, v); }
static void t_end(void *t, const uint8_t *silence) { nxdn_call_tx_end(t, silence); }

static const air_voice_tx_ops_t tx_ops = { t_ready, t_syms, t_sent, t_done, t_voice, t_end };

bool nxdn_engine_begin(nxdn_engine_t *e, const hal_air_t *air, const nxdn_engine_cfg_t *cfg,
		       const nxdn_engine_hooks_t *hooks, void *ctx)
{
	memset(e, 0, sizeof *e);
	e->cfg = *cfg;
	e->hooks = *hooks;
	e->ctx = ctx;
	e->av.tx_voice = hooks->tx_voice;
	e->av.ctx = ctx;
	e->av.silence = cfg->silence;
	nxdn_call_rx_init(&e->rx, cfg->rx_ran, &sink, e);
	return air_voice_open(&e->av, air, nxdn_engine_mode(cfg->rate9600), &e->rx.rx, e->frame, e->soft,
			      NXDN_FRAME_SYMS, &tx_ops, &e->tx, &e->heard, sizeof e->heard);
}

void nxdn_engine_end(nxdn_engine_t *e)
{
	air_voice_close(&e->av);
}

void nxdn_engine_retuned(nxdn_engine_t *e)
{
	air_voice_retuned(&e->av);
}

void nxdn_engine_set(nxdn_engine_t *e, const nxdn_engine_cfg_t *cfg)
{
	bool rate = e->cfg.rate9600;

	e->cfg = *cfg;
	e->cfg.rate9600 = rate;		// the mode is the session's: a new rate is a new session
	e->av.silence = cfg->silence;
	e->rx.rx.ran = cfg->rx_ran;
	e->rx.scramble_key = key_for(e, e->rx.call.vcall.cipher, e->rx.call.vcall.key_id);
	air_voice_recheck(&e->av);
}

bool nxdn_engine_tx_prepare(nxdn_engine_t *e)
{
	const nxdn_engine_cfg_t *cfg = &e->cfg;
	uint8_t cipher = cfg->tx_cipher;
	uint8_t id = cfg->tx_key_id;

	if (cfg->tx_inhibit)
		return false;
	uint16_t key = key_for(e, cipher, id);
	if (cipher != NXDN_CIPHER_NONE && !key)
		return false;
	if (!air_voice_tx_can(&e->av, true))
		return false;
	if (air_voice_tx_begin(&e->av)) {
		nxdn_vcall_t v = {
			.msg = NXDN_MSG_VCALL,
			.cc_opt = cfg->emergency ? NXDN_CC_EMERGENCY : 0,
			.call_type = cfg->tx_call_type,
			.voice_opt = cfg->rate9600 ? NXDN_VOICE_9600_EHR : NXDN_VOICE_4800_EHR,
			.src = cfg->unit_id,
			.dst = cfg->tx_dst,
			.cipher = cipher,
			.key_id = cipher == NXDN_CIPHER_NONE ? 0 : id,
		};

		// A subscriber unit sends inbound, direct or through a repeater (table 5.2-2)
		nxdn_call_tx_begin(&e->tx, &v, cfg->tx_ran, NXDN_DIR_INBOUND, cfg->rate9600);
		e->tx.scramble_key = key;
	}
	return air_voice_tx_prime(&e->av);
}

bool nxdn_engine_tx_go(nxdn_engine_t *e)
{
	return air_voice_tx_go(&e->av);
}

void nxdn_engine_tx_stop(nxdn_engine_t *e)
{
	air_voice_tx_stop(&e->av);
}

void nxdn_engine_tx_abort(nxdn_engine_t *e)
{
	air_voice_tx_abort(&e->av);
}

air_tx_state_t nxdn_engine_tx_state(const nxdn_engine_t *e)
{
	return air_voice_tx_state(&e->av);
}

void nxdn_engine_tx_clear(nxdn_engine_t *e)
{
	air_voice_tx_clear(&e->av);
}

void nxdn_engine_view(const nxdn_engine_t *e, nxdn_engine_view_t *out)
{
	air_voice_view_t v;

	air_voice_view(&e->av, &v, &out->rx);
	out->rx_active = v.rx_active;
	out->rx_played = v.rx_played;
	out->tx = v.tx;
	out->rx_calls = v.rx_calls;
	out->tx_calls = v.tx_calls;
	out->tx_filler = v.tx_filler;
}
