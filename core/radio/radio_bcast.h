// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Broadcast FM receive (CONFIG_BCAST_FM; docs/spec/airband-and-broadcast.md 4). Like the air band, it is not stored in
// the codeplug: an FM channel or the VFO tuned inside the broadcast receiver's range (board.bcast, CAP_BCAST_FM) is
// heard on that receiver instead of the channel receiver, and never transmits. The helpers are pure (the board
// descriptor aside) so the radio core and its tests share them.
#ifndef RADIO_BCAST_H
#define RADIO_BCAST_H

#include <stdint.h>
#include <stdbool.h>
#include "codeplug/codeplug.h"

// The broadcast band's 100 kHz points: every receiver's grid divides it, and a frequency typed in may be any of them
#define RADIO_BCAST_STEP_HZ	100000u

// The squelch works on the receiver's signal strength, turned into the FM noise scale (radio_sm_squelch_open_below:
// larger = quieter): noise = RADIO_BCAST_SQL_REF - rssi, rssi in the receiver's units (about dBuV). The default level
// 5 then opens above 45 dBuV, level 1 above 33, level 9 above 57. Set from a receiver's readings on empty frequencies
// (22 to 37) and on stations (50 and up), without an antenna
#define RADIO_BCAST_SQL_REF	97
// dBm from the receiver's units for the signal meter: 0 dBuV across 50 ohms is -107 dBm
#define RADIO_BCAST_DBUV_DBM	(-107)
// The receiver's volume while it plays when the radio has no volume of its own to follow (an analog knob after the
// amplifier): near the top of hal_bcast.h's scale, leaving the knob the control
#define RADIO_BCAST_VOLUME_FIXED	12

// This radio has a broadcast receiver the core may use (CAP_BCAST_FM and board.bcast)
bool radio_bcast_capable(void);
// hz is in the broadcast receiver's range on this radio (false when there is none)
bool radio_bcast_band(uint32_t hz);
// The channel (or the VFO's settings) is heard on the broadcast receiver: an FM channel in its range
bool radio_bcast_channel(const channel_t *ch);
// Transmitting on hz is refused because it is in the broadcast receiver's range
bool radio_bcast_tx_refused(uint32_t hz);
// The VFO's automatic step in the broadcast band for radio_settings_t.bcast_step
uint32_t radio_bcast_step_hz(uint8_t setting);
// The VFO's next frequency from hz, dir -1 or 1, for radio_settings_t.bcast_step. RADIO_BCAST_STEP_200_ODD goes to the
// next odd tenth of a MHz (90.2 up is 90.3, 90.1 up is 90.3), so a frequency typed in between gets back on the grid
uint32_t radio_bcast_step(uint32_t hz, int dir, uint8_t setting);
// hz on the receiver's grid, in kHz (rounded to the nearest grid point)
uint32_t radio_bcast_khz(uint32_t hz);
// The squelch's noise-scale value for a receiver RSSI (0 = strongest .. 255)
uint8_t radio_bcast_noise(uint8_t rssi);
// The receiver's volume (0..BCAST_VOLUME_MAX) for a radio volume in percent; < 0 = the radio sets none
uint8_t radio_bcast_volume(int percent);

#endif
