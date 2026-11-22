// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The emergency wipe (docs/spec/zeroize.md, CONFIG_ZEROIZE): a 4-digit wipe code kept as a salted hash in the settings
// store, the key sequence that types it (four digits and #, on Home or while the keys are locked), and the wipe: the
// keys erased from the flash (sector erases, every old copy included) and zeroed in RAM, then the codeplug when the
// setting says so. The key sequence is header only, so the UI and its host tests need nothing else of this module
#ifndef RFOUNDRY_ZEROIZE_H
#define RFOUNDRY_ZEROIZE_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "config.h"

#define ZEROIZE_CODE_LEN	4
#define ZEROIZE_HASH_LEN	16u
#define ZEROIZE_TRIES_MAX	15u	// settings_t.zz_flags bits 4-7

// --- The key sequence (3): digits typed one by one, then # ---
typedef struct {
	char d[ZEROIZE_CODE_LEN];
	uint8_t n;		// digits typed so far; ZEROIZE_CODE_LEN + 1: too many, no code
} zeroize_seq_t;

static inline void zeroize_seq_reset(zeroize_seq_t *s)
{
	memset(s, 0, sizeof(*s));
}

static inline void zeroize_seq_digit(zeroize_seq_t *s, char d)
{
	if (s->n < ZEROIZE_CODE_LEN) {
		s->d[s->n] = d;
	}
	if (s->n <= ZEROIZE_CODE_LEN) {
		s->n++;
	}
}

// # typed: true with exactly ZEROIZE_CODE_LEN digits before it, copied to out. The sequence starts over either way
static inline bool zeroize_seq_end(zeroize_seq_t *s, char out[ZEROIZE_CODE_LEN])
{
	bool four = s->n == ZEROIZE_CODE_LEN;
	if (four) {
		memcpy(out, s->d, ZEROIZE_CODE_LEN);
	}
	zeroize_seq_reset(s);
	return four;
}

#if CONFIG_ZEROIZE
// --- The code (2) ---
// The hash of code under salt: the first ZEROIZE_HASH_LEN octets of SHA-256("RFZZ1" | UID | salt | code)
void zeroize_hash(uint32_t salt, const char code[ZEROIZE_CODE_LEN], uint8_t out[ZEROIZE_HASH_LEN]);
// Set (a new salt, its hash) or clear the code in the settings store; false when code isn't four digits
bool zeroize_code_set(const char code[ZEROIZE_CODE_LEN]);
void zeroize_code_clear(void);
bool zeroize_code_is_set(void);
// The wipe code (constant time); false with no code set
bool zeroize_code_check(const char code[ZEROIZE_CODE_LEN]);

// --- The options (3.1) ---
bool zeroize_keys_only(void);
bool zeroize_confirm(void);
uint8_t zeroize_tries(void);		// wrong codes while locked before a wipe, 0 off
void zeroize_set_keys_only(bool on);
void zeroize_set_confirm(bool on);
void zeroize_set_tries(uint8_t n);

// --- The wipe (4) ---
// Steps that failed (zeroize_run)
enum {
	ZEROIZE_FAIL_KEYS	= 0x01,	// the codeplug settings area's keys
	ZEROIZE_FAIL_SETTINGS	= 0x02,	// the settings ring, the journal, the staging area
	ZEROIZE_FAIL_CODEPLUG	= 0x04,	// the open-firmware codeplug's regions
	ZEROIZE_FAIL_NATIVE	= 0x08,	// the native area
	ZEROIZE_FAIL_PURGE	= 0x10,	// a copy on write flash's old copies
	ZEROIZE_FAIL_RADIO	= 0x20,	// the radio core didn't say its keys were cleared in time
};
// The whole wipe but the restart, on the housekeeping task (a storage job): the keys in the flash, the codeplug unless
// keys_only, the old copies of a copy on write flash, then the keys in RAM. Every step runs whatever failed before
// it. The ZEROIZE_FAIL_* bits of the steps that failed, 0 when all went
uint8_t zeroize_run(bool keys_only);

// A feature that keeps keys in RAM of its own zeroes them here. Weak, empty by default; key fill (CONFIG_KEY_FILL)
// defines it in core/ui/ui_svc_keyfill.c. Called by zeroize_run on the housekeeping task after the flash steps, with no
// lock held: no long waits, no storage lock (docs/spec/zeroize.md 4.2)
void zeroize_ram_hook(void);
#endif

#endif
