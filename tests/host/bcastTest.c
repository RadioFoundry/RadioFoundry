// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The broadcast FM receiver's behavior (core/bcast, docs/spec/airband-and-broadcast.md 4) against a fake receiver:
// power, tune, step with wrap, seek followed to the end, volume, presets, the saved state, and the channel traffic
// policy (paused while the channel is busy, back BCAST_RESUME_MS after, off in hotspot mode)
#include <stdio.h>
#include <string.h>
#include "bcast/bcast.h"

static int failures;
static int checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static struct {
	bool on, audio, fail_power;
	uint32_t khz, seek_to;
	int busy_polls;		// status reads until a tune / seek is done
	bool seek_failed;
	uint8_t volume;
	int tunes, seeks;
} rx;

static bool f_power(bool on) { if (rx.fail_power) return false; rx.on = on; return true; }
static bool f_tune(uint32_t khz) { if (!rx.on) return false; rx.khz = khz; rx.busy_polls = 2; rx.tunes++; return true; }
static bool f_seek(bool up)
{
	(void)up;
	rx.seeks++;
	rx.busy_polls = 3;
	if (rx.seek_to != 0) {
		rx.khz = rx.seek_to;
	}
	return rx.on;
}
static void f_volume(uint8_t v) { rx.volume = v; }
static bool f_status(bcast_status_t *st)
{
	if (!rx.on) {
		return false;
	}
	if (rx.busy_polls > 0) {
		rx.busy_polls--;
	}
	st->khz = rx.khz;
	st->tuning = rx.busy_polls > 0;
	st->seek_failed = rx.seek_failed;
	st->station = st->stereo = rx.khz == 101300;
	st->rssi = 50;
	return true;
}
static void f_audio(bool on) { rx.audio = on; }
static const hal_bcast_t hw = { 65000, 108000, 50, f_power, f_tune, f_seek, f_volume, f_status, f_audio };

static bcast_t b;
static uint32_t now;

static void run(uint32_t ms, bool busy)
{
	for (uint32_t t = 0; t < ms; t += 25) {
		now += 25;
		bcast_poll(&b, busy, false, now);
	}
}

