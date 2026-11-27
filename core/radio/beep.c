// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Beeps and alert tones (beep.h)
#include <string.h>
#include "radio/beep.h"
#include "config.h"

#if CONFIG_BEEP

#define LOAD(x)		__atomic_load_n(&(x), __ATOMIC_ACQUIRE)
#define STORE(x, v)	__atomic_store_n(&(x), (v), __ATOMIC_RELEASE)

#define RAMP_SAMPLES	16		// 2 ms fade in and out of each tone: no clicks
#define AMP_PER_LEVEL	160		// level 100 = about half of full scale (room to mix over voice)
// The own stream routes the speaker silent blocks before the tone (one, or the board's speaker_lead_ms) and holds it
// for a few after: switching the amplifier and the speaker source while the tone plays pops, and turning the route off
// drops the audio still queued (the tail and its fade out)
#define LEAD_BLOCKS_MAX		10	// 200 ms
#define ROUTE_HOLD_BLOCKS	4	// 80 ms
#define IDLE_BLOCKS		ROUTE_HOLD_BLOCKS	// silent blocks after a pattern before the own stream stops
#define STALL_POLLS	8		// beep_service polls (25 ms) without a block: the stream's clock stopped, give up

// Distinct sounds (radio users tell them apart without looking, docs/ux/survey.md)
static const beep_seg_t p_key[] = { { 1000, 25 } };
static const beep_seg_t p_error[] = { { 400, 150 } };
static const beep_seg_t p_permit[] = { { 1600, 50 }, { 0, 40 }, { 1600, 50 } };
static const beep_seg_t p_permit_short[] = { { 1600, 80 } };	// the same pitch, once: it delays keying
static const beep_seg_t p_deny[] = { { 300, 400 } };
static const beep_seg_t p_call_end[] = { { 1200, 50 }, { 0, 30 }, { 900, 70 } };
static const beep_seg_t p_tot[] = { { 1000, 100 }, { 0, 100 }, { 1000, 100 } };
static const beep_seg_t p_batt_low[] = { { 800, 150 }, { 0, 100 }, { 600, 200 } };
static const beep_seg_t p_batt_crit[] = { { 600, 150 }, { 0, 80 }, { 600, 150 }, { 0, 80 }, { 600, 150 } };
static const beep_seg_t p_scan_hold[] = { { 1400, 40 } };
// Two-tone warble, like no other beep: someone else's emergency
static const beep_seg_t p_emergency[] = { { 1500, 160 }, { 900, 160 }, { 1500, 160 }, { 900, 160 }, { 1500, 160 },
					  { 900, 160 } };
// Rising three-note chime: a message
static const beep_seg_t p_message[] = { { 1000, 60 }, { 0, 30 }, { 1300, 60 }, { 0, 30 }, { 1700, 90 } };
// Two rising notes: back on the trunked system
static const beep_seg_t p_system_back[] = { { 1000, 60 }, { 0, 60 }, { 1400, 60 } };
// A fast 1400 / 1100 Hz trill, rung twice like a phone: a call alert paged this radio. Unlike the slow, wide
// emergency warble
static const beep_seg_t p_call_alert[] = { { 1400, 40 }, { 1100, 40 }, { 1400, 40 }, { 1100, 40 }, { 1400, 40 },
					   { 1100, 40 }, { 0, 200 }, { 1400, 40 }, { 1100, 40 }, { 1400, 40 },
					   { 1100, 40 }, { 1400, 40 }, { 1100, 40 } };
// The speaker test (tests/hil/test_speaker.py): silence with the route up (a pop or a click there is the route's, not
// the sweep's), the sweep, a short tail
static const beep_seg_t p_sweep[] = { { 0, 400 }, { BEEP_HZ_SWEEP, 4000 }, { 0, 200 } };

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
	[BEEP_EMERGENCY] = PAT(p_emergency),
	[BEEP_MESSAGE] = PAT(p_message),
	[BEEP_TX_PERMIT_SHORT] = PAT(p_permit_short),
	[BEEP_SWEEP] = PAT(p_sweep),
	[BEEP_SYSTEM_BACK] = PAT(p_system_back),
	[BEEP_CALL_ALERT] = PAT(p_call_alert),
};

