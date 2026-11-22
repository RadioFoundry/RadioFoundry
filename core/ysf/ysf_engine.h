// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Yaesu System Fusion for a board (docs/spec/ysf-modem.md 5): the receiver and transmitter of ysf_frame.h joined to
// either of the two ways a radio can carry C4FM:
//   - a flat FM audio path (hal_audio_tap_t's AUDIO_TAP_FLAT): discriminator samples into the software demodulator,
//     the software modulator's samples into the FM modulator (ysf_c4fm.h)
//   - a modem chip that sends and receives 4FSK symbols itself at 4800 symbols/s with a root raised cosine pulse
//     (the DMR modems' physical layer mode): dibits straight out in the chip's 144-symbol blocks, and the chip's
//     decided dibits in
// Half duplex: one side at a time, sharing their state. The voice codec stays outside: 49-bit frames go to and come
// from the caller. No RTOS, no board; the radio core calls it from the task that owns the audio or the modem.
#ifndef RFOUNDRY_YSF_ENGINE_H
#define RFOUNDRY_YSF_ENGINE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "ysf/ysf_frame.h"
#include "ysf/ysf_c4fm.h"

// A board's paths, from its calibration (ysf-modem.md 5.2)
typedef struct {
	uint32_t rate;		// sample rate of the audio path both ways (8000 on the PCM taps); unused on a symbol path
	int16_t tx_amp;		// the sample value of a +1 symbol (900 Hz wide)
	bool tx_invert, rx_invert;
	uint16_t tx_hpf_hz;	// a first-order high-pass in the transmit audio path to undo ahead of it (0: flat)
	uint16_t rx_hpf_hz;	// and in the receive audio path, after it
	uint16_t lfr_floor_hz;	// the restorer's floor (0: YSF_LFR_FLOOR_HZ); the headroom it costs is hpf / floor
} ysf_path_t;

typedef enum {
	YSF_ENGINE_IDLE = 0,
	YSF_ENGINE_RX,
	YSF_ENGINE_TX,
} ysf_engine_mode_t;

// The modem chips' physical layer block: 144 symbols (30 ms), four dibits a byte, the first in bits 7-6
#define YSF_PHY_BLOCK_SYMS	144u
#define YSF_PHY_BLOCK_BYTES	36u

typedef struct {
	ysf_engine_mode_t mode;
	ysf_path_t path;
	union {
		struct {
			ysf_c4fm_demod_t dm;
			ysf_rx_t fr;
		} rx;
		struct {
			ysf_c4fm_mod_t mod;
			ysf_tx_t fr;
			ysf_lfr_t lfr;	// the transmit path's high-pass undone (a wire when tx_hpf_hz is 0)
			bool done;
		} tx;
	} u;
} ysf_engine_t;

// Start listening: cb gets what the receiver finds. False when the path's rate is out of range
bool ysf_engine_rx_start(ysf_engine_t *e, const ysf_path_t *path, const ysf_rx_cb_t *cb);
// Where VW voice goes (ysf_rx_voice_fr), after ysf_engine_rx_start
static inline void ysf_engine_rx_voice_fr(ysf_engine_t *e, ysf_voice_fr_t fn)
{
	if (e->mode == YSF_ENGINE_RX) {
		ysf_rx_voice_fr(&e->u.rx.fr, fn);
	}
}
// Received discriminator samples (in RX)
void ysf_engine_rx(ysf_engine_t *e, const int16_t *s, size_t n);
// Dibits a modem chip decided (in RX), one per byte
void ysf_engine_rx_dibits(ysf_engine_t *e, const uint8_t *d, size_t n);
// The receiver's state and counters (for the screens and the debug link)
static inline const ysf_rx_t *ysf_engine_receiver(const ysf_engine_t *e)
{
	return e->mode == YSF_ENGINE_RX ? &e->u.rx.fr : NULL;
}

// Start a transmission (cfg: callsigns, FICH fields, the voice source)
bool ysf_engine_tx_start(ysf_engine_t *e, const ysf_path_t *path, const ysf_tx_config_t *cfg);
// n samples of modulation (hal_audio_tap's pull fits); after the terminator, zeros. Returns false once the
// transmission's last sample has gone (the radio can unkey)
bool ysf_engine_tx(ysf_engine_t *e, int16_t *pcm, uint16_t n);
// The next 144 symbols for a modem chip's physical layer. Returns false once the terminator has gone; the block that
// carries its end is padded with +1 / -1 pairs and still returns true
bool ysf_engine_tx_block(ysf_engine_t *e, uint8_t raw[YSF_PHY_BLOCK_BYTES]);
// End a transmission: the frame being sent is finished, then the terminator
void ysf_engine_tx_stop(ysf_engine_t *e);

void ysf_engine_idle(ysf_engine_t *e);

#endif
