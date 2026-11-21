// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// AM receive (CONFIG_AM_RX; docs/spec/airband-and-broadcast.md 3). Two ways in, on a radio that can (CAP_AM_RX and
// board.radio->rx_am): an FM channel or the VFO tuned inside the aeronautical band is received in AM, as it always
// was; and a channel of the AM kind (codeplug kind 5, docs/spec/channel-modes.md 4), or the VFO set to AM, is received
// in AM wherever the board's receiver demodulates it (board_t.am_bands). AM never transmits. This is the AM kind's
// mode module: the rest of the core asks these helpers, never the kind id. The helpers are pure (the board descriptor
// aside) so the radio core and its tests share them
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

// hz is in the air band
bool radio_am_band(uint32_t hz);
// This radio receives AM (CAP_AM_RX and board.radio->rx_am)
bool radio_am_capable(void);
// ... and outside the air band too (board_t.am_bands): the AM mode is offered for channels and the VFO
bool radio_am_anywhere(void);
// The receiver demodulates AM at hz: the air band, or one of board_t.am_bands that the band table receives now
bool radio_am_rx_at(uint32_t hz);
// The channel (or the VFO) is set to AM: the AM kind, whether or not this radio can receive it where it is
bool radio_am_kind(const channel_t *ch);
// The channel (or the VFO's settings) is received in AM here: an FM channel in the air band, or one set to AM where
// the receiver demodulates AM, on a radio that can
bool radio_am_channel(const channel_t *ch);
// A channel read from the codeplug, as the radio runs it: one set to AM is FM underneath (its base record is the safe
// stand-in, channel-modes.md 2.3); the AM demodulator comes from radio_am_channel. Others are left alone
void radio_am_take(channel_t *ch);
// Set the channel (the VFO) to AM, or back to plain FM
void radio_am_set(channel_t *ch, bool on);
// Write ch at channel number as a channel set to AM (radio_am_cp.c, on the task that writes the codeplug): its record
// in the stand-in shape, then its tag, kept where one of the kind followed the record. False when it can't be written
bool radio_am_save(uint16_t number, const channel_t *ch);
// Transmitting the channel is refused: it is set to AM. AM is receive only on every radio so far: none has an AM
// modulator, and where it would be lawful (some amateur bands) is a question for a later change
bool radio_am_tx_never(const channel_t *ch);
// Transmitting on hz is refused because it is in the air band of a radio that receives AM there
bool radio_am_tx_refused(uint32_t hz);
// One VFO step from hz in dir with step_hz. The 8.33 kHz step moves on the exact 25/3 kHz grid (rounded to the Hz,
// snapping an off-grid frequency to the nearest grid point first); other steps add step_hz
uint32_t radio_am_step(uint32_t hz, int dir, uint32_t step_hz);

#endif
