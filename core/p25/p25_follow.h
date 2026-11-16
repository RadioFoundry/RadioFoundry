// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A receive-only P25 trunking follower (docs/spec/p25-trunking.md): it hunts for a system's control channel, holds
// it while its signaling decodes, and follows voice channel grants for the talk groups it is given, handing the voice
// channel to the conventional call layer (p25_engine.h) and coming back when the call ends. Build switch
// CONFIG_P25_TRUNK.
//
// Pure: no RTOS call, no blocking, no transmit path of any kind. Time comes in as a millisecond tick on every call;
// tuning goes out through the caller's hook. All calls come from one task (the provider's, where units arrive, is
// the natural one; tick it from there too), or under one lock. The state is one structure the caller places.
#ifndef RFOUNDRY_P25_FOLLOW_H
#define RFOUNDRY_P25_FOLLOW_H

#include <stdbool.h>
#include <stdint.h>

#include "p25_tsbk.h"

#define P25_FOLLOW_WACN_ANY	0xFFFFFFu	// any value over 20 bits
#define P25_FOLLOW_SYSID_ANY	0xFFFFu		// any value over 12 bits
#define P25_FOLLOW_IDENS	16
#define P25_FOLLOW_LEARN	4		// control channels learned off air (secondary, adjacent sites)

// Timers, ms
#define P25_FOLLOW_DWELL_MS	600	// a control channel candidate with nothing decoded: the next
#define P25_FOLLOW_ID_MS	4000	// ... with signaling but not yet the system's identity
#define P25_FOLLOW_CC_LOST_MS	1000	// nothing decoded on the control channel held: hunt again
#define P25_FOLLOW_BACKOFF_MS	2000	// after a pass with no control channel, the wait on the first, doubled each pass
#define P25_FOLLOW_BACKOFF_MAX	30000
#define P25_FOLLOW_REJOIN_MS	1500	// after a call's terminator, its updates are not followed again for this long
#define P25_FOLLOW_VOICE_LOST_MS 1000	// defaults for the cfg's 0
#define P25_FOLLOW_CALL_MAX_MS	120000

// p25_follow_cfg_t.flags
#define P25_FOLLOW_SKIP_ENC	0x01	// grants and calls marked encrypted are not followed
#define P25_FOLLOW_PLAN_ONLY	0x02	// only the programmed identifier plan: identifiers off air are not learned
#define P25_FOLLOW_ALL_GROUPS	0x04	// every talk group of the system
#define P25_FOLLOW_NO_UNIT	0x08	// unit to unit calls to or from unit_id are not followed

// A control channel to hunt: its receive frequency, or its channel number through the identifier table
typedef struct {
	uint32_t hz;		// 0: from chan
	uint16_t chan;		// identifier << 12 | channel; 0 none
	uint8_t site;		// for the view (0xFF none)
} p25_follow_cc_t;

// A programmed channel identifier
typedef struct {
	uint32_t base_hz;
	uint32_t spacing_hz;	// a multiple of 5 Hz, at most 327675
	uint8_t slots;		// channels per carrier (TDMA), 0 or 1 = FDMA
	bool tdma;		// not playable here: grants on it are refused
} p25_follow_iden_t;

typedef struct {
	uint32_t wacn;		// P25_FOLLOW_WACN_ANY: any
	uint16_t sysid;		// P25_FOLLOW_SYSID_ANY: any
	uint16_t nac;		// of the control channel; P25_NAC_ANY: any
	uint8_t flags;		// P25_FOLLOW_*
	uint32_t unit_id;	// ours; 0 none
	uint16_t talk_group;	// 0 none
	uint8_t ngroups;	// more talk groups followed
	const uint16_t *groups;
	uint8_t ncc;		// control channels in hunt order, the first the primary
	const p25_follow_cc_t *cc;
	uint16_t iden_set;	// bit n: iden[n] is programmed
	const p25_follow_iden_t *iden;	// P25_FOLLOW_IDENS entries when iden_set
	uint32_t hold_ms;	// after a followed group call, only its talk group is followed for this long; 0 none
	uint32_t voice_lost_ms;	// no voice for this long on a voice channel: back; 0 default
	uint32_t call_max_ms;	// on a voice channel at most; 0 default
	uint32_t fallback_ms;	// no control channel for this long: the fallback hook; 0 never
} p25_follow_cfg_t;

// Tune: a control channel (id 0), a group call (id = talk group) or a unit to unit call (id = the called unit)
enum { P25_FOLLOW_TUNE_CONTROL, P25_FOLLOW_TUNE_GROUP, P25_FOLLOW_TUNE_UNIT };

