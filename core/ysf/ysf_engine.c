// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Yaesu System Fusion for a board (ysf_engine.h)
#include <string.h>
#include "ysf/ysf_engine.h"

// The demodulator's symbols into the receiver; the demodulator's loops slow down while the receiver holds a sync
static void rx_push(void *ctx, int16_t soft, int16_t unit)
{
	ysf_engine_t *e = ctx;
	bool was = ysf_rx_locked(&e->u.rx.fr);
	ysf_rx_symbol(&e->u.rx.fr, e->path.rx_invert ? (int16_t)-soft : soft, unit);
	bool now = ysf_rx_locked(&e->u.rx.fr);
	if (now != was) {
		ysf_c4fm_demod_lock(&e->u.rx.dm, now);
	}
}

bool ysf_engine_rx_start(ysf_engine_t *e, const ysf_path_t *path, const ysf_rx_cb_t *cb)
{
	memset(e, 0, sizeof(*e));
	e->path = *path;
	ysf_rx_init(&e->u.rx.fr, cb);
	// A symbol path has no samples: the demodulator stays unused, but a rate of zero mustn't refuse it
	if (path->rate != 0) {
		if (!ysf_c4fm_demod_init(&e->u.rx.dm, path->rate, rx_push, e)) {
			return false;
		}
		ysf_lfr_init(&e->u.rx.dm.lfr, path->rate, path->rx_hpf_hz, path->lfr_floor_hz);
	}
	e->mode = YSF_ENGINE_RX;
	return true;
}

void ysf_engine_rx(ysf_engine_t *e, const int16_t *s, size_t n)
{
	if (e->mode == YSF_ENGINE_RX && e->u.rx.dm.push != NULL) {
		ysf_c4fm_demod(&e->u.rx.dm, s, n);
	}
}

void ysf_engine_rx_dibits(ysf_engine_t *e, const uint8_t *d, size_t n)
{
	if (e->mode != YSF_ENGINE_RX) {
		return;
	}
	for (size_t i = 0; i < n; i++) {
		ysf_rx_dibit(&e->u.rx.fr, (uint8_t)(e->path.rx_invert ? d[i] ^ 2u : d[i]));
	}
}

// The transmitter's dibits with the path's polarity: inverting the levels flips the first bit of each dibit
static int tx_pull(void *ctx)
{
	ysf_engine_t *e = ctx;
	int d = ysf_tx_dibit(&e->u.tx.fr);
	return d < 0 ? d : (e->path.tx_invert ? d ^ 2 : d);
}

bool ysf_engine_tx_start(ysf_engine_t *e, const ysf_path_t *path, const ysf_tx_config_t *cfg)
{
	memset(e, 0, sizeof(*e));
	e->path = *path;
	ysf_tx_start(&e->u.tx.fr, cfg);
	if (path->rate != 0) {
		if (!ysf_c4fm_mod_init(&e->u.tx.mod, path->rate, path->tx_amp, tx_pull, e)) {
			return false;
		}
		ysf_lfr_init(&e->u.tx.lfr, path->rate, path->tx_hpf_hz, path->lfr_floor_hz);
	}
	e->mode = YSF_ENGINE_TX;
	return true;
}

bool ysf_engine_tx(ysf_engine_t *e, int16_t *pcm, uint16_t n)
{
	if (e->mode != YSF_ENGINE_TX || e->u.tx.done || e->u.tx.mod.pull == NULL) {
		memset(pcm, 0, n * sizeof(pcm[0]));
		return false;
	}
	if (!ysf_c4fm_mod(&e->u.tx.mod, pcm, n)) {
		e->u.tx.done = true;
	}
	if (e->u.tx.lfr.kc != 0) {
		for (uint16_t i = 0; i < n; i++) {
			pcm[i] = ysf_lfr(&e->u.tx.lfr, pcm[i]);
		}
	}
	return !e->u.tx.done;
}

bool ysf_engine_tx_block(ysf_engine_t *e, uint8_t raw[YSF_PHY_BLOCK_BYTES])
{
	memset(raw, 0, YSF_PHY_BLOCK_BYTES);
	if (e->mode != YSF_ENGINE_TX || e->u.tx.done) {
		return false;
	}
	for (uint32_t i = 0; i < YSF_PHY_BLOCK_SYMS; i++) {
		int d = e->u.tx.done ? -1 : tx_pull(e);
		if (d < 0) {
			if (i == 0) {
				e->u.tx.done = true;
				return false;
			}
			e->u.tx.done = true;
			d = (i & 1u) ? 2 : 0;	// +1, -1: no DC while the chip finishes the block
		}
		raw[i >> 2] |= (uint8_t)(d << (6u - 2u * (i & 3u)));
	}
	return true;
}

void ysf_engine_tx_stop(ysf_engine_t *e)
{
	if (e->mode == YSF_ENGINE_TX) {
		ysf_tx_stop(&e->u.tx.fr);
	}
}

void ysf_engine_idle(ysf_engine_t *e)
{
	memset(e, 0, sizeof(*e));
}
