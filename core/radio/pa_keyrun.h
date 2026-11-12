// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The PA key run (docs/spec/pa-keyup.md 7): a calibration's keys, each at its own drive, keyed one after another by the
// radio itself from one debug link request. A radio whose own RF upsets its USB link while it transmits still runs
// every key in time, and the host needs the link only before and after. The keys are read from the request as they
// run: nothing is kept in RAM
#ifndef RFOUNDRY_PA_KEYRUN_H
#define RFOUNDRY_PA_KEYRUN_H

#include <stdbool.h>
#include <stdint.h>
#include "radio/pa_keyup.h"

#define PA_KEYRUN_HEAD		6u	// key_ms u16, gap_ms u16, flags u8, keys u8
#define PA_KEYRUN_CFG		14u	// the key-up constants, as RADIO op 37 takes them (seven u16)
#define PA_KEYRUN_KEY_MS_MAX	2000u	// one key's length
#define PA_KEYRUN_GAP_MS_MIN	100u	// the radio's key-down and the next key's set-up
#define PA_KEYRUN_GAP_MS_MAX	2000u
#define PA_KEYRUN_FLAG_CFG	1u	// the constants follow the head, set before the first key (RAM, as op 37)
#define PA_KEYRUN_FLAG_REATTACH	2u	// after the run and its reply the host port leaves the bus and comes back
#define PA_KEYRUN_FLAG_RESTART	4u	// after the run and its reply the radio restarts: what a USB port its own RF
					// upset needs on some radios (a reattach isn't enough); the constants set are lost
#define PA_KEYRUN_END_MS	1000u	// how long a key-down may take before the run stops

typedef struct {
	uint16_t key_ms, gap_ms;
	uint8_t n;			// keys
	bool has_cfg;
	bool reattach;			// PA_KEYRUN_FLAG_REATTACH
	bool restart;			// PA_KEYRUN_FLAG_RESTART
	pa_keyup_cfg_t cfg;
	const uint8_t *drives;		// n u16, little-endian, in the request
} pa_keyrun_req_t;

// What a run keys with: the board's drive override (hal_radio_t.tx_drive), the key-up constants (pa_keyup_set), an
// injected PTT held at most max_ms (input_inject_ptt), whether the radio transmits now, and a delay
typedef struct {
	bool (*drive)(uint16_t drive);
	bool (*keyup)(const pa_keyup_cfg_t *c);
	bool (*ptt)(bool down, uint32_t max_ms);
	bool (*keyed)(void);
	void (*delay_ms)(uint32_t ms);
} pa_keyrun_io_t;

// The request's arguments (after the op): key_ms u16, gap_ms u16, flags u8, n u8, [the constants], n drives u16.
// False: a length or a limit is wrong
bool pa_keyrun_parse(const uint8_t *a, uint16_t len, pa_keyrun_req_t *r);

// Runs r: per key the drive, PTT for key_ms (its time-out just past it), the release, the key-down awaited, gap_ms.
// Returns the keys run; it stops at a refused write or PTT, or a key-down that doesn't come. The drive override is 0
// again at the end
uint8_t pa_keyrun_run(const pa_keyrun_req_t *r, const pa_keyrun_io_t *io);

#endif
