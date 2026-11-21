// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
/*
 * Soft combining of repeated BPTC bursts (dmr-modem.md 2.2.1). A control channel sends the same few CSBKs over and over
 * (Alohas, broadcasts), so a burst too damaged to decode alone often decodes when summed with the copies heard before
 * it. Per slot a few sets of summed soft values are kept; a burst joins the set whose hard bits are closest, and the
 * set is decoded. The modem driver stages the soft values of a data burst whose CRC failed (on its interrupt, if it
 * must: staging only copies), then calls dmrSoftCombTake at task level with the payload the chip gave. That payload is
 * replaced only by a decode whose codeword matches the soft values better. A wrong decode doesn't repeat (the sets keep
 * changing), so the accept invalid CRC rule (rx_seen.h) still holds for what this gives.
 *
 * Soft values: one int8 per info bit, the 196 info bits in burst order (bits 0-97, then 166-263), positive for a 1 and
 * the larger the surer. The caller owns the memory (about 2 KB): the modem glue allocates it while DMR runs.
 */

#ifndef RFOUNDRY_DMR_SOFT_COMB_H
#define RFOUNDRY_DMR_SOFT_COMB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DMR_SOFT_BITS		196
#define DMR_SOFT_SLOTS		2
#define DMR_SOFT_SETS		4	// per slot: a control channel's rotation of messages
#define DMR_SOFT_STAGES		2	// bursts staged and not yet taken
#define DMR_SOFT_JOIN		50	// a burst joins a set at most this many hard bits from it
#define DMR_SOFT_HALVE		6	// a set halves after this many bursts, so it follows a message that changes

typedef struct {
	int8_t v[DMR_SOFT_BITS];
	uint8_t n;	// bursts in the sum since the last halving; 0 = empty
	uint8_t used;	// the take count when it was last joined, to replace the one unused longest
} dmrSoftSet_t;

typedef struct {
	uint32_t taken;		// bursts taken
	uint32_t combined;	// joined a set and the set decoded (BPTC clean)
	uint32_t changed;	// the chip's payload replaced
	uint32_t stale;		// staged values overwritten before they were taken
} dmrSoftStats_t;

typedef struct {
	dmrSoftSet_t set[DMR_SOFT_SLOTS][DMR_SOFT_SETS];
	int8_t stage[DMR_SOFT_STAGES][DMR_SOFT_BITS];
	volatile uint8_t stage_tag[DMR_SOFT_STAGES];
	uint8_t next_tag;
	uint8_t clock;
	dmrSoftStats_t st;
} dmrSoftComb_t;

void dmrSoftCombInit(dmrSoftComb_t *c);

// A staging buffer for one burst's soft values and its tag (never 0). Fill it before the burst's event can be taken.
// Only copies: callable from an interrupt
int8_t *dmrSoftCombStage(dmrSoftComb_t *c, uint8_t *tag);

// The staged burst tag, on slot 0 or 1, combined: payload (12 bytes) holds the chip's decode and is replaced when a
// decode matches the soft values better. Returns true when it was replaced. The reserved bits R(3)-R(0) a decode
// carried (non-zero on some systems) are kept for the comparison
bool dmrSoftCombTake(dmrSoftComb_t *c, uint8_t tag, uint8_t slot, uint8_t *payload);

// A combiner held only while DMR runs, for the modem glue: what the memory comes from, and the lock that keeps the
// diag line's read of the counters off a combiner being freed (the glue's own, ctx NULL)
typedef struct {
	void *(*alloc)(size_t size);
	void (*release)(void *p);
	void (*lock)(void *ctx);
	void (*unlock)(void *ctx);
} dmrSoftHoldOps_t;

typedef struct {
	dmrSoftComb_t *volatile *at;	// the engine's pointer: the combiner while held, NULL otherwise
	const dmrSoftHoldOps_t *ops;
	dmrSoftStats_t done;		// the counters of the combiners freed
} dmrSoftHold_t;

// Holds a combiner (on) or frees it (off). A failed allocation leaves none, and nothing is combined. Only from the task
// that combines, so a combiner is never freed under dmrSoftCombTake
void dmrSoftHoldSet(dmrSoftHold_t *h, bool on);
// The counters since boot: the freed combiners' and the held one's
void dmrSoftHoldStats(dmrSoftHold_t *h, dmrSoftStats_t *st);

#endif
