// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A session on the air interface (air_session.h).
#include "air_session.h"

#include <string.h>

uint32_t air_session_fit(const hal_air_t *air, const air_mode_t *mode)
{
	return air && air->fit ? air->fit(&mode->profile) : 0;
}

// The last chunk goes out when the queue has room for it
static void try_finish(air_session_t *s)
{
	if (s->finish && !s->finished && air_framer_tx_finish(&s->fr, s->air)) {
		s->finished = true;
		if (s->tx == AIR_TX_ON)
			s->tx = AIR_TX_ENDING;
	}
}

static void fill(air_session_t *s)
{
	if (s->finish) {
		try_finish(s);
		return;
	}
	if (s->ops.tx_fill)
		s->ops.tx_fill(s->ctx);
}

static void on_event(const air_event_t *ev, void *ctx)
{
	air_session_t *s = ctx;

	switch (ev->kind) {
	case AIR_EV_RX:
		s->stats.rx_chunks++;
		s->stats.rx_syms += ev->chunk->nsyms;
		air_framer_rx(&s->fr, ev->chunk);
		break;
	case AIR_EV_TX_STARTED:
		s->stats.tx_chunks_started++;
		s->stats.tx_started_us = ev->t_us;
		break;
	case AIR_EV_TX_UNDERRUN:
		s->stats.tx_underruns++;
		if (s->tx == AIR_TX_ON)
			fill(s);
		break;
	case AIR_EV_TX_SPACE:
		if (s->tx == AIR_TX_ON)
			fill(s);
		break;
	case AIR_EV_TX_DONE:
		if (s->tx == AIR_TX_IDLE)
			break;
		s->tx = AIR_TX_DONE;
		// What was on the channel while we sent is not a continuation of what was there before
		air_framer_rx_reset(&s->fr);
		if (s->ops.tx_done)
			s->ops.tx_done(s->ctx);
		break;
	default:
		break;
	}
}

bool air_session_open(air_session_t *s, const hal_air_t *air, const air_mode_t *mode, void *mode_ctx, uint8_t *hard,
		      int8_t *soft, uint16_t buf_syms, const air_session_ops_t *ops, void *ctx)
{
	uint32_t fit = air_session_fit(air, mode);

	memset(s, 0, sizeof *s);
	if (!(fit & (AIR_FIT_RX | AIR_FIT_TX)))
		return false;
	s->fit = fit;
	s->ops = *ops;
	s->ctx = ctx;
	air_framer_init(&s->fr, mode, mode_ctx, hard, (fit & AIR_FIT_SOFT) ? soft : NULL, buf_syms);
	// Events can come from inside open: everything they use is set
	s->air = air;
	if (!air->open(&mode->profile, on_event, s)) {
		s->air = NULL;
		return false;
	}
	return true;
}

void air_session_close(air_session_t *s)
{
	const hal_air_t *air = s->air;

	if (!air)
		return;
	if (s->tx == AIR_TX_ON || s->tx == AIR_TX_ENDING)
		air->tx_abort();
	air->close();
	s->air = NULL;
	s->tx = AIR_TX_IDLE;
}

bool air_session_is_open(const air_session_t *s)
{
	return s->air != NULL;
}

void air_session_retuned(air_session_t *s)
{
	if (!s->air)
		return;
	s->air->rx_restart();
	air_framer_rx_reset(&s->fr);
}

bool air_session_tx_prime(air_session_t *s)
{
	if (!s->air || !(s->fit & AIR_FIT_TX) || (s->tx != AIR_TX_IDLE && s->tx != AIR_TX_PRIMING))
		return false;
	if (s->tx == AIR_TX_IDLE) {
		s->finish = s->finished = false;
		air_framer_tx_start(&s->fr, s->fit);
		s->tx = AIR_TX_PRIMING;
	}
	fill(s);
	return s->air->tx_space() < s->air->tx_queue_depth;
}

bool air_session_tx_go(air_session_t *s)
{
	if (!s->air || s->tx != AIR_TX_PRIMING)
		return false;
	// From here the provider's task owns the transmit side: the state first, its events can come inside tx_begin
	s->tx = s->finished ? AIR_TX_ENDING : AIR_TX_ON;
	if (!s->air->tx_begin()) {
		s->tx = AIR_TX_DONE;
		return false;
	}
	return true;
}

void air_session_tx_abort(air_session_t *s)
{
	if (!s->air || s->tx == AIR_TX_IDLE || s->tx == AIR_TX_DONE)
		return;
	if (s->tx == AIR_TX_PRIMING) {
		// Never keyed, but chunks sit in the provider's queue: it drops them, and says done
		s->tx = AIR_TX_ON;
	}
	s->air->tx_abort();
}

air_tx_state_t air_session_tx_state(const air_session_t *s)
{
	return (air_tx_state_t)s->tx;
}

void air_session_tx_clear(air_session_t *s)
{
	if (s->tx == AIR_TX_DONE)
		s->tx = AIR_TX_IDLE;
}

uint16_t air_session_tx_space(const air_session_t *s)
{
	return s->air && !s->finish ? air_framer_tx_space(&s->fr, s->air) : 0;
}

bool air_session_tx(air_session_t *s, const uint8_t *syms, uint16_t first, uint16_t n)
{
	if (!s->air || s->finish || !air_framer_tx(&s->fr, s->air, syms, first, n))
		return false;
	s->stats.tx_syms += n;
	return true;
}

uint16_t air_session_tx_queued(const air_session_t *s)
{
	if (!s->air)
		return 0;
	return (uint16_t)((s->air->tx_queue_depth - s->air->tx_space()) * s->air->tx_chunk_syms);
}

void air_session_tx_finish(air_session_t *s)
{
	if (!s->air || s->finish)
		return;
	s->finish = true;
	try_finish(s);
}
