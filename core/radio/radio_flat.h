// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The flat FM paths' calibration as the modes on them use it (docs/spec/dstar-modem.md 5.2): the
// board's own (hal_audio_tap_t.flat_cal) with the bench's settings over it (settings_t flat_*: the deviation scale, each
// polarity, the high-pass to restore each way; 0 or unset = the board's).
//
// One function for every mode on the paths, since the calibration is the paths', not a mode's: a scale the bench
// measured once with D-STAR holds for any other data mode on them.
#ifndef RADIO_FLAT_H
#define RADIO_FLAT_H

#include "config.h"
#include "hal_audio_tap.h"

#if CONFIG_FM_FLAT
// tap: the board's (NULL: none, all zeros)
void radio_flat_cal(const hal_audio_tap_t *tap, audio_tap_flat_cal_t *out);
#endif

#endif
