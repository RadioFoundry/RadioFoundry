// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Beeps and alert tones (beep.h)
#include <string.h>
#include "radio/beep.h"
#include "config.h"

#if CONFIG_BEEP

#define LOAD(x)		__atomic_load_n(&(x), __ATOMIC_ACQUIRE)
#define STORE(x, v)	__atomic_store_n(&(x), (v), __ATOMIC_RELEASE)

#define RAMP_SAMPLES	16		// 2 ms fade in and out of each tone: no clicks
#define AMP_PER_LEVEL	160		// level 100 = about half of full scale (room to mix over voice)
#define IDLE_BLOCKS	2		// silent blocks after a pattern before the own stream stops

// Distinct sounds (radio users tell them apart without looking, docs/ux/survey.md)
static const beep_seg_t p_key[] = { { 1000, 25 } };
static const beep_seg_t p_error[] = { { 400, 150 } };
static const beep_seg_t p_permit[] = { { 1600, 50 }, { 0, 40 }, { 1600, 50 } };
static const beep_seg_t p_deny[] = { { 300, 400 } };
static const beep_seg_t p_call_end[] = { { 1200, 50 }, { 0, 30 }, { 900, 70 } };
static const beep_seg_t p_tot[] = { { 1000, 100 }, { 0, 100 }, { 1000, 100 } };
static const beep_seg_t p_batt_low[] = { { 800, 150 }, { 0, 100 }, { 600, 200 } };
static const beep_seg_t p_batt_crit[] = { { 600, 150 }, { 0, 80 }, { 600, 150 }, { 0, 80 }, { 600, 150 } };
static const beep_seg_t p_scan_hold[] = { { 1400, 40 } };

#define PAT(p)	{ p, sizeof(p) / sizeof(p[0]) }
static const struct {
	const beep_seg_t *segs;
	uint8_t count;
} patterns[BEEP_COUNT] = {
	[BEEP_KEY] = PAT(p_key),
	[BEEP_ERROR] = PAT(p_error),
	[BEEP_TX_PERMIT] = PAT(p_permit),
	[BEEP_TX_DENY] = PAT(p_deny),
	[BEEP_CALL_END] = PAT(p_call_end),
	[BEEP_TOT_WARN] = PAT(p_tot),
	[BEEP_BATTERY_LOW] = PAT(p_batt_low),
	[BEEP_BATTERY_CRIT] = PAT(p_batt_crit),
	[BEEP_SCAN_HOLD] = PAT(p_scan_hold),
};

// One period of a sine, Q15
static const int16_t sine[64] = {
	0, 3212, 6393, 9512, 12539, 15446, 18204, 20787,
	23170, 25329, 27245, 28898, 30273, 31356, 32137, 32609,
	32767, 32609, 32137, 31356, 30273, 28898, 27245, 25329,
	23170, 20787, 18204, 15446, 12539, 9512, 6393, 3212,
	0, -3212, -6393, -9512, -12539, -15446, -18204, -20787,
	-23170, -25329, -27245, -28898, -30273, -31356, -32137, -32609,
	-32767, -32609, -32137, -31356, -30273, -28898, -27245, -25329,
	-23170, -20787, -18204, -15446, -12539, -9512, -6393, -3212,
};

static const hal_audio_t *aud;
static beep_stats_t stats;

// Handed from the radio task to the audio side
static uint8_t req_pattern;
static uint8_t req_level;
static uint8_t req_seq;

// Radio task only
static bool own_stream;

// Audio side
static uint8_t cb_seq;
static uint8_t cb_pattern;
static uint8_t cb_seg;
static uint32_t cb_left;	// samples left in the segment
static uint32_t cb_len;		// samples in the segment
static uint32_t cb_phase, cb_step;
static int32_t cb_amp;
static volatile bool cb_active;
static volatile uint8_t cb_idle;

void beep_init(const hal_audio_t *audio)
{
	aud = audio;
}

uint8_t beep_pattern(beep_t b, const beep_seg_t **segs)
{
	if (b <= BEEP_NONE || b >= BEEP_COUNT) {
		*segs = NULL;
		return 0;
	}
	*segs = patterns[b].segs;
	return patterns[b].count;
}

