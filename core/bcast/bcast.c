// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The broadcast FM receiver's behavior (bcast.h; docs/spec/airband-and-broadcast.md 4.3, 4.5)
#include <string.h>
#include "bcast/bcast.h"
#include "config.h"

static void changed(bcast_t *b)
{
	b->serial++;
}

static void save(bcast_t *b)
{
	b->dirty = true;
	changed(b);
}

static uint32_t grid(const bcast_t *b)
{
	return b->view.grid_khz != 0 ? b->view.grid_khz : 50u;
}

static bool in_range(const bcast_t *b, uint32_t khz)
{
	return khz >= b->view.lo_khz && khz <= b->view.hi_khz;
}

// Round to the receiver's grid
static uint32_t snap(const bcast_t *b, uint32_t khz)
{
	uint32_t g = grid(b);
	return (khz + g / 2) / g * g;
}

static void find_preset(bcast_t *b)
{
	b->view.preset = -1;
	for (int i = 0; i < BCAST_PRESETS; i++) {
		if (b->view.preset_khz[i] != 0 && b->view.preset_khz[i] == b->view.khz) {
			b->view.preset = (int8_t)i;
			break;
		}
	}
}

// Audio to the speaker only while on, not paused and not seeking (the sweep is noise)
static void audio(bcast_t *b)
{
	bool on = b->view.on && !b->view.paused && !b->view.seeking;
	if (b->hw != NULL && b->audio_sent != (int8_t)on) {
		b->audio_sent = (int8_t)on;
		b->hw->audio(on);
	}
}

static bool tune(bcast_t *b, uint32_t khz)
{
	if (!b->hw->tune(khz)) {
		return false;
	}
	b->view.khz = khz;
	b->view.tuning = true;
	b->view.seeking = false;
	b->view.seek_failed = false;
	b->view.station = b->view.stereo = false;
	find_preset(b);
	save(b);
	return true;
}

static bool power(bcast_t *b, bool on)
{
	if (on == b->view.on) {
		return true;
	}
	if (!on) {
		b->view.on = b->view.paused = b->view.tuning = b->view.seeking = false;
		audio(b);
		b->hw->power(false);
		save(b);
		return true;
	}
	if (!b->hw->power(true)) {
		b->view.failed = true;
		changed(b);
		return false;
	}
	b->view.failed = false;
	b->view.on = true;
	b->view.paused = b->quiet_ms == 0;	// the next poll lifts the pause once the channel has been quiet
	b->hw->volume(b->view.volume);
	if (!tune(b, b->view.khz)) {
		b->view.khz = snap(b, b->view.lo_khz + (b->view.hi_khz - b->view.lo_khz) / 2);
		tune(b, b->view.khz);
	}
	audio(b);
	save(b);
	return true;
}

void bcast_init(bcast_t *b, const hal_bcast_t *hw, const bcast_saved_t *saved, uint32_t now_ms)
{
	memset(b, 0, sizeof(*b));
	b->hw = hw;
	b->audio_sent = -1;
	b->view.preset = -1;
	b->view.volume = BCAST_VOLUME_DEFAULT;
	b->quiet_ms = now_ms != 0 ? now_ms : 1;
	if (hw == NULL) {
		return;
	}
	b->view.present = true;
	b->view.lo_khz = hw->lo_khz;
	b->view.hi_khz = hw->hi_khz;
	b->view.grid_khz = hw->grid_khz;
	b->view.khz = in_range(b, BCAST_DEFAULT_KHZ) ? BCAST_DEFAULT_KHZ : snap(b, hw->lo_khz);
	bool resume = false;
	if (saved != NULL) {
		if (saved->khz != 0 && in_range(b, snap(b, saved->khz))) {
			b->view.khz = snap(b, saved->khz);
		}
		if (saved->volume <= BCAST_VOLUME_MAX) {
			b->view.volume = saved->volume;
		}
		for (int i = 0; i < BCAST_PRESETS; i++) {
			uint32_t k = snap(b, saved->preset_khz[i]);
			b->view.preset_khz[i] = saved->preset_khz[i] != 0 && in_range(b, k) ? k : 0;
		}
		resume = saved->on;
	}
	find_preset(b);
	audio(b);
	if (resume) {
		power(b, true);
	}
	b->dirty = false;	// nothing new to store yet
}

