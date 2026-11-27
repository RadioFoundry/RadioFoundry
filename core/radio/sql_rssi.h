// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A squelch reading from signal strength, for a radio whose calibration holds an open threshold in dBm for each squelch
// level (the core's levels: radio_sm.h).
//
// Some receive chips sit behind a narrow IF filter, so their own noise reading stays low with no signal and doesn't
// tell an empty channel from a carrier on the core's scale: the squelch stays open. Such a board reports, as
// radio_status_t.noise, the signal strength turned onto the core's scale so that the reading crosses each level's line
// (radio_sm_squelch_open_below) exactly at that level's threshold: the core's one squelch opens where the calibration
// says.
//
// The core closes a level RADIO_SQL_HYST_DB over its line, which is the next lower level's line: a level closes where
// the one below opens, so its hysteresis is the calibration's step between the two (radio_sql_rssi_levels keeps that
// at 1 dB or more). Level 1 closes 3 dB under its threshold.
#ifndef RADIO_SQL_RSSI_H
#define RADIO_SQL_RSSI_H

#include <stdbool.h>
#include <stdint.h>
#include "radio/radio_sm.h"

// No carrier (a chip's own carrier test failed): over every level's close line, level 1's too
#define RADIO_SQL_NO_CARRIER	(RADIO_SQL_OPEN_L1 + RADIO_SQL_HYST_DB)

// The open thresholds of levels 1..9 (index 0..8) as read: false if a tighter level opens on less signal (not
// thresholds). Equal neighbors are moved 1 dB apart, so every level closes under its open point
bool radio_sql_rssi_levels(int16_t open_dbm[RADIO_SQUELCH_MAX]);

// The reading for rssi_dbm against thresholds radio_sql_rssi_levels accepted: under level n's line exactly when
// rssi_dbm >= open_dbm[n - 1]. Under level 1's threshold it rises a unit per dB, over level 9's it falls a unit per dB
uint8_t radio_sql_rssi_noise(const int16_t open_dbm[RADIO_SQUELCH_MAX], int16_t rssi_dbm);

#endif