bool beep_busy(void)
{
	return cb_active || LOAD(req_seq) != cb_seq;
}

void beep_play(beep_t b, uint8_t level, bool shared)
{
	if (b <= BEEP_NONE || b >= BEEP_COUNT || level == 0) {
		return;
	}
	if (aud == NULL || aud->start == NULL) {
		stats.no_stream++;
		return;
	}
	req_pattern = (uint8_t)b;
	req_level = level > 100 ? 100 : level;
	STORE(req_seq, (uint8_t)(req_seq + 1));
	if (!shared) {
		// Always (re)start: the board just installs the callback when it already streams. The voice stream may have
		// started and stopped since the last beep, so own_stream alone can't say whether ours still runs
		if (aud->start(beep_audio_block, NULL)) {
			own_stream = true;
		} else {
			own_stream = false;
			cb_seq = req_seq;	// nothing will play it
			stats.no_stream++;
			return;
		}
	}
	stats.played++;
}

void beep_service(bool shared)
{
	if (!own_stream) {
		return;
	}
	if (shared) {
		own_stream = false;	// the voice stream's callback replaced ours and mixes the beep in
		return;
	}
	if (!beep_busy() && cb_idle >= IDLE_BLOCKS) {
		own_stream = false;
		aud->stop();
		if (aud->speaker_pcm != NULL) {
			aud->speaker_pcm(false);
		}
	}
}

static void load_seg(void)
{
	const beep_seg_t *s = &patterns[cb_pattern].segs[cb_seg];
	cb_len = cb_left = (uint32_t)s->ms * (AUDIO_RATE_HZ / 1000);
	// Phase step: hz / rate of a 2^32 turn
	cb_step = (uint32_t)(((uint64_t)s->hz << 32) / AUDIO_RATE_HZ);
}

bool beep_mix(int16_t *spk)
{
	uint8_t seq = LOAD(req_seq);
	if (seq != cb_seq) {
		cb_seq = seq;
		cb_pattern = req_pattern;
		cb_amp = (int32_t)req_level * AMP_PER_LEVEL;
		cb_seg = 0;
		cb_phase = 0;
		cb_active = true;
		load_seg();
	}
	if (!cb_active) {
		if (cb_idle < 255) {
			cb_idle++;
		}
		return false;
	}
	cb_idle = 0;
	stats.blocks++;
	for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
		while (cb_left == 0) {
			if (++cb_seg >= patterns[cb_pattern].count) {
				cb_active = false;
				return true;	// the rest of this block is left as it was
			}
			load_seg();
		}
		if (cb_step != 0) {
			uint32_t done = cb_len - cb_left;
			uint32_t edge = done < cb_left ? done : cb_left - 1;
			int32_t amp = cb_amp;
			if (edge < RAMP_SAMPLES) {
				amp = amp * (int32_t)edge / RAMP_SAMPLES;
			}
			// Linear interpolation between table entries (top 6 bits index, next 10 the fraction)
			uint32_t idx = cb_phase >> 26;
			int32_t frac = (int32_t)((cb_phase >> 16) & 0x3FFu);
			int32_t a = sine[idx], b = sine[(idx + 1) & 63u];
			int32_t v = a + (((b - a) * frac) >> 10);
			int32_t out = spk[i] + ((v * amp) >> 15);
			spk[i] = (int16_t)(out > 32767 ? 32767 : out < -32768 ? -32768 : out);
			cb_phase += cb_step;
		}
		cb_left--;
	}
	if (cb_left == 0 && cb_seg + 1u >= patterns[cb_pattern].count) {
		cb_active = false;	// ended exactly at the block's end
	}
	return true;
}

void beep_audio_block(const int16_t *mic, int16_t *spk, void *ctx)
{
	(void)mic;
	(void)ctx;
	memset(spk, 0, AUDIO_BLOCK_SAMPLES * sizeof(int16_t));
	bool on = beep_mix(spk);
	if (aud != NULL && aud->speaker_pcm != NULL) {
		aud->speaker_pcm(on);	// every block: the voice stream may have switched the route since (cheap when equal)
	}
}

void beep_get_stats(beep_stats_t *out)
{
	*out = stats;
}

void beep_reset_stats(void)
{
	memset(&stats, 0, sizeof(stats));
}

#endif