bool bcast_request(bcast_t *b, uint8_t cmd, int32_t arg, uint32_t now_ms)
{
	(void)now_ms;
	if (b->hw == NULL) {
		return false;
	}
	if (cmd == BCMD_POWER) {
		return power(b, arg == 2 ? !b->view.on : arg != 0);
	}
	if (cmd == BCMD_PRESET_SAVE) {
		if (arg < 0 || arg >= BCAST_PRESETS) {
			return false;
		}
		b->view.preset_khz[arg] = b->view.khz;
		find_preset(b);
		save(b);
		return true;
	}
	if (cmd == BCMD_VOLUME || cmd == BCMD_VOLUME_STEP) {
		int32_t v = cmd == BCMD_VOLUME ? arg : (int32_t)b->view.volume + (arg > 0 ? 1 : -1);
		if (v < 0 || v > BCAST_VOLUME_MAX) {
			return false;
		}
		b->view.volume = (uint8_t)v;
		if (b->view.on) {
			b->hw->volume(b->view.volume);
		}
		save(b);
		return true;
	}
	if (!b->view.on) {
		return false;	// tuning needs the receiver on
	}
	switch (cmd) {
	case BCMD_TUNE: {
		uint32_t k = arg > 0 ? snap(b, (uint32_t)arg) : 0;
		return in_range(b, k) && tune(b, k);
	}
	case BCMD_STEP: {
		int32_t k = (int32_t)b->view.khz + (arg > 0 ? BCAST_STEP_KHZ : -BCAST_STEP_KHZ);
		uint32_t next = (uint32_t)k;
		if (k < (int32_t)b->view.lo_khz) {
			next = b->view.hi_khz;	// wrap round the range
		} else if (k > (int32_t)b->view.hi_khz) {
			next = b->view.lo_khz;
		}
		return tune(b, snap(b, next));
	}
	case BCMD_SEEK:
		if (!b->hw->seek(arg > 0)) {
			return false;
		}
		b->view.tuning = b->view.seeking = true;
		b->view.seek_failed = false;
		b->view.station = b->view.stereo = false;
		audio(b);
		changed(b);
		return true;
	case BCMD_PRESET:
		if (arg < 0 || arg >= BCAST_PRESETS || b->view.preset_khz[arg] == 0) {
			return false;
		}
		return tune(b, b->view.preset_khz[arg]);
	default:
		return false;
	}
}

bool bcast_channel_busy(const radio_view_t *v)
{
	if (v->state == RS_TX || v->state == RS_TX_DENIED) {
		return true;
	}
#if CONFIG_DMR
	if (v->ch.mode == RADIO_MODE_DMR && v->dmr.active) {
		// Calls we play and our own; a call on our slot we don't play (another talkgroup) doesn't interrupt
		return (v->dmr.state != RDMR_IDLE && v->dmr.state != RDMR_RX_OTHER) || v->dmr.reply_active;
	}
#endif
	return v->state == RS_RX || v->monitor;
}

void bcast_poll(bcast_t *b, bool channel_busy, bool hotspot, uint32_t now_ms)
{
	if (b->hw == NULL) {
		return;
	}
	if (channel_busy) {
		b->quiet_ms = 0;
	} else if (b->quiet_ms == 0) {
		b->quiet_ms = now_ms != 0 ? now_ms : 1;
	}
	if (!b->view.on) {
		return;
	}
	if (hotspot) {
		power(b, false);	// the host link drives the radio now
		return;
	}
	bool pause = b->quiet_ms == 0 || now_ms - b->quiet_ms < BCAST_RESUME_MS;
	if (pause != b->view.paused) {
		b->view.paused = pause;
		changed(b);
	}
	if (b->view.tuning || now_ms - b->poll_ms >= BCAST_POLL_MS) {
		b->poll_ms = now_ms;
		bcast_status_t st;
		if (b->hw->status(&st)) {
			bool was_tuning = b->view.tuning;
			if (st.khz != b->view.khz || st.stereo != b->view.stereo || st.station != b->view.station ||
			    st.rssi / 4u != b->view.rssi / 4u || st.tuning != b->view.tuning) {
				changed(b);
			}
			b->view.stereo = st.stereo;
			b->view.station = st.station;
			b->view.rssi = st.rssi;
			if (b->view.seeking) {
				b->view.khz = st.khz;	// the display follows the sweep
			}
			b->view.tuning = st.tuning;
			if (was_tuning && !st.tuning) {
				if (b->view.seeking) {
					b->view.seek_failed = st.seek_failed;
					b->view.seeking = false;
					b->view.khz = st.khz;
					find_preset(b);
					save(b);
				}
			}
		}
	}
	audio(b);
}

void bcast_take_saved(bcast_t *b, bcast_saved_t *out)
{
	out->khz = b->view.khz;
	out->volume = b->view.volume;
	out->on = b->view.on;
	memcpy(out->preset_khz, b->view.preset_khz, sizeof(out->preset_khz));
	b->dirty = false;
}
