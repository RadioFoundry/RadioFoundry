// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Which mode a frequency takes: the one rule the radio core and the channel editor share
// (docs/spec/airband-and-broadcast.md 2, docs/spec/ui.md 2.1). Pure, the board descriptor aside.
#ifndef RADIO_BANDS_H
#define RADIO_BANDS_H

#include <stdbool.h>
#include <stdint.h>
#include "config.h"
#include "radio/radio_am.h"
#include "radio/radio_bcast.h"

// Listen-only bands (channels must be RX-only): air band (on a radio with AM RX) and the broadcast receiver's range
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

#endif
