// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// D-STAR DV on a board's flat FM audio taps (dstar_engine.h)
#include <string.h>
#include "dstar/dstar_engine.h"

static void push_soft(void *ctx, int16_t soft, int16_t unit)
{
	dstar_engine_t *e = ctx;
	bool was = dstar_rx_locked(&e->u.rx.fr);
	dstar_rx_bit(&e->u.rx.fr, soft, unit);
	bool now = dstar_rx_locked(&e->u.rx.fr);
	if (now != was) {
		dstar_gmsk_demod_lock(&e->u.rx.dm, now);
	}
}

static uint16_t floor_hz(const dstar_path_t *p)
{
	return p->lfr_floor_hz ? p->lfr_floor_hz : 60u;
}

bool dstar_engine_rx_start(dstar_engine_t *e, const dstar_path_t *path, const dstar_rx_cb_t *cb)
{
	memset(e, 0, sizeof(*e));
	e->path = *path;
	if (!dstar_gmsk_demod_init(&e->u.rx.dm, path->rate, push_soft, e)) {
		return false;
	}
	e->u.rx.dm.invert = path->rx_invert;
	dstar_lfr_init(&e->u.rx.dm.lfr, path->rate, path->rx_hpf_hz, floor_hz(path));
	dstar_rx_init(&e->u.rx.fr, cb);
	e->mode = DSTAR_ENGINE_RX;
	return true;
}

void dstar_engine_rx(dstar_engine_t *e, const int16_t *s, size_t n)
{
	if (e->mode == DSTAR_ENGINE_RX) {
		dstar_gmsk_demod(&e->u.rx.dm, s, n);
	}
}

static int pull_bit(void *ctx)
{
	dstar_engine_t *e = ctx;
	int b = dstar_tx_bit(&e->u.tx.fr);
	return b < 0 ? -1 : (e->path.tx_invert ? b ^ 1 : b);
}

// With no voice source the transmitter sends the silence frame until it is told to stop
static bool silence_until_stop(void *ctx, uint8_t frame[DSTAR_VOICE_BYTES])
{
	dstar_engine_t *e = ctx;
	if (e->u.tx.done) {
		return false;
	}
	memcpy(frame, dstar_voice_silence, DSTAR_VOICE_BYTES);
	return true;
}

bool dstar_engine_tx_start(dstar_engine_t *e, const dstar_path_t *path, const dstar_header_t *h, const char *msg,
			   uint16_t preamble_bits, dstar_voice_pull_t pull, void *ctx)
{
	memset(e, 0, sizeof(*e));
	e->path = *path;
	if (pull) {
		dstar_tx_init(&e->u.tx.fr, h, msg, preamble_bits, pull, ctx);
	} else {
		dstar_tx_init(&e->u.tx.fr, h, msg, preamble_bits, silence_until_stop, e);
	}
	if (!dstar_gmsk_mod_init(&e->u.tx.mod, path->rate, path->tx_amp, pull_bit, e)) {
		return false;
	}
	dstar_lfr_init(&e->u.tx.mod.lfr, path->rate, path->tx_hpf_hz, floor_hz(path));
	e->mode = DSTAR_ENGINE_TX;
	return true;
}

bool dstar_engine_tx(dstar_engine_t *e, int16_t *pcm, uint16_t n)
{
	size_t got = e->mode == DSTAR_ENGINE_TX ? dstar_gmsk_mod(&e->u.tx.mod, pcm, n) : 0;
	if (got < n) {
		memset(&pcm[got], 0, (n - got) * sizeof(pcm[0]));
	}
	return got == n;
}

void dstar_engine_tx_stop(dstar_engine_t *e)
{
	if (e->mode == DSTAR_ENGINE_TX) {
		e->u.tx.done = true;
	}
}

void dstar_engine_idle(dstar_engine_t *e)
{
	e->mode = DSTAR_ENGINE_IDLE;
}
