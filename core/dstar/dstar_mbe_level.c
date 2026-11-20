// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// SPDX-FileCopyrightText: 2026 mbe contributors
/*
 * Speech level control for the D-STAR encoder (dstar_mbe_level.h), ported from the research codec's mbe_level.c so
 * that the two produce the same frames bit for bit.
 *
 * Why. Over a Pluto -> IC-9700 loop (18 takes), at one fixed level a loud talker drove the 9700's decoded audio into a
 * hard ceiling (about 26,900; 18 to 1,352 samples over 24,000 per take, heard as distortion) while a LibriSpeech reader
 * never passed 19,200. Read with mbelib's tables our frames of the two sat 15 dB apart though their active input levels
 * differed by 5 dB, and the level the frames carry predicts the 9700's output level within 1.5 dB (std over the takes)
 * when read as a DVSI decoder plays them: each frame's log magnitudes expanded by 1/0.73 about their mean, power not
 * kept (no expansion: 2.9 dB; 1/0.6: 2.7 dB). So the level to control is that one, measured on the parameters, not the
 * microphone's. Icom radios level their own audio too: an IC-9700's frames did not change between LAN audio levels 6 dB
 * apart.
 *
 * How. Per frame, that level la. A background estimate follows la down at once and up at 0.05 dB a frame; a frame more
 * than LV_ACTIVE above it, and above LV_ABS_FLOOR, is speech. The speech level S follows speech frames up with a 1/4
 * step (about 80 ms) and down with 1/64 (about 1.3 s); between words it holds, so pauses don't pump the background up.
 * The gain is LV_TARGET - S within LV_GAIN_MIN..LV_GAIN_MAX, and on every frame no more than brings la to LV_CEILING:
 * a loud onset is capped at once rather than after the attack. The gain is added to every magnitude after the analysis
 * (and its DVSI match), before the quantizer.
 *
 * On air (the leveler through the same loop, three sources each at its own level and 10 dB under and 6 dB over it):
 * every source came back within about 1.5 dB over that 16 dB range. A clean recording of a male voice and the
 * LibriSpeech reader never reached the 9700's ceiling; a male voice recorded through a messaging app's compressed
 * audio touched it (about 26,900) on 10 to 17 samples a take.
 *
 * LV_TARGET. The research value (41900) put the reader about 2 dB under its best fixed level, so it was to rise 2 dB.
 * The research's level model reads each source's loudest frames against the take that just clipped (the compressed
 * voice at a fixed offset, peak -2.0 dBFS): at 41900 they sit 2.1-2.6 dB under it (5 loudest frames, 99th percentile),
 * at +2 dB 0.1-0.6 dB under, which is no margin, and at +1 dB 1.1-1.5 dB under. So +1 dB (42580). The compressed voice
 * still touched the ceiling at the research value, so it may clip a little more at this one; the on-air check of the
 * new encoder decides (docs/testing/dm32-first-session.md). LV_CEILING, 10 dB over the target, caps single loud frames
 * (the loud talker's loudest sit 13.6 dB over the speech estimate). The reader at +12 dB had its gain index b2 at its
 * top (62) on 10 % of voiced frames: a second reason not to aim higher.
 */
#include "dstar/dstar_mbe_level.h"
#include "dstar/dstar_mbe_fixed.h"

#define DB(x)		((int32_t)((x) * 680.4))	// dB -> log2 amplitude Q12 (4096 / 6.0206), constants only
#define LV_EXPAND_Q12	5611				// 1 / 0.73: how a DVSI decoder reads our frames' depth
#define LV_TARGET	42580				// log2 amplitude Q12, our magnitudes' scale: see above
#define LV_CEILING	(LV_TARGET + DB(10))
#define LV_GAIN_MIN	DB(-20)
#define LV_GAIN_MAX	DB(20)
#define LV_ACTIVE	DB(15)
#define LV_ABS_FLOOR	(LV_TARGET - DB(45))
#define LV_FLOOR_RISE	DB(0.05)
#define LV_SILENT	(-6 * 4096)			// the analysis's silent frame: LOG_FLOOR / 2 in every band

void dstar_mbe_level_init(dstar_mbe_level_t *s)
{
	s->speech = LV_TARGET;
	s->floor = LV_ABS_FLOOR;	// low, so speech from the first frame counts as speech
	s->gain = 0;
	s->off = 0;
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
	int32_t emx = m + (((mx - m) >> 4) * LV_EXPAND_Q12 >> 8);
	// the sum of 2^(2 e) relative to the largest: each term at most 4096, so at most 56 x 4096
	uint32_t pw = 0;
	for (unsigned l = 1; l <= L; ++l) {
		int32_t e = m + (((p->log2_amp[l] - m) >> 4) * LV_EXPAND_Q12 >> 8);
		pw += dstar_mbe_pow2_q12(2 * (e - emx) + (12 << 12));
	}
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
