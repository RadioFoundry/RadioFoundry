// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The speaker's second output (spk_aux.h). The sequence per sound: the first block with sound powers the output
// (SPK_AUX_ARMED) and is queued; once lead_ms of blocks are queued the speaker opens and the output plays them, so the
// sound starts lead_ms late and whole. Each later block is queued too, sound or not, until the queue holds only
// silence; then the speaker mutes, and tail_ms later the amplifier goes off. The mic gate (spk_aux_pending) covers all
// of it: the sound reaches the speaker later than the stream's own would.
#include "spk_aux.h"
#include "config.h"
#include <string.h>

enum { ST_OFF, ST_LEAD, ST_OPEN, ST_TAIL };

static volatile uint8_t st = ST_OFF;
static uint8_t blocks;	// ST_LEAD: blocks queued; ST_TAIL: blocks since the speaker muted
static uint8_t quiet;	// ST_OPEN: silent blocks queued since the last sound
static uint32_t sounds;

uint32_t spk_aux_sounds(void)
{
	return sounds;
}

static uint8_t ms_blocks(uint8_t ms)
{
	return (uint8_t)((ms + AUDIO_BLOCK_MS - 1u) / AUDIO_BLOCK_MS);
}

void spk_aux_reset(const hal_spk_aux_t *aux)
{
	if (st != ST_OFF) {
		aux->power(SPK_AUX_OFF);
	}
	st = ST_OFF;
	blocks = 0;
	quiet = 0;
}

bool spk_aux_pending(void)
{
	return st != ST_OFF;
}

void spk_aux_block(const hal_spk_aux_t *aux, int16_t *spk)
{
	if (!aux->active()) {
		spk_aux_reset(aux);	// the stream's output reaches the speaker again: it plays from here
		return;
	}
	bool sound = false;
	for (int i = 0; i < AUDIO_BLOCK_SAMPLES && !sound; i++) {
		sound = spk[i] != 0;
	}
	if (st == ST_OFF || st == ST_TAIL) {
		if (!sound) {
			if (st == ST_TAIL && ++blocks >= ms_blocks(aux->tail_ms)) {
				spk_aux_reset(aux);
			}
			return;
		}
		if (st == ST_OFF) {
			aux->power(SPK_AUX_ARMED);
			sounds++;
		}
		st = ST_LEAD;
		blocks = 0;
	}
	aux->write(spk);
	memset(spk, 0, AUDIO_BLOCK_SAMPLES * sizeof(int16_t));
	quiet = sound ? 0u : (uint8_t)(quiet + 1u);
	uint8_t lead = ms_blocks(aux->lead_ms);
	if (st == ST_LEAD && ++blocks >= lead) {
		aux->power(SPK_AUX_OPEN);
		st = ST_OPEN;
	}
	// The output plays lead blocks behind the stream, and a block more when the stream hands one over early (the
	// queue holds lead + 1 then): once that many silent ones follow the sound, it has played out
	if (st == ST_OPEN && quiet > lead + 1u) {
		aux->power(SPK_AUX_ARMED);
		st = ST_TAIL;
		blocks = 0;
	}
}

#if !(CONFIG_NET_USRP || CONFIG_APRS_RX)
// The wrapped stream: its start installs the trampoline below around the user's callback, its stop ends any sound.
// With the audio mux (core/usbaudio/audio_mux.c) the mux's own block step calls spk_aux_block instead: one wrapper
static const hal_audio_t *hw;
static hal_audio_t wrapped;
static audio_block_cb_t user_cb;
static void *user_ctx;
static volatile bool live;	// between start and stop: a stop from the user's callback skips the block's step

static void block(const int16_t *mic, int16_t *spk, void *ctx)
{
	(void)ctx;
	audio_block_cb_t cb = user_cb;
	if (cb != NULL) {
		cb(mic, spk, user_ctx);
	}
	if (live) {
		spk_aux_block(hw->spk_aux, spk);
	}
}

static bool start(audio_block_cb_t cb, void *ctx)
{
	user_cb = cb;
	user_ctx = ctx;
	live = true;
	return hw->start(block, NULL);
}

// The callers (the radio task) run below the audio task, so no block is inside spk_aux_block while this resets
static void stop(void)
{
	live = false;
	hw->stop();
	spk_aux_reset(hw->spk_aux);
}

static const hal_audio_t *wrap(const hal_audio_t *a)
{
	hw = a;
	wrapped = *a;
	wrapped.start = start;
	wrapped.stop = stop;
	return &wrapped;
}
#define WRAP	wrap
#else
#define WRAP	NULL	// the audio mux runs the step
#endif

const struct spk_aux_core spk_aux_core = { .wrap = WRAP, .block = spk_aux_block, .reset = spk_aux_reset,
					   .pending = spk_aux_pending, .sounds = spk_aux_sounds };
