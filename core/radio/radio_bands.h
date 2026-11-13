// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Which mode a frequency takes: the one rule the radio core and the channel editor share
// (docs/spec/airband-and-broadcast.md 2, docs/spec/ui.md 2.1). Pure, the board descriptor aside.
#ifndef RADIO_BANDS_H
#define RADIO_BANDS_H

#include <stdbool.h>
#include <stdint.h>
#include "board.h"
#include "config.h"
#include "radio/radio_am.h"
#include "radio/radio_bcast.h"

// The air band (on a radio with AM receive) and the broadcast receiver's range: receive only, and never DMR
static inline bool radio_listen_band(uint32_t hz)
{
#if CONFIG_AM_RX
	if (radio_am_tx_refused(hz))
		return true;
#endif
#if CONFIG_BCAST_FM
	if (radio_bcast_band(hz))
		return true;
#endif
	(void)hz;
	return false;
}

// DMR may be received on hz: any band of the table but a listen band, as FM. Where the radio can't transmit, a channel
// must also be RX only, which the TX checks ask for
static inline bool radio_dmr_band(uint32_t hz)
{
	return board_in_band(hz, false) && !radio_listen_band(hz);
}

#endif
