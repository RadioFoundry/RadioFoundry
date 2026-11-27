// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// D-STAR DV on a board's flat FM audio taps (docs/spec/dstar-modem.md 5): the receive side (discriminator samples ->
// GMSK demodulator -> deframer -> header, voice frames, message, end) and the transmit side (header, message and
// voice frames -> bit stream -> GMSK modulator -> samples a board pulls into its modulator, hal_audio_tap_t's
// AUDIO_TAP_FLAT). Half duplex: one side at a time, sharing their state. The voice codec stays outside: frames go
// to and come from the caller (dstar_voice.h). No RTOS, no board; the radio core calls it from the task that owns
// the audio taps.
#ifndef RFOUNDRY_DSTAR_ENGINE_H
#define RFOUNDRY_DSTAR_ENGINE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "dstar/dstar_frame.h"
#include "dstar/dstar_gmsk.h"

// A board's paths, from its calibration (dstar-modem.md 5.2)
typedef struct {
	uint32_t rate;		// sample rate both ways (8000 on the PCM taps)
	int16_t tx_amp;		// the sample value that deviates 1200 Hz
	bool tx_invert, rx_invert;
	uint16_t tx_hpf_hz;	// a first-order high-pass in the transmit path to restore (0: flat)
	uint16_t rx_hpf_hz;	// and in the receive path
	uint16_t lfr_floor_hz;	// the restorers' floor (the headroom they cost is hpf / floor at DC)
} dstar_path_t;

typedef enum {
	DSTAR_ENGINE_IDLE = 0,
	DSTAR_ENGINE_RX,
	DSTAR_ENGINE_TX,
} dstar_engine_mode_t;

typedef struct {
	dstar_engine_mode_t mode;
	dstar_path_t path;
	union {
		struct {
			dstar_gmsk_demod_t dm;
			dstar_rx_t fr;
		} rx;
		struct {
			dstar_gmsk_mod_t mod;
			dstar_tx_t fr;
			bool done;
		} tx;
	} u;
} dstar_engine_t;

// Start listening: cb gets what the deframer finds
bool dstar_engine_rx_start(dstar_engine_t *e, const dstar_path_t *path, const dstar_rx_cb_t *cb);
// Received samples (in RX)
void dstar_engine_rx(dstar_engine_t *e, const int16_t *s, size_t n);

// Start a transmission: the header, an optional message, the preamble in bits, voice frames from pull (NULL: the
// standard's silence frame until dstar_engine_tx_stop)
bool dstar_engine_tx_start(dstar_engine_t *e, const dstar_path_t *path, const dstar_header_t *h, const char *msg,
			   uint16_t preamble_bits, dstar_voice_pull_t pull, void *ctx);
// n samples of modulation (hal_audio_tap's pull fits); after the end pattern, zeros. Returns false once the
// transmission's last sample has gone (the radio can unkey)
bool dstar_engine_tx(dstar_engine_t *e, int16_t *pcm, uint16_t n);
// End a transmission with no voice source: the end pattern follows the current frame
void dstar_engine_tx_stop(dstar_engine_t *e);

void dstar_engine_idle(dstar_engine_t *e);

#endif
