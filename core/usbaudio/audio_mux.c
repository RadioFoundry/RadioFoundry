// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The PCM stream shared between the radio's users and a host audio session (audio_mux.h)
#include "usbaudio/audio_mux.h"

#include <string.h>

#define RX_MASK		(AUDIO_MUX_RX_CAP - 1u)

static const hal_audio_t *hw;
static hal_audio_t wrapped;

static audio_block_cb_t volatile prim_cb;	// the radio's user that started the stream (DMR voice or a beep)
static void *volatile prim_ctx;
static bool running;				// the board's stream runs (radio task)
static volatile audio_mux_rx_t host_rx;
static volatile bool host_tx_dmr;
#define host_rx_on	(host_rx != AUDIO_MUX_RX_OFF)
static uint32_t retry_ms;
static bool retry_armed;

static int16_t rx_ring[AUDIO_MUX_RX_CAP];
static volatile uint32_t rx_wr, rx_rd;		// free-running: the audio task writes, the host task reads
static pcm_slip_t slip;
static int16_t host_mic[AUDIO_BLOCK_SAMPLES];
static audio_mux_stats_t st;

static void rx_put(const int16_t *pcm, uint32_t n)
{
	uint32_t room = AUDIO_MUX_RX_CAP - (rx_wr - rx_rd);
	if (n > room) {
		st.rx_overflow += n - room;	// the newest go: the reader owns rx_rd
		n = room;
	}
	uint32_t w = rx_wr;
	for (uint32_t i = 0; i < n; i++) {
		rx_ring[(w + i) & RX_MASK] = pcm[i];
	}
	__atomic_signal_fence(__ATOMIC_RELEASE);
	rx_wr = w + n;
	st.rx_samples += n;
}

static void block(const int16_t *mic, int16_t *spk, void *ctx)
{
	(void)ctx;
	st.blocks++;
	const int16_t *in = mic;
	if (host_tx_dmr) {
		pcm_slip_pull(&slip, host_mic, AUDIO_BLOCK_SAMPLES);
		in = host_mic;
		st.tx_blocks++;
	}
	audio_block_cb_t cb = prim_cb;
	if (cb != NULL) {
		cb(in, spk, prim_ctx);
	} else {
		memset(spk, 0, AUDIO_BLOCK_SAMPLES * sizeof *spk);
	}
	audio_mux_rx_t src = host_rx;
	if (src == AUDIO_MUX_RX_SILENT) {
		static const int16_t silence[AUDIO_BLOCK_SAMPLES];
		rx_put(silence, AUDIO_BLOCK_SAMPLES);
	} else if (src != AUDIO_MUX_RX_OFF) {
		rx_put(src == AUDIO_MUX_RX_OUTPUT ? spk : mic, AUDIO_BLOCK_SAMPLES);
	}
}

static bool hw_start(void)
{
	running = hw->start(block, NULL);
	return running;
}

// --- the stream the radio's users see ---

static bool w_start(audio_block_cb_t cb, void *ctx)
{
	prim_ctx = ctx;
	prim_cb = cb;
	if (running) {
		return true;
	}
	if (!hw_start()) {
		prim_cb = NULL;
		return false;
	}
	return true;
}

// Also called from the callback itself (the DMR voice ends its stream there): the board's stop is safe there
static void w_stop(void)
{
	prim_cb = NULL;
	if (!host_rx_on) {
		running = false;
		hw->stop();
	}
}

static void w_speaker_pcm(bool on)
{
	if (hw->speaker_pcm != NULL) {
		hw->speaker_pcm(on);
	}
}

const hal_audio_t *audio_mux_wrap(const hal_audio_t *board_audio)
{
	if (board_audio == NULL) {
		return NULL;
	}
	hw = board_audio;
	wrapped.start = w_start;
	wrapped.stop = w_stop;
	wrapped.speaker_pcm = w_speaker_pcm;
	pcm_slip_init(&slip, AUDIO_MUX_TX_TARGET, AUDIO_BLOCK_SAMPLES);
	return &wrapped;
}

// --- host side ---

void audio_mux_host_rx(audio_mux_rx_t src)
{
	if (src != AUDIO_MUX_RX_OFF && !host_rx_on) {
		rx_rd = rx_wr;		// nothing stale from an earlier session
	}
	host_rx = src;
}

uint32_t audio_mux_host_avail(void)
{
	return rx_wr - rx_rd;
}

uint32_t audio_mux_host_read(int16_t *pcm, uint32_t n)
{
	uint32_t avail = rx_wr - rx_rd;
	__atomic_signal_fence(__ATOMIC_ACQUIRE);
	if (n > avail) {
		n = avail;
	}
	uint32_t r = rx_rd;
	for (uint32_t i = 0; i < n; i++) {
		pcm[i] = rx_ring[(r + i) & RX_MASK];
	}
	rx_rd = r + n;
	return n;
}

void audio_mux_host_push(const int16_t *pcm, uint32_t n)
{
	uint32_t room = PCM_SLIP_CAP - pcm_slip_fill(&slip);
	if (n > room) {
		n = room;	// never let the push move the read side under the consumer
	}
	pcm_slip_push(&slip, pcm, n);
	st.tx_pushes++;
}

void audio_mux_host_tx(bool on, bool dmr)
{
	if (on) {
		pcm_slip_reset(&slip);
		host_tx_dmr = dmr;
	} else {
		host_tx_dmr = false;
	}
}

uint32_t audio_mux_host_pushes(void)
{
	return st.tx_pushes;
}

uint32_t audio_mux_host_tx_fill(void)
{
	return pcm_slip_fill(&slip);
}

// --- radio task ---

void audio_mux_service(uint32_t now_ms)
{
	if (hw == NULL) {
		return;
	}
	if (host_rx_on && !running) {
		if (retry_armed && (now_ms - retry_ms) < AUDIO_MUX_RETRY_MS) {
			return;
		}
		st.starts++;
		if (!hw_start()) {
			st.start_fails++;
			retry_armed = true;
			retry_ms = now_ms;
		} else {
			retry_armed = false;
		}
	} else if (!host_rx_on && running && prim_cb == NULL) {
		running = false;
		hw->stop();
	}
}

// --- the board's FM transmit path ---

void audio_mux_fm_pull(int16_t *pcm, uint16_t n)
{
	pcm_slip_pull(&slip, pcm, n);
	st.fm_pulls += n;
}

void audio_mux_stats(audio_mux_stats_t *out)
{
	*out = st;
}

void audio_mux_slip_stats(pcm_slip_stats_t *out)
{
	*out = slip.stats;
}

void audio_mux_reset(void)
{
	hw = NULL;
	memset(&wrapped, 0, sizeof wrapped);
	prim_cb = NULL;
	prim_ctx = NULL;
	running = false;
	host_rx = AUDIO_MUX_RX_OFF;
	host_tx_dmr = false;
	retry_armed = false;
	rx_wr = rx_rd = 0;
	memset(&st, 0, sizeof st);
}
