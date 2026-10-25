// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Air band AM receive (CONFIG_AM_RX; docs/spec/airband-and-broadcast.md 3). AM is not stored in the codeplug: an FM
// channel or the VFO tuned inside the aeronautical band is received in AM on a radio that can (CAP_AM_RX and
// board.radio->rx_am), and it never transmits there. The helpers are pure (the board descriptor aside) so the radio
// core and its tests share them.
#ifndef RADIO_AM_H
#define RADIO_AM_H

#include <stdint.h>
#include <stdbool.h>
#include "codeplug/codeplug.h"

// The aeronautical VHF band (ITU Radio Regulations: 108-117.975 MHz radio navigation, 117.975-137 MHz aeronautical
// mobile), AM everywhere in the world. hi is exclusive
#define RADIO_AM_LO_HZ		108000000u
#define RADIO_AM_HI_HZ		137000000u

// Air band channel steps: 25 kHz, and the 8.33 kHz spacing (25 kHz / 3) used in Europe. RADIO_AM_STEP_833 is the
// step value that means "the 8.33 kHz grid" (radio_am_step keeps the frequency on the exact grid)
#define RADIO_AM_STEP_25K	25000u
#define RADIO_AM_STEP_833	8333u

// AM squelch: there is no useful noise reading in AM, so the squelch works on the signal strength. The RSSI is turned
// into the FM noise scale (radio_sm_squelch_open_below, 1 dB units, larger = quieter channel) so the same squelch
// levels and hysteresis apply: level 1 opens above -82 dBm, level 5 (the default) above -70 dBm, level 9 above
// -58 dBm. Set from a radio's readings on empty air band channels (-76 to -86 dBm): the RSSI the boards report in AM
// isn't calibrated, so level 5 sits about 8 dB above that floor
#define RADIO_AM_SQL_REF_DBM	(-18)

// hz is in the air band
bool radio_am_band(uint32_t hz);
// This radio receives AM (CAP_AM_RX and board.radio->rx_am)
bool radio_am_capable(void);
// The channel (or the VFO's settings) is received in AM here: an FM channel in the air band on a radio that can
bool radio_am_channel(const channel_t *ch);
// Transmitting on hz is refused because it is in the air band of a radio that receives AM there
bool radio_am_tx_refused(uint32_t hz);
// The squelch's noise-scale value for an RSSI in AM (0 = strongest .. 255)
uint8_t radio_am_noise(int16_t rssi_dbm);
// One VFO step from hz in dir with step_hz. The 8.33 kHz step moves on the exact 25/3 kHz grid (rounded to the Hz,
// snapping an off-grid frequency to the nearest grid point first); other steps add step_hz
uint32_t radio_am_step(uint32_t hz, int dir, uint32_t step_hz);

#endif
