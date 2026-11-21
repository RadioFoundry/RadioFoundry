// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// AM receive helpers, the AM kind's mode module (radio_am.h; docs/spec/airband-and-broadcast.md 3)
#include "board.h"
#include "radio/radio_am.h"
#include "codeplug/codeplug_tag.h"
#include "config.h"

bool radio_am_band(uint32_t hz)
{
	return hz >= RADIO_AM_LO_HZ && hz < RADIO_AM_HI_HZ;
}

bool radio_am_capable(void)
{
	return CONFIG_AM_RX && (board.caps & CAP_AM_RX) != 0 && board.radio != NULL && board.radio->rx_am != NULL;
}

bool radio_am_anywhere(void)
{
	return radio_am_capable() && board.am_bands != NULL && board.am_band_count != 0u;
}

bool radio_am_rx_at(uint32_t hz)
{
	if (!radio_am_capable()) {
		return false;
	}
	if (radio_am_band(hz)) {
		return true;
	}
	if (board.am_bands == NULL || !board_in_band(hz, false)) {
		return false;
	}
	for (uint8_t i = 0; i < board.am_band_count; i++) {
		if (hz >= board.am_bands[i].lo_hz && hz < board.am_bands[i].hi_hz) {
			return true;
		}
	}
	return false;
}

bool radio_am_kind(const channel_t *ch)
{
	return ch->kind == CODEPLUG_KIND_AM;
}

bool radio_am_channel(const channel_t *ch)
{
	if (radio_am_kind(ch)) {
		return radio_am_rx_at(ch->rx_hz);	// the mode word is the kind's: its stand-in DMR doesn't count
	}
	return radio_am_capable() && ch->mode == RADIO_MODE_FM && radio_am_band(ch->rx_hz);
}

void radio_am_take(channel_t *ch)
{
	if (radio_am_kind(ch)) {
		ch->mode = RADIO_MODE_FM;
	}
}

void radio_am_set(channel_t *ch, bool on)
{
	ch->kind = on ? (uint8_t)CODEPLUG_KIND_AM : (uint8_t)CODEPLUG_KIND_NONE;
	ch->system = CODEPLUG_SYSTEM_NONE;	// a plain channel and an AM one have none
	ch->mode = RADIO_MODE_FM;
}

bool radio_am_tx_never(const channel_t *ch)
{
	return radio_am_kind(ch);
}

bool radio_am_tx_refused(uint32_t hz)
{
	return radio_am_capable() && radio_am_band(hz);
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
