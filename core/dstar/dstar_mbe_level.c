// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// SPDX-FileCopyrightText: 2026 mbe contributors
/*
 * Speech level control for the D-STAR encoder (dstar_mbe_level.h), ported from the research codec's mbe_level.c so
 * that the two produce the same frames bit for bit (the research's test/icom/fwcheck.sh checks it).
 *
 * Why a leveler (19 Nov 2026). Over a Pluto -> IC-9700 loop (18 takes), at one fixed level a loud talker drove the
 * 9700's decoded audio into a hard ceiling (about 26,900; 18 to 1,352 samples over 24,000 per take, heard as
 * distortion) while a LibriSpeech reader never passed 19,200. Read with mbelib's tables our frames of the two sat 15 dB
 * apart though their active input levels differed by 5 dB, and the level the frames carry predicts the 9700's output
 * level within 1.5 dB (std over the takes) when read as a DVSI decoder plays them: each frame's log magnitudes expanded
 * by 1/0.73 about their mean, power not kept (no expansion: 2.9 dB; 1/0.6: 2.7 dB). So the level to control is that
 * one, measured on the parameters, not the microphone's. Icom radios level their own audio too: an IC-9700's frames
 * did not change between LAN audio levels 6 dB apart. On air the leveler held three sources within about 1.5 dB over a
 * 16 dB input range.
 *
 * Why it measures the speech band (21 Nov 2026, offline, the research's LOUDNESS-21NOV.md). Icom handhelds played our
 * D-STAR quietly. Decoded through mbelib, in the 1-3.4 kHz band a handheld's speaker actually reproduces, our overs
 * were no louder than an ID-51's own (-35.4 / -33.4 against -35.7 / -33.6 dBFS) and about 15 dB under an IC-9700's
 * (-17.8). Our extra level was all below 500 Hz: through the 9700's own decoder our frames were 12.3 dB over the
 * ID-51's below 400 Hz but 2.4 dB over it at 1-3.4 kHz. Measured full band, that bass set the gain and held the speech
 * band down. So each harmonic below 500 Hz is weighted by (f / 500)^2 in amplitude, a second-order high-pass the shape
 * of the study's small-speaker stand-in, before the power is summed: a bass-heavy voice or microphone no longer turns
 * the speech down. It is a log2 offset per harmonic, 2 log2(f / 500 Hz), on the magnitudes the leveler already sees.
 * The ceiling is measured the same way, so it too caps what a listener hears; bass alone can no longer trip it.
 *
 * How. Per frame, that level la. A background estimate follows la down at once and up at 0.05 dB a frame; a frame more
 * than LV_ACTIVE above it, and above LV_ABS_FLOOR, is speech. The speech level S follows speech frames up with a 1/4
 * step (about 80 ms) and down with 1/64 (about 1.3 s); between words it holds, so pauses don't pump the background up.
 * The gain is LV_TARGET - S within LV_GAIN_MIN..LV_GAIN_MAX, and on every frame no more than brings la to LV_CEILING:
 * a loud onset is capped at once rather than after the attack. The gain is added to every magnitude after the analysis
 * (and its DVSI match and balance), before the quantizer.
 *
 * LV_TARGET = LV_TARGET_19NOV + LV_LOUDER. LV_TARGET_19NOV (42580) was set on 19 Nov against the 9700's LAN-audio
 * ceiling (the compressed voice's loudest frames 1.1-1.5 dB under the take that clipped). That ceiling belongs to the
 * 9700's LAN path, not to what a handheld user hears, and the 9700's own microphone frames already reach 32,760
 * through mbelib, so a DVSI station is louder still. Closing the 15 dB gap is planned in two steps, each followed by a
 * listen on an ID-51 and the 9700's speaker rather than LAN dBFS: LV_LOUDER is the first, +6 dB; the next +6 is
 * DB(12) there. Our gain index b2 has the room: its median ran about 15 steps (about 25 dB of gamma) under the 9700's.
 * The speech-band weighting and the DVSI balance's bass cut add level of their own, since bass no longer counts
 * against the target: offline (five recordings, mbelib's decode, 1-3.4 kHz), the weighting alone gave +0.9 to +4.3 dB,
 * with the balance +1.6 to +6.9, and with this step +6.0 to +11.8 (the operator's voice -30.6 -> -18.8 dBFS, an
 * IC-9700 operator's -17.8). So the second step may not be needed: listen before taking it. LV_CEILING stays
 * 10 dB over the target, which caps single loud frames (the loud talker's loudest sat 13.6 dB over the speech
 * estimate); expect loud talkers to reach the 9700's LAN ceiling, which is accepted. LV_ABS_FLOOR is a threshold on
 * the input, so it stays where the 19 Nov target put it rather than moving with each step.
 *
 * LV_GAIN_MAX was 20 dB, which a quiet talker's speech already came close to needing (17.6 dB for the test's quiet
 * syllables). The target's step adds 6 dB to every gain and the speech-band measure reads a voice lower by however much
 * of its level was bass (3.3 dB on the test's 120 Hz voice, about 5 on our real overs), so at 20 the quiet talkers the
 * leveler used to bring up would stay 9 dB or more short. 30 keeps the 19 Nov input range with room for the second
 * step. The gain follows speech frames only, so it is not a noise gate's gain: pauses keep the speech's gain.
 * LV_GAIN_MIN stays: the new measure and target only ever ask for more gain, not less.
 */
