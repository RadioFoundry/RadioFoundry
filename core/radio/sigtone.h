// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Signaling tones (docs/spec/radio-features.md 4.3): short tone chains played at the start of a transmission, at
// its end and when a reception ends, on the speaker, on air, or both, separately for DMR and FM. Up to SIG_STEPS
// {Hz, ms} steps each, the shape of the Frequency / Period rows of a CPS alert tone page.
//
// This file is pure data and a sample generator: no RTOS, no board. The radio core (radio_sm.c) picks what to play
// and where; beep.c plays the speaker copy; the DMR voice encoder and the FM tone hook carry the on air copy.
#ifndef RADIO_SIGTONE_H
#define RADIO_SIGTONE_H

#include <stdint.h>
#include <stdbool.h>
#include "radio/beep.h"

#define SIG_STEPS	10u	// ten: an eight tone ZVEI or a CPS call tone's five and some to spare
#define SIG_AIR_GUARD_MS 60u	// silence the DMR on air copy carries before a start tone: a link that drops the first
				// frames of a transmission (a hotspot) loses this instead of the tone
#define SIG_AIR_END_GUARD_MS 100u	// the same after an end tone (longer than SIG_AIR_GUARD_MS: a receiver that is slow to
				// close its audio cut the tone's last part now and then at 60)
#define SIG_AIR_STEPS	(SIG_STEPS + 1u)	// a chain and its guard
#define SIG_MAX_MS	900u	// a sequence's total length is cut to this: it holds the transmission or the speaker (on air
				// the call stack's tail hold is three superframes, about 1.08 s)

typedef enum {
	SIG_EV_BEGIN,		// the transmission starts (talk permit)
	SIG_EV_END,		// the transmission ends (roger)
	SIG_EV_IDLE,		// the reception ends, the channel is free
	SIG_EV_COUNT
} sig_event_t;

typedef enum {
	SIG_OUT_DEFAULT,	// what the radio did before this setting existed
	SIG_OUT_OFF,
	SIG_OUT_LOCAL,		// the speaker only
	SIG_OUT_AIR,		// transmitted only
	SIG_OUT_BOTH,
	SIG_OUT_COUNT
} sig_out_t;

// A chain is up to SIG_STEPS steps, each a frequency (Hz, 0 = a pause) and a length in units of SIG_MS_UNIT. A step of
// length 0 ends the chain; a chain with no step is the event's standard sound
#define SIG_MS_UNIT	10u
#define SIG_HZ_MAX	3400u	// inside the voice band the AMBE encoder carries
#define SIG_HZ_UNIT	10u	// frequencies are multiples of this (the settings store a step in 16 bits)

// Whether out sounds on the speaker / goes on air. Idle has no air copy (nothing is being sent)
static inline bool sig_out_local(uint8_t out)
{
	return out == SIG_OUT_LOCAL || out == SIG_OUT_BOTH;
}

static inline bool sig_out_air(uint8_t out, sig_event_t ev)
{
	return ev != SIG_EV_IDLE && (out == SIG_OUT_AIR || out == SIG_OUT_BOTH);
}

// A valid out for the event (idle: no air), else DEFAULT
static inline uint8_t sig_out_clean(uint8_t out, sig_event_t ev)
{
	if (out >= SIG_OUT_COUNT || (ev == SIG_EV_IDLE && (out == SIG_OUT_AIR || out == SIG_OUT_BOTH))) {
		return SIG_OUT_DEFAULT;
	}
	return out;
}

// The chain and the records below are in sigtone.c, which builds with CONFIG_SIG_TONES only (the sample generator is
// the beep engine's, beep.h)

// A chain (as stored: hz[], ms10[] in SIG_MS_UNIT) into out[SIG_STEPS]; the count. An empty chain gives the event's
// standard sound, so there is always at least one step. A frequency over SIG_HZ_MAX is cut to it; the total length is
// within SIG_MAX_MS
uint8_t sig_chain(const uint16_t hz[SIG_STEPS], const uint8_t ms10[SIG_STEPS], sig_event_t ev,
		  beep_seg_t out[SIG_STEPS]);

// The settings area's record of one event (docs/spec/codeplug-settings.md 3): the outputs and the chain, the fields in
// this order. A record shorter than SIG_REC_LEN reads with the missing fields zero (Default, no step); a longer one
// (a later version) is read from the front
// The first SIG_REC_BASE steps are laid out as the first version's five (a record written then reads unchanged), the
// rest follow: hz[SIG_REC_BASE..] and ms10[SIG_REC_BASE..]
#define SIG_REC_BASE	5u
#define SIG_REC_LEN	(2u + 3u * SIG_STEPS)
typedef struct {
	uint8_t dmr, fm;			// sig_out_t
	uint16_t hz[SIG_STEPS];
	uint8_t ms10[SIG_STEPS];		// in SIG_MS_UNIT
} sig_rec_t;
// A value as read (len bytes, v holds at most SIG_REC_LEN of them) as the event's record: false when the setting
// can't take it (an output the event doesn't have, a frequency off the SIG_HZ_UNIT grid or over the limit, a step over
// the length limit), which reads as an
// absent key. Nothing is clamped
bool sig_rec_decode(const uint8_t *v, int len, sig_event_t ev, sig_rec_t *r);
void sig_rec_encode(const sig_rec_t *r, uint8_t out[SIG_REC_LEN]);


#endif