typedef struct {
	// Retune the receiver to hz. Units still in the pipe are of the old channel: call p25_follow_retuned once the
	// receiver is on the new one (from inside the hook too, if the caller retunes at once). On a voice channel the
	// caller sets the engine's selective squelch to id for a group call
	void (*tune)(uint32_t hz, uint8_t what, uint32_t id, void *ctx);
	// No control channel for cfg.fallback_ms: the caller may move to its fallback channel (once per loss)
	void (*fallback)(void *ctx);
} p25_follow_hooks_t;

enum { P25_FOLLOW_OFF, P25_FOLLOW_HUNT, P25_FOLLOW_CONTROL, P25_FOLLOW_VOICE };
// Why the last grant we wanted was not followed
enum {
	P25_REFUSE_NONE,
	P25_REFUSE_ENC,		// encrypted, and cfg says skip
	P25_REFUSE_TDMA,	// a two-slot (TDMA) voice channel: not playable here
	P25_REFUSE_NO_IDEN,	// its channel identifier is not known (yet)
};

typedef struct {
	uint8_t state;		// P25_FOLLOW_*
	uint8_t cc;		// the control channel tried or held: index into cfg.cc, then the learned ones; 0xFF none
	bool fallback;		// the fallback was reported for this loss
	uint8_t refused;	// P25_REFUSE_* of the last grant not followed
	uint16_t refused_tg;
	uint16_t nac;		// of the control channel held
	uint32_t wacn;		// 0xFFFFFFFF until heard
	uint16_t sysid;		// 0xFFFF until heard
	uint8_t rfss, site;	// 0xFF until heard
	uint32_t cc_hz, voice_hz;
	// The call followed (now, or last)
	bool unit_call, enc;
	uint16_t tg;
	uint32_t src, dst;
	uint16_t idens;		// bit n: identifier n known (programmed or learned)
	uint8_t learned;	// control channels learned
	uint8_t mbt_op;		// the last multi-block trunking opcode
	uint32_t blocks, crc_fail;
	uint16_t mfr, mbt, pdu_other, grants, follows, enc_skip, tdma, no_iden, wrong_sys, hunts, cc_lost, voice_lost,
		 wrong_call;
	uint8_t op[P25_OPCODES];	// blocks of the standard's table by opcode (saturating at 255)
} p25_follow_view_t;

typedef struct {
	uint32_t base_hz;
	uint16_t spacing5;	// x 5 Hz
	uint8_t slots;
	uint8_t flags;		// IDEN_xxx in p25_follow.c
} p25_follow_iden_e;

typedef struct {
	const p25_follow_cfg_t *cfg;		// the caller's, kept until p25_follow_end
	const p25_follow_hooks_t *hooks;
	void *ctx;
	uint32_t t_state;	// the step in hand began (a dwell, an identity wait, a backoff)
	uint32_t t_good;	// the last block decoded on the control channel, or the last voice unit
	uint32_t t_nocc;	// no control channel since
	uint32_t t_call;	// on the voice channel since
	uint32_t t_hold;	// the talk group hold began
	uint32_t t_left;	// left a call on its terminator
	uint32_t backoff;	// ms
	uint16_t hold_tg, left_tg, left_ch;
	uint16_t learn[P25_FOLLOW_LEARN];
	uint8_t nlearn;
	uint8_t good;		// the last control channel that answered, 0xFF none
	uint8_t tried;		// candidates tried in this pass
	uint8_t phase;		// in a hunt: dwell, identity wait, backoff
	bool left_enc;		// left_ch / left_tg is an encrypted call skipped
	bool tuning;		// a tune was asked: units are of the old channel until p25_follow_retuned
	bool have_id;		// the candidate's system identity matched
	volatile uint8_t seq;	// odd while view is written
	p25_follow_iden_e iden[P25_FOLLOW_IDENS];
	p25_follow_view_t view;
} p25_follow_t;

// Begin hunting (the first tune is asked from here). cfg and hooks stay the caller's until end
void p25_follow_begin(p25_follow_t *f, const p25_follow_cfg_t *cfg, const p25_follow_hooks_t *hooks, void *ctx,
		      uint32_t now);
void p25_follow_end(p25_follow_t *f);
// The receiver is on the frequency of the last tune
void p25_follow_retuned(p25_follow_t *f, uint32_t now);
// A unit the receiver took (p25_engine_hooks_t.rx_unit), and a call's fields (rx_call)
void p25_follow_unit(p25_follow_t *f, const p25_rx_unit_t *u, uint32_t now);
void p25_follow_call(p25_follow_t *f, const p25_call_t *c, uint32_t now);
// One block whose CRC held, from a control channel of this NAC (p25_follow_unit calls it for each)
void p25_follow_tsbk(p25_follow_t *f, const uint8_t blk[P25_TSBK_BYTES], uint16_t nac, uint32_t now);
// Timers: call at least every 100 ms
void p25_follow_tick(p25_follow_t *f, uint32_t now);
// A copy of the view for another task
void p25_follow_view(const p25_follow_t *f, p25_follow_view_t *out);

#endif