// One period of a sine, Q15
static const int16_t beep_sine[64] = {
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

#if CONFIG_SIG_TONES
// Custom patterns (BEEP_CUSTOM): two copies, alternating
static beep_seg_t cust[2][BEEP_CUSTOM_MAX];
static uint8_t cust_n[2];
static uint8_t cust_next;
#endif

// Handed from the radio task to the audio side
static uint8_t req_pattern;
#if CONFIG_SIG_TONES
static uint8_t req_slot;
#endif
static uint8_t req_level;
static uint8_t req_seq;

// Radio task only
static bool own_stream;

// Audio side
static uint8_t cb_seq;
static uint8_t cb_pattern;
static beep_gen_t cb_gen;	// the pattern's steps, taken when it started, and where it is
static volatile bool cb_active;
static volatile uint8_t cb_idle;
static volatile uint32_t cb_blocks;	// own callback calls, for the stall check
static uint32_t seen_blocks;
static uint8_t stall_polls;
static uint8_t cb_lead;		// lead-in blocks played since the route turned on
static uint8_t mix_lead;	// the voice stream's lead-in blocks for the pending pattern (beep_mix)
static uint8_t mix_echo;	// blocks the mic may still hear the last mixed one (beep_mic_echo)

void beep_init(const hal_audio_t *audio)
{
	aud = audio;
}

// The amplifier held powered (beep_warm): since when, until when, and the last poll's time
static bool warm_on;
static uint32_t warm_since, warm_until, last_now;
static volatile uint8_t lead_target = 1;	// lead-in blocks for the pattern requested last (beep_play)
static uint8_t lead_seq;		// the request the lead-in count was last reset for (the audio callback's)

static uint8_t lead_blocks(void)
{
	uint32_t ms = aud != NULL ? aud->speaker_lead_ms : 0;
	// Warm for at least the lead: the amplifier is up already, one block is enough for the route to settle
	if (warm_on && last_now - warm_since >= ms) {
		return 1;
	}
	uint32_t n = (ms + AUDIO_BLOCK_MS - 1u) / AUDIO_BLOCK_MS;
	return (uint8_t)(n == 0 ? 1u : n > LEAD_BLOCKS_MAX ? LEAD_BLOCKS_MAX : n);
}

void beep_warm(uint32_t now_ms)
{
	last_now = now_ms;
	if (aud == NULL || aud->speaker_warm == NULL || aud->speaker_hang_s == 0) {
		return;
	}
	warm_until = now_ms + (uint32_t)aud->speaker_hang_s * 1000u;
	if (!warm_on) {
		warm_on = true;
		warm_since = now_ms;
		stats.warms++;
		aud->speaker_warm(true);
	}
}

bool beep_warm_on(void)
{
	return warm_on;
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
	if (b <= BEEP_NONE || (b >= BEEP_COUNT && (!CONFIG_SIG_TONES || b != BEEP_CUSTOM)) || level == 0) {
		return;
	}
	stats.last = (uint8_t)b;
	if (aud == NULL || aud->start == NULL) {
		stats.no_stream++;
		return;
	}
	req_pattern = (uint8_t)b;
	req_level = level > 100 ? 100 : level;
	lead_target = lead_blocks();	// before the warm below: an amplifier only just powered still needs the lead
	beep_warm(last_now);		// a beep is a sound: the amplifier stays up for the next one
	STORE(req_seq, (uint8_t)(req_seq + 1));
	if (shared) {
		stats.shared++;
	} else {
		// Always (re)start: the board just installs the callback when it already streams. The voice stream may have
		// started and stopped since the last beep, so own_stream alone can't say whether ours still runs
		if (aud->start(beep_audio_block, NULL)) {
			own_stream = true;
			seen_blocks = cb_blocks;
			stall_polls = 0;
		} else {
			// A start that fails part way (the codec clock dropped while it synchronized) may already have run the
			// callback: stop it for sure and give the speaker route back, or the analog audio stays muted
			own_stream = false;
			cb_lead = 0;
			aud->stop();
			if (aud->speaker_pcm != NULL) {
				aud->speaker_pcm(false);
			}
			cb_active = false;
			cb_seq = req_seq;	// nothing will play it
			stats.no_stream++;
			return;
		}
	}
	stats.played++;
}

#if CONFIG_SIG_TONES
void beep_play_custom(const beep_seg_t *segs, uint8_t n, uint8_t level, bool shared)
{
	bool tone = false;
	if (n > BEEP_CUSTOM_MAX) {
		n = BEEP_CUSTOM_MAX;
	}
	for (uint8_t i = 0; i < n; i++) {
		tone = tone || (segs[i].hz > BEEP_HZ_SWEEP && segs[i].ms != 0);
	}
	if (!tone) {
		return;
	}
	uint8_t slot = cust_next;
	cust_next ^= 1u;
	memcpy(cust[slot], segs, n * sizeof(beep_seg_t));
	for (uint8_t i = 0; i < n; i++) {
		if (cust[slot][i].hz == BEEP_HZ_SWEEP) {
			cust[slot][i].hz = 0;	// the sweep is a fixed pattern's
		}
	}
	cust_n[slot] = n;
	req_slot = slot;
	beep_play(BEEP_CUSTOM, level, shared);
}
#endif

void beep_service(bool shared, uint32_t now_ms)
{
	last_now = now_ms;
	if (warm_on && (int32_t)(now_ms - warm_until) >= 0) {
		warm_on = false;
		if (aud != NULL && aud->speaker_warm != NULL) {
			aud->speaker_warm(false);
		}
	}
	if (!own_stream) {
		return;
	}
	if (shared) {
		own_stream = false;	// the voice stream's callback replaced ours and mixes the beep in
		return;
	}
	// A stream whose clock stopped (the codec went quiet after a few blocks) never calls back again: without this
	// the PCM speaker route would stay on and mute the analog receive audio until a reboot
	uint32_t n = cb_blocks;
	if (n != seen_blocks) {
		seen_blocks = n;
		stall_polls = 0;
	} else if (++stall_polls >= STALL_POLLS) {
		own_stream = false;
		stall_polls = 0;
		cb_lead = 0;
		aud->stop();
		if (aud->speaker_pcm != NULL) {
			aud->speaker_pcm(false);
		}
		cb_active = false;	// no callback runs now, so the pattern state is ours
		cb_seq = LOAD(req_seq);
		stats.stalls++;
		return;
	}
	if (!beep_busy() && cb_idle >= IDLE_BLOCKS) {
		own_stream = false;
		cb_lead = 0;
		aud->stop();
		if (aud->speaker_pcm != NULL) {
			aud->speaker_pcm(false);
		}
	}
}

static void gen_load(beep_gen_t *g)
{
	const beep_seg_t *s = &g->seg[g->i];
	g->len = g->left = (uint32_t)s->ms * (AUDIO_RATE_HZ / 1000);
	// Phase step: hz / rate of a 2^32 turn
	g->step = (uint32_t)(((uint64_t)s->hz << 32) / AUDIO_RATE_HZ);
	g->dstep = 0;
	if (s->hz == BEEP_HZ_SWEEP && g->len != 0) {
		g->step = (uint32_t)(((uint64_t)BEEP_SWEEP_LO_HZ << 32) / AUDIO_RATE_HZ);
		g->dstep = (uint32_t)((((uint64_t)(BEEP_SWEEP_HI_HZ - BEEP_SWEEP_LO_HZ) << 32) / AUDIO_RATE_HZ) / g->len);
	}
}

void beep_gen_start(beep_gen_t *g, const beep_seg_t *s, uint8_t n, int32_t amp_q15)
{
	memset(g, 0, sizeof(*g));
	g->seg = s;
	g->n = n;
	g->amp = amp_q15 > 32767 ? 32767 : amp_q15 < 0 ? 0 : amp_q15;
	if (n != 0) {
		gen_load(g);
	}
}

static bool beep_gen_more(beep_gen_t *g)
{
	// Exhausted steps are skipped here so "more" is true only while samples remain
	while (g->i < g->n && g->left == 0) {
		if (++g->i < g->n) {
			gen_load(g);
		}
	}
	return g->i < g->n;
}

// The next sample of the sequence (amplitude applied): false when it is over
static bool beep_gen_next(beep_gen_t *g, int32_t *v)
{
	if (!beep_gen_more(g)) {
		return false;
	}
	int32_t out = 0;
	if (g->step != 0) {
		uint32_t done = g->len - g->left;
		uint32_t edge = done < g->left ? done : g->left - 1;
		int32_t amp = g->amp;
		if (edge < RAMP_SAMPLES) {
			amp = amp * (int32_t)edge / RAMP_SAMPLES;
		}
		// Linear interpolation between table entries (top 6 bits index, next 10 the fraction)
		uint32_t idx = g->phase >> 26;
		int32_t frac = (int32_t)((g->phase >> 16) & 0x3FFu);
		int32_t a = beep_sine[idx], b = beep_sine[(idx + 1) & 63u];
		out = ((a + (((b - a) * frac) >> 10)) * amp) >> 15;
		g->phase += g->step;
		g->step += g->dstep;
	}
	g->left--;
	*v = out;
	return true;
}

bool beep_gen_block(beep_gen_t *g, int16_t *pcm, unsigned count)
{
	for (unsigned k = 0; k < count; k++) {
		int32_t v;
		pcm[k] = beep_gen_next(g, &v) ? (int16_t)v : 0;
	}
	return beep_gen_more(g);
}

// The pattern into one block (both streams)
static bool tone_mix(int16_t *spk)
{
	uint8_t seq = LOAD(req_seq);
	if (seq != cb_seq) {
		cb_seq = seq;
		cb_pattern = req_pattern;
		const beep_seg_t *segs;
		uint8_t count;
#if CONFIG_SIG_TONES
		if (cb_pattern == BEEP_CUSTOM) {
			segs = cust[req_slot];
			count = cust_n[req_slot];
		} else
#endif
		{
			segs = patterns[cb_pattern].segs;
			count = patterns[cb_pattern].count;
		}
		beep_gen_start(&cb_gen, segs, count, (int32_t)req_level * AMP_PER_LEVEL);
		cb_active = true;
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
		int32_t v;
		if (!beep_gen_next(&cb_gen, &v)) {
			cb_active = false;
			return true;	// the rest of this block is left as it was
		}
		int32_t out = spk[i] + v;
		spk[i] = (int16_t)(out > 32767 ? 32767 : out < -32768 ? -32768 : out);
	}
	if (!beep_gen_more(&cb_gen)) {
		cb_active = false;	// ended exactly at the block's end
	}
	return true;
}

static bool mix_block(int16_t *spk)
{
	// A pattern the amplifier isn't ready for (cold, or only just powered): its lead-in in silence with the speaker
	// routed, as on the own stream. A DMR call leaves the voice stream running until the radio leaves DMR, so most
	// beeps on a DMR channel come this way. Warm, the route settling is the voice stream's: the tone at once
	if (LOAD(req_seq) == cb_seq) {
		mix_lead = 0;		// nothing pending
	} else if (!cb_active) {
		uint8_t want = lead_target;
		if (want > 1 && mix_lead < want) {
			mix_lead++;
			stats.leads++;
			return true;
		}
	}
	return tone_mix(spk);
}

bool beep_mix(int16_t *spk)
{
	bool on = mix_block(spk);
	if (on) {
		mix_echo = BEEP_ECHO_BLOCKS;
	} else if (mix_echo != 0) {
		mix_echo--;
	}
	return on;
}

bool beep_mic_echo(void)
{
	return mix_echo != 0;
}

void beep_audio_block(const int16_t *mic, int16_t *spk, void *ctx)
{
	(void)mic;
	(void)ctx;
	cb_blocks++;
	memset(spk, 0, AUDIO_BLOCK_SAMPLES * sizeof(int16_t));
	// A new pattern after the route's hold ran out: its lead-in from the first block (inside the hold the route is
	// still up: none). The hold's end on this stream resets the count too, but the voice stream may have taken the
	// last beep over (beep_mix counts the idle blocks there) and left it counted up
	uint8_t seq = LOAD(req_seq);
	if (seq != lead_seq) {
		lead_seq = seq;
		if (!cb_active && cb_idle >= ROUTE_HOLD_BLOCKS) {
			cb_lead = 0;
		}
	}
	bool on;
	if (cb_lead < lead_target && beep_busy()) {
		cb_lead++;
		stats.leads++;
		on = true;	// silence: the route settles before the tone
	} else {
		on = tone_mix(spk) || cb_idle < ROUTE_HOLD_BLOCKS;
	}
	if (!on) {
		cb_lead = 0;
	}
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
