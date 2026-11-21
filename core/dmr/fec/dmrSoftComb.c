// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors

#include "config.h"

#if CONFIG_DMR

#include <string.h>
#include "dmr/fec/dmrSoftComb.h"
#include "dmr/fec/dmrFec.h"

// Info bit i's place in the burst: bits 0-97, then 166-263 (dmrFec.h)
static int burstBitOf(int i)
{
	return i < 98 ? i : i + 68;
}

static void hardBurst(const int8_t *v, uint8_t *burst)
{
	memset(burst, 0, DMR_FEC_BURST_BYTES);
	for (int i = 0; i < DMR_SOFT_BITS; i++) {
		if (v[i] > 0) {
			int b = burstBitOf(i);
			burst[b / 8] |= (uint8_t)(0x80u >> (b % 8));
		}
	}
}

static int hardDistance(const int8_t *a, const int8_t *b)
{
	int d = 0;
	for (int i = 0; i < DMR_SOFT_BITS; i++) {
		d += (a[i] > 0) != (b[i] > 0);
	}
	return d;
}

// How well payload's codeword (reserved bits r) matches the soft values: the sum of each value signed by its bit
static int32_t score(const int8_t *v, const uint8_t *payload, uint8_t r, uint8_t *burst)
{
	memset(burst, 0, DMR_FEC_BURST_BYTES);
	dmrFecBptcEncodeR(payload, r, burst);
	int32_t s = 0;
	for (int i = 0; i < DMR_SOFT_BITS; i++) {
		int b = burstBitOf(i);
		s += ((burst[b / 8] >> (7 - (b % 8))) & 1u) ? v[i] : -v[i];
	}
	return s;
}

void dmrSoftCombInit(dmrSoftComb_t *c)
{
	memset(c, 0, sizeof(*c));
}

int8_t *dmrSoftCombStage(dmrSoftComb_t *c, uint8_t *tag)
{
	if (++c->next_tag == 0) {
		c->next_tag = 1;
	}
	uint8_t i = c->next_tag % DMR_SOFT_STAGES;
	c->stage_tag[i] = c->next_tag;
	*tag = c->next_tag;
	return c->stage[i];
}

// The set this burst joins (summed into it), or a new one in place of the empty or longest unused set. Returns the
// set joined, NULL for a new one
static dmrSoftSet_t *join(dmrSoftComb_t *c, dmrSoftSet_t *sets, const int8_t *l)
{
	dmrSoftSet_t *near = NULL, *old = &sets[0];
	int nd = DMR_SOFT_JOIN + 1;
	for (int k = 0; k < DMR_SOFT_SETS; k++) {
		dmrSoftSet_t *s = &sets[k];
		if (s->n == 0) {
			if (old->n != 0) {
				old = s;
			}
			continue;
		}
		if (old->n != 0 && (uint8_t)(c->clock - s->used) > (uint8_t)(c->clock - old->used)) {
			old = s;
		}
		int d = hardDistance(s->v, l);
		if (d < nd) {
			nd = d;
			near = s;
		}
	}
	if (near == NULL) {
		memcpy(old->v, l, DMR_SOFT_BITS);
		old->n = 1;
		old->used = c->clock;
		return NULL;
	}
	for (int i = 0; i < DMR_SOFT_BITS; i++) {
		int v = near->v[i] + l[i];
		near->v[i] = (int8_t)(v > 127 ? 127 : v < -127 ? -127 : v);
	}
	near->used = c->clock;
	if (++near->n >= DMR_SOFT_HALVE) {
		for (int i = 0; i < DMR_SOFT_BITS; i++) {
			near->v[i] = (int8_t)(near->v[i] / 2);
		}
		near->n = DMR_SOFT_HALVE / 2;
	}
	return near;
}

bool dmrSoftCombTake(dmrSoftComb_t *c, uint8_t tag, uint8_t slot, uint8_t *payload)
{
	uint8_t i = tag % DMR_SOFT_STAGES;
	if (tag == 0 || slot >= DMR_SOFT_SLOTS) {
		return false;
	}
	if (c->stage_tag[i] != tag) {
		c->st.stale++;
		return false;
	}
	const int8_t *l = c->stage[i];
	c->st.taken++;
	c->clock++;
	const dmrSoftSet_t *set = join(c, c->set[slot], l);
	if (c->stage_tag[i] != tag) {
		c->st.stale++;	// staged again while taken: a burst that came in between (not seen with two stages)
		return false;
	}
	if (set == NULL) {
		return false;	// a new set: the burst alone. Its own decode isn't offered: through heavy noise BPTC miscorrects
				// to a codeword that fits the soft values better than the chip's right decode (dmr-modem.md 2.2.1)
	}
	uint8_t burst[DMR_FEC_BURST_BYTES], comb[DMR_FEC_BPTC_PAYLOAD_BYTES], r = 0;
	hardBurst(set->v, burst);
	if (dmrFecBptcDecodeR(burst, comb, &r) < 0) {
		return false;
	}
	c->st.combined++;
	// Both against the sum, with the reserved bits the decode found. The chip's decode is kept on a tie: it may have
	// used more than these values (its own soft decoding)
	if (memcmp(comb, payload, DMR_FEC_BPTC_PAYLOAD_BYTES) == 0 ||
	    score(set->v, comb, r, burst) <= score(set->v, payload, r, burst)) {
		return false;
	}
	memcpy(payload, comb, DMR_FEC_BPTC_PAYLOAD_BYTES);
	c->st.changed++;
	return true;
}

static void statsAdd(dmrSoftStats_t *to, const dmrSoftStats_t *from)
{
	to->taken += from->taken;
	to->combined += from->combined;
	to->changed += from->changed;
	to->stale += from->stale;
}

void dmrSoftHoldSet(dmrSoftHold_t *h, bool on)
{
	if (on && *h->at == NULL) {
		dmrSoftComb_t *c = h->ops->alloc(sizeof(*c));
		if (c != NULL) {
			dmrSoftCombInit(c);
			*h->at = c;
		}
	} else if (!on && *h->at != NULL) {
		dmrSoftComb_t *c = *h->at;
		h->ops->lock(NULL);
		*h->at = NULL;
		statsAdd(&h->done, &c->st);
		h->ops->unlock(NULL);
		h->ops->release(c);
	}
}

void dmrSoftHoldStats(dmrSoftHold_t *h, dmrSoftStats_t *st)
{
	h->ops->lock(NULL);
	*st = h->done;
	dmrSoftComb_t *c = *h->at;
	if (c != NULL) {
		statsAdd(st, &c->st);
	}
	h->ops->unlock(NULL);
}

#endif
