// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The squelch's reading for the radio core (hal_radio.h noise), from the calibration page's thresholds
// (hardware-db25d.md 6.4).
//
// The main chip sits behind a narrow IF filter, so its own noise reading stays low with no signal and doesn't tell an
// empty channel from a carrier on the core's scale: the squelch stayed open. The calibration page holds an open
// threshold in dBm for each squelch level and frequency segment. The board turns the signal strength into a reading on
// the core's scale that crosses each level's line at that level's threshold, so the core's one squelch opens where the
// page says. The FD6818 also needs its noise and glitch readings low (its own limits), as its gate does.
#ifndef SQUELCH_DB25D_H
#define SQUELCH_DB25D_H

#include <stdbool.h>
#include <stdint.h>

// The core's squelch scale (radio_sm.h): level n opens below DB25D_SQL_OPEN_L1 - (n - 1) * DB25D_SQL_STEP
#define DB25D_SQL_OPEN_L1	64
#define DB25D_SQL_STEP		3
#define DB25D_SQL_LEVELS	9

// open_dbm: the open threshold of levels 1..9 (index 0..8), each at or above the one before. rssi_dbm: the reading.
// The result is below level n's line exactly when rssi_dbm >= open_dbm[n - 1]; under level 1's threshold it rises a
// unit per dB, over level 9's it falls a unit per dB
uint8_t db25d_sql_noise(const int16_t open_dbm[DB25D_SQL_LEVELS], int16_t rssi_dbm);

// The FD6818's own noise and glitch limits for a carrier (hardware-db25d.md 6.4): false = no carrier, whatever the
// signal strength. noise: register 0x65 bits 6:0; glitch: register 0x6A; wide: a 25 kHz channel
bool db25d_sql_fd6818_carrier(uint8_t noise, uint16_t glitch, int16_t rssi_dbm, bool wide);

#endif
