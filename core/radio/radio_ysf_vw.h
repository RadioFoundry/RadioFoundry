// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Fusion's Voice FR (VW) codec in the radio core (CONFIG_YSF_VW; docs/spec/ysf-modem.md 4.1, 5): the full-rate decoder
// and encoder of core/mbe, the P25 Phase 1 vocoder, in integers on the CPU. radio_ysf.c runs them in its audio block
// callback where a DN over runs the board's AMBE+2 vocoder, so VW needs no codec in flash.
//
// The memory, about 4 KB (radio_ysf_vw_mem_t), is the DMR vocoder's lent state block while a VW over runs, where the
// platform lends one, else the radio core's mode area beside Fusion's engine (radio_ysf.h RADIO_YSF_VW_IN_AREA).
// Fusion runs DN on that vocoder and a channel may hear DN at any moment, so the block is taken per over, not per
// channel: radio_ysf.c binds it at a VW over's first frame and unbinds it before the vocoder's next call. A radio
// transmits or receives, so the encoder works in the decoder's prediction state and scratch (as P25 voice does); only
// the estimator's history is its own.
//
// Threading: everything here runs in the board's audio task (radio_ysf.c's audio block callback). No RTOS call.
#ifndef RADIO_YSF_VW_H
#define RADIO_YSF_VW_H

#include <stdint.h>
#include "config.h"
#include "ysf/ysf_frame.h"
#if CONFIG_YSF_VW
#include "mbe/imbe.h"
#include "mbe/mbe_est.h"
#endif

#if CONFIG_YSF_VW
// 3,964 B on a 32-bit target: the estimator's history 516, the decoder's prediction and channel state 132, a frame's
// parameters 176 (scratch), the synthesis core's state 516, its last frame and the frame being made 288 each, its
// scratch 2,048
typedef struct {
	mbe_est_state_t est;
	imbe_dec_state_t st;
	mbe_params_t pr;
	mbe_state_t ms;
	mbe_frame_t prev, cur;
	int32_t buf[MBE_BUF_WORDS];
} radio_ysf_vw_mem_t;

// The memory the calls below use, from now on (radio_ysf.c, at its start); NULL at its stop: the calls then play
// silence and send the silence frame
void radio_ysf_vw_bind(radio_ysf_vw_mem_t *m);
// A received over starts: the decoder from a fresh state (also after a transmission, whose encoder used its state)
void radio_ysf_vw_rx_start(void);
// One frame (88 bits, the corrections ysf_vw_frame_decode packed) to 160 samples. A frame its rules don't trust, and
// up to four after it, repeat the last good one, then the output mutes. Returns the bad frames in a row, 0 for a good
// one
int radio_ysf_vw_decode(const uint8_t imbe[YSF_VW_IMBE_BYTES], uint8_t errs, int16_t pcm[160]);
// A block with no frame in time during an over: concealed as a lost frame
void radio_ysf_vw_lost(int16_t pcm[160]);
// A transmission starts: the encoder from a fresh state
void radio_ysf_vw_tx_start(void);
// 160 mic samples to a frame's 88 bits
void radio_ysf_vw_encode(const int16_t pcm[160], uint8_t imbe[YSF_VW_IMBE_BYTES]);
// The full-rate frame of silence (pitch code 216), what the transmitter sends with no frame ready
extern const uint8_t *const radio_ysf_vw_silence;
#endif

#endif
