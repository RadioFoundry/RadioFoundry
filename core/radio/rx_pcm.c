// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// FM receive audio for the core's decoders (rx_pcm.h)
#include "config.h"
#if CONFIG_APRS_RX
#include <string.h>
#include "radio/rx_pcm.h"
#include "board.h"
#include "hal_afsk.h"
#include "hal_audio_tap.h"
#include "usbaudio/audio_mux.h"

#define RX_PCM_SINKS	2u

static struct {
	uint8_t want;			// decoders that want the audio (rx_pcm_want)
	rx_pcm_sink_t sink[RX_PCM_SINKS];
	void *ctx[RX_PCM_SINKS];
	bool running;			// the path is started
	bool own;			// on the board's own path (board.afsk), else the stream
	bool retry_wait;		// the last start was refused: wait for retry_ms
	uint32_t retry_ms;
	uint32_t start_fails;
	int16_t buf[160];
} rx RFOUNDRY_CPU_RAM;	// CPU only: buf is read into by a copy, as when it was APRS's (aprs_s)

static bool by_board(void)
{
	const hal_afsk_t *h = board.afsk;
	return h != NULL && h->rx_start != NULL && h->rx_read != NULL;
}

static bool by_stream(void)
{
	return board.audio != NULL && board.audio_tap != NULL && (board.audio_tap->caps & AUDIO_TAP_FM_RX) != 0;
}

bool rx_pcm_can(void)
{
	return by_board() || by_stream();
}

uint32_t rx_pcm_rate(void)
{
	return by_board() ? board.afsk->rx_rate_hz : 8000u;
}

static uint8_t sink_index(uint8_t who)
{
	return who == RX_PCM_APRS ? 0u : 1u;
}

bool rx_pcm_want(uint8_t who, bool want, rx_pcm_sink_t sink, void *ctx)
{
	bool was = (rx.want & who) != 0;
	if (want) {
		rx.want |= who;
		rx.sink[sink_index(who)] = sink;
		rx.ctx[sink_index(who)] = ctx;
	} else {
		rx.want &= (uint8_t)~who;
	}
	return want && !was;
}

static void stop(void)
{
	if (rx.own) {
		if (board.afsk->rx_stop != NULL) {
			board.afsk->rx_stop();
		}
	} else {
		audio_mux_aprs_rx(false);
	}
	rx.running = false;
}

void rx_pcm_release(void)
{
	rx.want = 0;
	if (rx.running) {
		stop();
	}
}

void rx_pcm_service(uint32_t now_ms)
{
	bool want = rx.want != 0 && rx_pcm_can();
	// The wait only after a refusal: an old retry_ms compared 24.8 days later would read as in the future
	if (want && !rx.running && (!rx.retry_wait || (int32_t)(now_ms - rx.retry_ms) >= 0)) {
		rx.own = by_board();
		if (rx.own) {
			rx.running = board.afsk->rx_start();
			rx.retry_wait = !rx.running;
			if (!rx.running) {
				rx.start_fails++;
				rx.retry_ms = now_ms + RX_PCM_RETRY_MS;
			}
		} else {
			audio_mux_aprs_rx(true);
			rx.running = true;
		}
	} else if (!want && rx.running) {
		stop();
	}
	if (!want) {
		rx.retry_wait = false;	// a refusal's wait is for the try it followed, not a later one
	}
	if (!rx.running) {
		return;
	}
	uint16_t n = (uint16_t)(sizeof(rx.buf) / sizeof(rx.buf[0]));
	uint16_t k;
	while ((k = rx.own ? board.afsk->rx_read(rx.buf, n) : (uint16_t)audio_mux_aprs_read(rx.buf, n)) != 0) {
		for (uint8_t i = 0; i < RX_PCM_SINKS; i++) {
			if ((rx.want & (1u << i)) != 0 && rx.sink[i] != NULL) {
				rx.sink[i](rx.ctx[i], rx.buf, k, now_ms);
			}
		}
	}
}

bool rx_pcm_running(void)
{
	return rx.running;
}

bool rx_pcm_feeding(uint8_t who)
{
	return rx.running && (rx.want & who) != 0;
}

uint32_t rx_pcm_start_fails(void)
{
	return rx.start_fails;
}

void rx_pcm_init(void)
{
	memset(&rx, 0, sizeof(rx));
}
#endif