#include "dstar/dstar_mbe_level.h"
#include "dstar/dstar_mbe_fixed.h"

#define DB(x)		((int32_t)((x) * 680.4))	// dB -> log2 amplitude Q12 (4096 / 6.0206), constants only
#define LV_EXPAND_Q12	5611				// 1 / 0.73: how a DVSI decoder reads our frames' depth
#define LV_TARGET_19NOV	42580				// log2 amplitude Q12, our magnitudes' scale: see above
#define LV_LOUDER	DB(6)				// 21 Nov, first of two steps; the second makes it DB(12)
#define LV_TARGET	(LV_TARGET_19NOV + LV_LOUDER)
#define LV_CEILING	(LV_TARGET + DB(10))
#define LV_GAIN_MIN	DB(-20)
#define LV_GAIN_MAX	DB(30)				// 20 on 19 Nov: see above
#define LV_ACTIVE	DB(15)
#define LV_ABS_FLOOR	(LV_TARGET_19NOV - DB(45))	// an input threshold: not moved by LV_LOUDER
#define LV_FLOOR_RISE	DB(0.05)
#define LV_SILENT	(-6 * 4096)			// the analysis's silent frame: LOG_FLOOR / 2 in every band
#define LV_HP_Q24	(1u << 20)			// 500 Hz in f0_q24's units (500 / 8000 x 2^24)

void dstar_mbe_level_init(dstar_mbe_level_t *s)
{
	s->speech = LV_TARGET;
	s->floor = LV_ABS_FLOOR;	// low, so speech from the first frame counts as speech
	s->gain = 0;
	s->off = 0;
}

// Harmonic l's log2 magnitude as a DVSI decoder plays it (expanded about the frame's mean m), weighted by (f / 500)^2
// below 500 Hz (see above)
static int32_t heard(const dstar_ambe_params_t *p, unsigned l, int32_t m)
{
	int32_t e = m + (((p->log2_amp[l] - m) >> 4) * LV_EXPAND_Q12 >> 8);
	uint32_t f = l * p->f0_q24;
	if (f < LV_HP_Q24)
		e += 2 * (dstar_mbe_log2_q12(f) - (20 << 12));
	return e;
}

int32_t dstar_mbe_level_of(const dstar_ambe_params_t *p)
{
	unsigned L = p->L;
	if (L == 0)
		return INT32_MIN;
	int32_t sum = 0, mx = INT32_MIN;
	for (unsigned l = 1; l <= L; ++l) {
		sum += p->log2_amp[l];
		if (p->log2_amp[l] > mx)
			mx = p->log2_amp[l];
	}
	if (mx <= LV_SILENT)
		return INT32_MIN;
	int32_t m = sum / (int32_t)L;
	// The power is summed relative to the loudest weighted harmonic, not the loudest unweighted one: a frame whose
	// loudest harmonic is a weighted-down bass one would otherwise lose its upper harmonics to pow2's rounding
	int32_t emx = INT32_MIN;
	for (unsigned l = 1; l <= L; ++l) {
		int32_t e = heard(p, l, m);
		if (e > emx)
			emx = e;
	}
	// the sum of 2^(2 e) relative to the largest: each term at most 4096, so at most 56 x 4096
	uint32_t pw = 0;
	for (unsigned l = 1; l <= L; ++l)
		pw += dstar_mbe_pow2_q12(2 * (heard(p, l, m) - emx) + (12 << 12));
	return (dstar_mbe_log2_q12(pw ? pw : 1) - (12 << 12)) / 2 + emx;
}

int32_t dstar_mbe_level(dstar_mbe_level_t *s, dstar_ambe_params_t *p)
{
	if (s->off)
		return 0;
	int32_t la = dstar_mbe_level_of(p);
	if (la == INT32_MIN)
		return 0;
	if (la < s->floor)
		s->floor = la;
	else
		s->floor += LV_FLOOR_RISE;
	if (la > s->floor + LV_ACTIVE && la > LV_ABS_FLOOR) {
		s->speech += la > s->speech ? (la - s->speech) / 4 : (la - s->speech) / 64;
		int32_t g = LV_TARGET - s->speech;
		s->gain = g < LV_GAIN_MIN ? LV_GAIN_MIN : g > LV_GAIN_MAX ? LV_GAIN_MAX : g;
	}
	int32_t g = s->gain;
	if (la + g > LV_CEILING)
		g = LV_CEILING - la;
	for (unsigned l = 1; l <= p->L; ++l)
		p->log2_amp[l] += g;
	return g;
}