static void test_basics(void)
{
	memset(&rx, 0, sizeof(rx));
	now = 1000;
	bcast_init(&b, &hw, NULL, now);
	CHECK(b.view.present && !b.view.on && b.view.khz == BCAST_DEFAULT_KHZ && b.view.volume == BCAST_VOLUME_DEFAULT);
	CHECK(!rx.on && !rx.audio && !b.dirty);
	CHECK(!bcast_request(&b, BCMD_TUNE, 98500, now));	// off: refused

	CHECK(bcast_request(&b, BCMD_POWER, 2, now));
	CHECK(b.view.on && rx.on && rx.khz == BCAST_DEFAULT_KHZ && rx.volume == BCAST_VOLUME_DEFAULT && b.dirty);
	run(2500, false);
	CHECK(!b.view.paused && !b.view.tuning && rx.audio);

	CHECK(bcast_request(&b, BCMD_TUNE, 98520, now) && rx.khz == 98500);	// snapped to the 50 kHz grid
	CHECK(!bcast_request(&b, BCMD_TUNE, 120000, now));
	CHECK(bcast_request(&b, BCMD_STEP, 1, now) && rx.khz == 98600);
	bcast_request(&b, BCMD_TUNE, 108000, now);
	CHECK(bcast_request(&b, BCMD_STEP, 1, now) && rx.khz == 65000);	// wraps round the range
	CHECK(bcast_request(&b, BCMD_STEP, -1, now) && rx.khz == 108000);

	// Seek: muted while it sweeps, the result taken when it ends
	rx.seek_to = 101300;
	CHECK(bcast_request(&b, BCMD_SEEK, 1, now) && b.view.seeking && !rx.audio);
	run(200, false);
	CHECK(!b.view.seeking && !b.view.tuning && b.view.khz == 101300 && b.view.station && b.view.stereo && rx.audio);

	// Presets
	CHECK(!bcast_request(&b, BCMD_PRESET, 0, now));	// empty
	CHECK(bcast_request(&b, BCMD_PRESET_SAVE, 0, now) && b.view.preset_khz[0] == 101300 && b.view.preset == 0);
	bcast_request(&b, BCMD_TUNE, 90000, now);
	CHECK(b.view.preset == -1);
	CHECK(bcast_request(&b, BCMD_PRESET, 0, now) && rx.khz == 101300 && b.view.preset == 0);
	CHECK(!bcast_request(&b, BCMD_PRESET_SAVE, BCAST_PRESETS, now));

	// Volume
	CHECK(bcast_request(&b, BCMD_VOLUME, 15, now) && rx.volume == 15);
	CHECK(!bcast_request(&b, BCMD_VOLUME_STEP, 1, now) && rx.volume == 15);
	CHECK(bcast_request(&b, BCMD_VOLUME_STEP, -1, now) && rx.volume == 14);

	// The saved state comes back after a restart, listening again
	bcast_saved_t s;
	bcast_take_saved(&b, &s);
	CHECK(!b.dirty && s.on && s.khz == 101300 && s.volume == 14 && s.preset_khz[0] == 101300);
	memset(&rx, 0, sizeof(rx));
	bcast_init(&b, &hw, &s, now);
	CHECK(b.view.on && rx.on && rx.khz == 101300 && rx.volume == 14 && b.view.preset == 0);

	CHECK(bcast_request(&b, BCMD_POWER, 0, now) && !rx.on && !rx.audio && !b.view.on);

	// A receiver that doesn't answer
	rx.fail_power = true;
	CHECK(!bcast_request(&b, BCMD_POWER, 1, now) && b.view.failed && !b.view.on);
	// No receiver at all
	bcast_init(&b, NULL, NULL, now);
	CHECK(!b.view.present && !bcast_request(&b, BCMD_POWER, 1, now));
}

static void test_traffic(void)
{
	memset(&rx, 0, sizeof(rx));
	now = 1000;
	bcast_init(&b, &hw, NULL, now);
	bcast_request(&b, BCMD_POWER, 1, now);
	run(2500, false);
	CHECK(rx.audio && !b.view.paused);
	run(100, true);	// a call on the channel: muted at once
	CHECK(!rx.audio && b.view.paused && b.view.on);
	run(1500, false);	// still inside the resume delay
	CHECK(!rx.audio && b.view.paused);
	run(600, false);
	CHECK(rx.audio && !b.view.paused);
	// Hotspot mode switches the broadcast off
	bcast_poll(&b, false, true, now);
	CHECK(!b.view.on && !rx.on && !rx.audio);

	// The busy rule from the radio view
	radio_view_t v;
	memset(&v, 0, sizeof(v));
	v.ch.mode = RADIO_MODE_FM;
	CHECK(!bcast_channel_busy(&v));
	v.state = RS_RX;
	CHECK(bcast_channel_busy(&v));
	v.state = RS_IDLE;
	v.monitor = true;
	CHECK(bcast_channel_busy(&v));
	v.monitor = false;
	v.state = RS_TX_DENIED;
	CHECK(bcast_channel_busy(&v));
#if CONFIG_DMR
	memset(&v, 0, sizeof(v));
	v.ch.mode = RADIO_MODE_DMR;
	v.dmr.active = true;
	v.state = RS_RX;
	v.dmr.state = RDMR_RX_OTHER;	// another talkgroup: not ours to hear
	CHECK(!bcast_channel_busy(&v));
	v.dmr.state = RDMR_RX;
	CHECK(bcast_channel_busy(&v));
	v.dmr.state = RDMR_IDLE;
	v.dmr.reply_active = true;
	CHECK(bcast_channel_busy(&v));
#endif
}

int main(void)
{
	test_basics();
	test_traffic();
	printf("# bcastTest: %d checks, %s\n", checks, failures ? "FAILED" : "PASSED");
	return failures != 0;
}
