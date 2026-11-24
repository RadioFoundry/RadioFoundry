// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// FM receive audio as PCM for the core's decoders (APRS's AFSK demodulator, the DTMF decoder): one receive path, read
// once and handed to every decoder that wants it. The path is the board's own (board.afsk rx_start / rx_read /
// rx_stop, at rx_rate_hz), else FM receive audio on the PCM stream (AUDIO_TAP_FM_RX, 8 kHz, through audio_mux). Both
// have a single, destructive reader, which is this module. Radio task only. Built with CONFIG_APRS_RX, which brings
// the receive paths (docs/spec/aprs.md 5.3)
#ifndef RFOUNDRY_RX_PCM_H
#define RFOUNDRY_RX_PCM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "config.h"

// The decoders, one bit each
enum {
	RX_PCM_APRS = 1u << 0,
	RX_PCM_DTMF = 1u << 1,
};

// A decoder's samples: n at rx_pcm_rate(), from the radio task; ctx as given to rx_pcm_want
typedef void (*rx_pcm_sink_t)(void *ctx, const int16_t *s, size_t n, uint32_t now_ms);

// A start the board refused (no memory, its audio path busy) is tried again this much later, not on every poll: each
// try routes the receiver and back
#define RX_PCM_RETRY_MS	1000u

#if CONFIG_APRS_RX
// At start-up (aprs_init): nothing wanted, nothing running, as at power-up
void rx_pcm_init(void);
// The board has a receive path
bool rx_pcm_can(void);
// Its sample rate (the board's rx_rate_hz, or the stream's 8 kHz)
uint32_t rx_pcm_rate(void);
// A decoder wants the audio now (true) or not (false), each poll. True back when it is newly wanted, i.e. the decoder
// should start fresh: it wasn't wanted on the last call, or rx_pcm_release dropped it since
bool rx_pcm_want(uint8_t who, bool want, rx_pcm_sink_t sink, void *ctx);
// Start or stop the path for the decoders that want it, and hand them what it received. Any decoder may call it after
// its rx_pcm_want: each call reads what has come in since the last
void rx_pcm_service(uint32_t now_ms);
// The path is running (for anyone; the receiver must stay awake)
bool rx_pcm_running(void);
// The path is running for this decoder
bool rx_pcm_feeding(uint8_t who);
// Stop the path now and drop every decoder's want (radio task): before the board changes mode or frequency. The
// path holds the board's FM receive audio (on the HR_C7000 the copy interrupt, which a P25, NXDN or YSF engine's
// air_take needs); a decoder that still wants it after the tune asks again on its next poll
void rx_pcm_release(void);
// Starts the board refused
uint32_t rx_pcm_start_fails(void);
#else
static inline bool rx_pcm_can(void) { return false; }
static inline bool rx_pcm_running(void) { return false; }
static inline void rx_pcm_release(void) {}
#endif

#endif
