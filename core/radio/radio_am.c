// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Air band AM receive helpers (radio_am.h; docs/spec/airband-and-broadcast.md 3)
#include "board.h"
#include "radio/radio_am.h"
#include "config.h"

bool radio_am_band(uint32_t hz)
{
	return hz >= RADIO_AM_LO_HZ && hz < RADIO_AM_HI_HZ;
}

bool radio_am_capable(void)
{
	return CONFIG_AM_RX && (board.caps & CAP_AM_RX) != 0 && board.radio != NULL && board.radio->rx_am != NULL;
}

bool radio_am_channel(const channel_t *ch)
{
	return radio_am_capable() && ch->mode == RADIO_MODE_FM && radio_am_band(ch->rx_hz);
}

bool radio_am_tx_refused(uint32_t hz)
{
	return radio_am_capable() && radio_am_band(hz);
}

uint8_t radio_am_noise(int16_t rssi_dbm)
{
	int32_t n = RADIO_AM_SQL_REF_DBM - (int32_t)rssi_dbm;
	if (n < 0) {
		return 0;
	}
	return n > 255 ? 255 : (uint8_t)n;
}

uint32_t radio_am_step(uint32_t hz, int dir, uint32_t step_hz)
{
	if (step_hz != RADIO_AM_STEP_833) {
		int64_t next = (int64_t)hz + (int64_t)dir * step_hz;
		return next < 0 ? 0 : next > (int64_t)UINT32_MAX ? UINT32_MAX : (uint32_t)next;
	}
	// Grid point n is n x 25000 / 3 Hz: snap to the nearest, move one, round to the Hz
	int64_t n = ((int64_t)hz * 3 + 12500) / 25000;
	n += dir;
	if (n < 0) {
		n = 0;
	}
	return (uint32_t)((n * 25000 + 1) / 3);
}
