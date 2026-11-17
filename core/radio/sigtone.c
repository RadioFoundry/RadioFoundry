// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Signaling tones (sigtone.h)
#include <string.h>
#include "radio/sigtone.h"
#include "radio/beep.h"
#include "hal_audio.h"
#include "config.h"

#if CONFIG_SIG_TONES

#define RAMP_SAMPLES	16	// 2 ms, as beep.c: no clicks

// Pitches follow the beeps' (beep.c): a sequence reads as the same family of sounds
static const beep_seg_t s_permit[] = { { 1600, 50 }, { 0, 40 }, { 1600, 50 } };
static const beep_seg_t s_end[] = { { 1200, 50 }, { 0, 30 }, { 900, 70 } };

// The outputs a record may hold: what the menu offers (an end of reception has no air copy), so a value the radio
// would change reads as absent instead
static bool rec_out_ok(uint8_t out, sig_event_t ev)
{
	return out == sig_out_clean(out, ev);
}

// Byte offsets of step i's frequency and length in a record (the first SIG_REC_BASE steps as in the first version)
static unsigned rec_hz(unsigned i)
{
	return i < SIG_REC_BASE ? 2u + 2u * i : 2u + 3u * SIG_REC_BASE + 2u * (i - SIG_REC_BASE);
}

static unsigned rec_ms(unsigned i)
{
	return i < SIG_REC_BASE ? 2u + 2u * SIG_REC_BASE + i : 2u + 3u * SIG_REC_BASE + 2u * (SIG_STEPS - SIG_REC_BASE) + (i - SIG_REC_BASE);
}

bool sig_rec_decode(const uint8_t *v, int len, sig_event_t ev, sig_rec_t *r)
{
	uint8_t b[SIG_REC_LEN] = { 0 };
	if (len < 1) {
		return false;
	}
	memcpy(b, v, (unsigned)len < SIG_REC_LEN ? (unsigned)len : SIG_REC_LEN);
	r->dmr = b[0];
	r->fm = b[1];
	for (unsigned i = 0; i < SIG_STEPS; i++) {
		r->hz[i] = (uint16_t)(b[rec_hz(i)] | b[rec_hz(i) + 1] << 8);
		r->ms10[i] = b[rec_ms(i)];
		if (r->hz[i] > SIG_HZ_MAX || r->hz[i] % SIG_HZ_UNIT != 0 || r->ms10[i] > SIG_MAX_MS / SIG_MS_UNIT) {
			return false;
		}
	}
	return rec_out_ok(r->dmr, ev) && rec_out_ok(r->fm, ev);
}

void sig_rec_encode(const sig_rec_t *r, uint8_t out[SIG_REC_LEN])
{
	out[0] = r->dmr;
	out[1] = r->fm;
	for (unsigned i = 0; i < SIG_STEPS; i++) {
		out[rec_hz(i)] = (uint8_t)(r->hz[i] & 0xFFu);
		out[rec_hz(i) + 1] = (uint8_t)(r->hz[i] >> 8);
		out[rec_ms(i)] = r->ms10[i];
	}
}

uint8_t sig_chain(const uint16_t hz[SIG_STEPS], const uint8_t ms10[SIG_STEPS], sig_event_t ev,
		  beep_seg_t out[SIG_STEPS])
{
	uint32_t left = SIG_MAX_MS;
	uint8_t m = 0;
	for (uint8_t i = 0; i < SIG_STEPS && ms10[i] != 0 && left != 0; i++) {
		uint32_t ms = (uint32_t)ms10[i] * SIG_MS_UNIT;
		out[m].hz = hz[i] > SIG_HZ_MAX ? SIG_HZ_MAX : hz[i];
		out[m].ms = (uint16_t)(ms > left ? left : ms);
		left -= out[m].ms;
		m++;
	}
	if (m != 0) {
		return m;
	}
	const beep_seg_t *src = ev == SIG_EV_BEGIN ? s_permit : s_end;
	m = ev == SIG_EV_BEGIN ? sizeof(s_permit) / sizeof(s_permit[0]) : sizeof(s_end) / sizeof(s_end[0]);
	memcpy(out, src, m * sizeof(beep_seg_t));
	return m;
}

#endif
