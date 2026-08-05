// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The scan engine (docs/spec/radio-features.md 2): when to move on, where to go next, nuisance delete. No hardware and
// no channel reading here: the radio core (radio_sm.c) tunes, reads the codeplug and tells the engine what it hears.
//
// Phases on each stop: LISTEN (settle, then listen for the dwell time) -> PROBE (a carrier but nothing to stop for
// yet: waiting for the RX tone on FM or a call on DMR) -> HOLD (stopped on a signal) -> RESUME (the signal ended:
// waiting the resume delay before moving on). Resume modes: TIME also moves on after the hold time while the signal
// lasts; CARRIER waits for the signal to end; STOP ends the scan on the first signal.
#ifndef RADIO_SCAN_H
#define RADIO_SCAN_H

#include <stdint.h>
#include <stdbool.h>
#include "codeplug/codeplug.h"

typedef enum { SCAN_OFF, SCAN_ZONE, SCAN_VFO, SCAN_DUAL } scan_kind_t;
typedef enum { SCAN_PH_LISTEN, SCAN_PH_PROBE, SCAN_PH_HOLD, SCAN_PH_RESUME, SCAN_PH_PAUSED } scan_phase_t;
typedef enum { SCAN_RESUME_TIME, SCAN_RESUME_CARRIER, SCAN_RESUME_STOP } scan_resume_t;
typedef enum { SCAN_ACT_NONE, SCAN_ACT_NEXT, SCAN_ACT_STOP } scan_act_t;

#define SCAN_SETTLE_MS		50	// readings right after a retune belong to the last stop
#define SCAN_PRIORITY_EVERY	2	// with priority on: the start channel again after this many others
#define SCAN_VFO_NUISANCE_MAX	8	// frequencies a VFO scan can skip

typedef struct {
	uint16_t dwell_ms;	// listen time per stop with nothing heard (from the retune)
	uint16_t probe_ms;	// a carrier without a tone match (FM) or a call (DMR): wait this long for one
	uint16_t hold_ms;	// TIME: move on after this long on a signal, 0 = never
	uint16_t delay_ms;	// after the signal ends, move on after this long
	uint8_t resume;		// scan_resume_t
	bool priority;		// revisit the start channel (zone scans)
} scan_cfg_t;

typedef struct {
	uint8_t kind;		// scan_kind_t
	uint8_t phase;		// scan_phase_t
	uint32_t phase_ms;	// when the phase began
	uint32_t tuned_ms;	// when the current stop was tuned
	scan_cfg_t cfg;
	// Where the scan started (CANCEL goes back there; priority and dual watch revisit it)
	uint16_t origin_pos;
	uint32_t origin_hz;
	bool origin_vfo;
	uint16_t dual_pos;	// SCAN_DUAL: the other channel's zone position
	uint16_t sweep_pos;	// the zone sweep's last position (priority revisits don't move it)
	uint8_t since_priority;	// stops since the start channel
	// Nuisance delete: zone positions, or VFO frequencies, left out until the scan stops
	uint8_t skip[(CODEPLUG_ZONE_MAX + 7) / 8];
	uint32_t skip_hz[SCAN_VFO_NUISANCE_MAX];
	uint8_t skipped;
	// VFO range (inclusive)
	uint32_t lo_hz, hi_hz;
	// Statistics for the debug link
	uint32_t steps;
	uint32_t holds;
} scan_t;

// Whether a zone position can be scanned (readable, not scan_skip); supplied by the radio core
typedef bool (*scan_usable_fn)(uint16_t pos, void *ctx);

void scan_begin(scan_t *s, uint8_t kind, const scan_cfg_t *cfg, uint32_t now_ms);
void scan_end(scan_t *s);
static inline bool scan_on(const scan_t *s) { return s->kind != SCAN_OFF; }

// The radio core tuned a new stop
void scan_tuned(scan_t *s, uint32_t now_ms);
// Every poll while scanning (not while paused). active: a signal worth stopping for (FM: the squelch is open with
// the right tone; DMR: a call that passes the filters is playing). carrier: something on the frequency
scan_act_t scan_poll(scan_t *s, uint32_t now_ms, bool active, bool carrier);
// Monitor held, or anything else that stops the stepping for a while; resume restarts the current stop's listen
void scan_pause(scan_t *s, bool paused, uint32_t now_ms);

// The next zone position of count, skipping nuisance-deleted and unusable ones, with priority revisits (SCAN_ZONE)
// or alternating with the start channel (SCAN_DUAL). -1 when nothing can be scanned
int scan_next_pos(scan_t *s, uint16_t count, scan_usable_fn usable, void *ctx);
// The next VFO frequency after hz by step inside [lo_hz, hi_hz], wrapping, skipping nuisance frequencies. 0 if none
uint32_t scan_next_hz(const scan_t *s, uint32_t hz, uint32_t step);

// Leave this stop out until the scan ends. False if it was already left out or the list is full
bool scan_nuisance_pos(scan_t *s, uint16_t pos);
bool scan_nuisance_hz(scan_t *s, uint32_t hz);
bool scan_pos_skipped(const scan_t *s, uint16_t pos);

#endif
