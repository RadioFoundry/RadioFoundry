// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Broadcast FM receive helpers (radio_bcast.h; docs/spec/airband-and-broadcast.md 4)
#include "board.h"
#include "radio/radio_bcast.h"
#include "radio/radio.h"
#include "config.h"

bool radio_bcast_capable(void)
{
	return CONFIG_BCAST_FM && (board.caps & CAP_BCAST_FM) != 0 && board.bcast != NULL;
}

bool radio_bcast_band(uint32_t hz)
{
	if (!radio_bcast_capable()) {
		return false;
	}
	uint32_t khz = hz / 1000u;
	return khz >= board.bcast->lo_khz && khz <= board.bcast->hi_khz;
}

bool radio_bcast_channel(const channel_t *ch)
{
	return ch->mode == RADIO_MODE_FM && radio_bcast_band(ch->rx_hz);
}

bool radio_bcast_tx_refused(uint32_t hz)
{
	return radio_bcast_band(hz);
}

uint32_t radio_bcast_step_hz(uint8_t setting)
{
	return setting == RADIO_BCAST_STEP_100 ? RADIO_BCAST_STEP_HZ : 2u * RADIO_BCAST_STEP_HZ;
}

uint32_t radio_bcast_step(uint32_t hz, int dir, uint8_t setting)
{
	if (setting == RADIO_BCAST_STEP_100) {
		int64_t next = (int64_t)hz + (int64_t)dir * RADIO_BCAST_STEP_HZ;
		return next < 0 ? 0 : next > (int64_t)UINT32_MAX ? UINT32_MAX : (uint32_t)next;
	}
	// In 100 kHz points: the next point past hz in that direction, then on to an odd one
	int64_t n = dir > 0 ? (int64_t)(hz / RADIO_BCAST_STEP_HZ) + 1 :
			     (int64_t)((hz + RADIO_BCAST_STEP_HZ - 1u) / RADIO_BCAST_STEP_HZ) - 1;
	if (n % 2 == 0) {
		n += dir > 0 ? 1 : -1;
	}
	int64_t next = n * RADIO_BCAST_STEP_HZ;
	return next < 0 ? 0 : next > (int64_t)UINT32_MAX ? UINT32_MAX : (uint32_t)next;
}

uint32_t radio_bcast_khz(uint32_t hz)
{
	uint32_t grid = board.bcast != NULL && board.bcast->grid_khz != 0 ? board.bcast->grid_khz : 1u;
	uint32_t grid_hz = grid * 1000u;
	return (hz + grid_hz / 2u) / grid_hz * grid;
}

uint8_t radio_bcast_noise(uint8_t rssi)
{
	int n = RADIO_BCAST_SQL_REF - (int)rssi;
	return n < 0 ? 0 : (uint8_t)n;
}

uint8_t radio_bcast_volume(int percent)
{
	if (percent < 0) {
		return RADIO_BCAST_VOLUME_FIXED;
	}
	if (percent > 100) {
		percent = 100;
	}
	return (uint8_t)((percent * BCAST_VOLUME_MAX + 50) / 100);
}
