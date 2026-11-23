// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Fox mode (CONFIG_FOX, docs/spec/radio-features.md 3.8): the radio as a fox-hunt transmitter. On a timer it keys the
// FM channel, sends a message in Morse, plays a jingle (a tone chain, as the signaling tones', sigtone.h) and IDs with
// the callsign in Morse, then waits and repeats.
//
// This file is the fox's record (the codeplug key that holds what it sends) and its schedule: pure, no RTOS, no board.
// The radio core (radio_sm.c) keys the transmitter, plays each part and tells the schedule when a part is over.
#ifndef RADIO_FOX_H
#define RADIO_FOX_H

#include <stdint.h>
#include <stdbool.h>
#include "radio/beep.h"

#define FOX_STEPS		32u	// the jingle's steps: a short tune, or a long homing note as a few steps
#define FOX_MAX_MS		10000u	// the jingle's total length is cut to this
#define FOX_MSG_MAX		16u	// the message's characters (Morse letters, digits, punctuation and spaces)
#define FOX_INTERVALS		5u	// interval codes 0..4 (fox_interval_ms)
#define FOX_INTERVAL_DEFAULT	1u	// 1 min
#define FOX_LEAD_MS		300u	// silent carrier before the first part: receivers' squelch opens
#define FOX_GAP_MS		600u	// silent carrier between two parts
#define FOX_TAIL_MS		300u	// and after the last
#define FOX_BUSY_RETRY_MS	5000u	// the channel busy at key-up (busy channel lockout): try again this much later
#define FOX_CYCLE_MAX_MS	60000u	// the longest key-up on a channel with no TX time-out

// What a cycle holds besides the ID (fox_rec_t.flags; the ID is always sent)
#define FOX_F_MSG		0x01u
#define FOX_F_JINGLE		0x02u
#define FOX_F_MASK		0x03u

// The record (docs/spec/codeplug-settings.md 3.4): the interval code, the flags, the message (NUL padded, not
// terminated when FOX_MSG_MAX long) and the jingle's steps, a frequency in Hz (a multiple of SIG_HZ_UNIT, 0 = a pause)
// and a length in SIG_MS_UNIT (a step of length 0 ends it; no step = the default tune)
#define FOX_REC_LEN		(2u + FOX_MSG_MAX + 3u * FOX_STEPS)
typedef struct {
	uint8_t interval;
	uint8_t flags;
	char msg[FOX_MSG_MAX];
	uint16_t hz[FOX_STEPS];
	uint8_t ms10[FOX_STEPS];
} fox_rec_t;

// The settings a radio with no record has: 1 min, message "FOX" and the jingle on, the default tune
void fox_rec_default(fox_rec_t *r);
// A value as read (len bytes, at most FOX_REC_LEN of them looked at) as the record: false when the fox can't take it
// (an interval code it doesn't know, a flag it doesn't know, a character with no Morse, a frequency off the
// SIG_HZ_UNIT grid or over SIG_HZ_MAX), which reads as an absent key. A shorter record reads with the missing fields
// zero. Nothing is clamped
bool fox_rec_decode(const uint8_t *v, int len, fox_rec_t *r);
void fox_rec_encode(const fox_rec_t *r, uint8_t out[FOX_REC_LEN]);

// The interval for a code: 30 s, 1, 2, 5 or 10 min (0 for an unknown code)
uint32_t fox_interval_ms(uint8_t code);
// The message's length (up to the first NUL)
uint8_t fox_msg_len(const fox_rec_t *r);
// Whether a character may be in the message: one with Morse (morse_code), or a space
bool fox_msg_char_ok(char c);
// The jingle as steps into out[FOX_STEPS]: the record's, or the default tune when it has none. The count (at least 1)
uint8_t fox_jingle(const fox_rec_t *r, beep_seg_t out[FOX_STEPS]);
// How long one cycle keeps the transmitter keyed, lead-in to tail, at wpm with call as the ID: what the radio core
// checks against the TX time-out before it starts
uint32_t fox_cycle_ms(const fox_rec_t *r, const char *call, uint8_t call_len, uint8_t wpm);

// --- The schedule ---

typedef enum {
	FOX_OFF,		// not running
	FOX_WAIT,		// receiving until the next cycle (at)
	FOX_LEAD,		// keyed, a silent carrier until at
	FOX_MSG,		// the message in Morse, until the part is done
	FOX_GAP_JINGLE,		// a silent carrier before the jingle, until at
	FOX_JINGLE,		// the jingle, until the part is done
	FOX_GAP_ID,		// a silent carrier before the ID, until at
	FOX_ID,			// the callsign in Morse, until the part is done
	FOX_TAIL,		// a silent carrier until at, then the unkey
	FOX_PHASES
} fox_phase_t;

typedef enum {
	FOX_ACT_NONE,		// nothing new
	FOX_ACT_KEY,		// a cycle is due: key up (then radio_fox_keyed, radio_fox_retry or radio_fox_cut)
	FOX_ACT_QUIET,		// a gap or the tail started: a silent carrier
	FOX_ACT_MSG,		// the message starts
	FOX_ACT_JINGLE,		// the jingle starts
	FOX_ACT_ID,		// the ID starts
	FOX_ACT_UNKEY,		// the cycle is over: unkey (the schedule waits for the next one)
} fox_act_t;

typedef struct {
	uint8_t phase;		// fox_phase_t
	uint8_t parts;		// FOX_F_* this run sends
	uint32_t interval_ms;	// from the end of a cycle to the start of the next
	uint32_t at;		// the end of a timed phase (WAIT, LEAD, the gaps, TAIL)
	uint32_t cycles;	// cycles sent to the end
	uint32_t cut;		// cycles cut short (the transmitter refused or stopped)
} radio_fox_t;

// Start: the first cycle is due at once. parts: FOX_F_* (the ID is always sent)
void radio_fox_start(radio_fox_t *f, uint8_t parts, uint32_t interval_ms, uint32_t now_ms);
// Stop (the counts stay for the view)
void radio_fox_stop(radio_fox_t *f);
// Every few ms while running. part_done: the message, jingle or ID that plays now has ended. What to do now
fox_act_t radio_fox_poll(radio_fox_t *f, bool part_done, uint32_t now_ms);
// After FOX_ACT_KEY: keyed (the lead-in starts), refused for now (the channel busy: due again FOX_BUSY_RETRY_MS later)
// or refused or cut (counted; the next cycle is due an interval later)
void radio_fox_keyed(radio_fox_t *f, uint32_t now_ms);
void radio_fox_retry(radio_fox_t *f, uint32_t now_ms);
void radio_fox_cut(radio_fox_t *f, uint32_t now_ms);
// Whether the transmitter is keyed for a cycle (a phase from LEAD to TAIL)
bool radio_fox_on_air(const radio_fox_t *f);
// The ms left until the next cycle while waiting, else 0
uint32_t radio_fox_wait_left(const radio_fox_t *f, uint32_t now_ms);

#endif
