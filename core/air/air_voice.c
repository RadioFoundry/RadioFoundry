// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The shared part of an air-framed voice engine (air_voice.h)
#include "air_voice.h"

#include <string.h>

static void view_begin(air_voice_t *v) { v->seq++; }
static void view_end(air_voice_t *v) { v->seq++; }

// ── Receive (provider's task) ──
air_voice_event_t air_voice_rx_fields(air_voice_t *v, const void *call, bool start, bool play, void *heard,
				      uint16_t size)
{
	if (start)
		v->played = false;
	view_begin(v);
	memcpy(heard, call, size);
	v->view.rx_active = true;
	v->view.rx_played = play;
	if (start)
		v->view.rx_calls++;
	view_end(v);
	if (play && !v->played) {
		v->played = true;
		return AIR_VOICE_START;
	}
	if (play)
		return AIR_VOICE_UPDATE;
	if (v->played) {
		// A field that came in late says the call is not for us after all
		v->played = false;
		return AIR_VOICE_END;
	}
	return AIR_VOICE_NONE;
}

bool air_voice_rx_voice(const air_voice_t *v)
{
	return v->played;
}

air_voice_event_t air_voice_rx_end(air_voice_t *v)
{
	bool was = v->played;

	view_begin(v);
	v->view.rx_active = false;
	v->view.rx_played = false;
	view_end(v);
	v->played = false;
	return was ? AIR_VOICE_END : AIR_VOICE_NONE;
}

void air_voice_recheck(air_voice_t *v)
{
	v->recheck = true;
}

bool air_voice_recheck_due(air_voice_t *v)
{
	if (!v->recheck)
		return false;
	v->recheck = false;
	return true;
}

// ── Transmit ──
// Symbols to the session while its queue has room. A voice frame the encoder has not made yet is waited for as long
// as the air can spare: once less than the provider's lead plus one more chunk is queued, silence goes instead
static void tx_fill(void *ctx)
{
	air_voice_t *v = ctx;
	const air_voice_tx_ops_t *o = v->txo;
	void *tx = (uint8_t *)v + o->tx_at;
	const hal_air_t *air = v->session.air;
	uint16_t lead = (uint16_t)(((uint32_t)air->tx_lead_ms * v->session.fr.mode->profile.sym_rate) / 1000 +
				   air->tx_chunk_syms);

	for (;;) {
		uint16_t first, n = o->ready(tx, &first), space;

		if (o->done(tx)) {
			air_session_tx_finish(&v->session);
			return;
		}
		if (v->tx_stop && !v->tx_ended) {
			v->tx_ended = true;
			o->end(tx, o->silence(v));
			continue;
		}
		if (!n) {
			uint8_t f[AIR_VOICE_FRAME_MAX];
			int8_t got = o->next(v, f);

			// (not done and nothing ready: a frame waits for its next voice frame)
			if (got > 0 && o->voice(tx, f))
				continue;
			if (got < 0 ||
			    (air_session_tx_state(&v->session) != AIR_TX_PRIMING && air_session_tx_queued(&v->session) < lead)) {
				// (before the transmitter is on nothing is lost by waiting: the preamble and header are queued)
				if (got >= 0)
					v->view.tx_filler++;
				if (!o->voice(tx, o->silence(v)))
					return;
			} else {
				return;
			}
			continue;
		}
		space = air_session_tx_space(&v->session);
		if (!space)
			return;
		if (n > space)
			n = space;
		if (!air_session_tx(&v->session, o->syms(tx), first, n))
			return;
		o->sent(tx, n);
	}
}

static const air_session_ops_t ops = { tx_fill, NULL };

bool air_voice_open(air_voice_t *v, const hal_air_t *air, const air_mode_t *mode, void *mode_ctx,
		    uint8_t *frame, int8_t *soft, uint16_t frame_syms, const air_voice_tx_ops_t *txo)
{
	v->txo = txo;
	return air_session_open(&v->session, air, mode, mode_ctx, frame, soft, frame_syms, &ops, v);
}

void air_voice_close(air_voice_t *v)
{
	air_session_close(&v->session);
}

void air_voice_retuned(air_voice_t *v)
{
	air_session_retuned(&v->session);
}

bool air_voice_tx_can(const air_voice_t *v, bool need_silence)
{
	return air_session_is_open(&v->session) && (v->session.fit & AIR_FIT_TX) &&
	       (!need_silence || v->txo->silence((void *)v) != NULL);
}

bool air_voice_tx_begin(air_voice_t *v)
{
	if (air_session_tx_state(&v->session) != AIR_TX_IDLE)
		return false;
	v->tx_stop = v->tx_ended = false;
	return true;
}

bool air_voice_tx_prime(air_voice_t *v)
{
	return air_session_tx_prime(&v->session);
}

bool air_voice_tx_go(air_voice_t *v)
{
	if (!air_session_tx_go(&v->session))
		return false;
	v->view.tx_calls++;
	return true;
}

void air_voice_tx_stop(air_voice_t *v)
{
	v->tx_stop = true;
}

void air_voice_tx_abort(air_voice_t *v)
{
	air_session_tx_abort(&v->session);
}

air_tx_state_t air_voice_tx_state(const air_voice_t *v)
{
	return air_session_tx_state(&v->session);
}

void air_voice_tx_clear(air_voice_t *v)
{
	air_session_tx_clear(&v->session);
}

void air_voice_view(const air_voice_t *v, air_voice_view_t *out, const void *heard, void *call_out, uint16_t size)
{
	uint8_t seq;

	// The provider's task writes the view in a few instructions: read again if it did meanwhile
	do {
		seq = v->seq;
		*out = v->view;
		memcpy(call_out, heard, size);
	} while ((seq & 1) || seq != v->seq);
	out->tx = (uint8_t)air_session_tx_state(&v->session);
}
