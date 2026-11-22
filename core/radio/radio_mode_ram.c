// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The digital voice modes' shared engine RAM (radio_mode_ram.h)
#include <string.h>
#include "radio/radio_mode_ram.h"

#if CONFIG_DSTAR || CONFIG_YSF
#if CONFIG_DSTAR
#include "radio/radio_dstar.h"
#endif
#if CONFIG_YSF
#include "radio/radio_ysf.h"
#endif

// Sized and aligned by what each mode keeps there, so a change to either moves the area with it. The uint64_t member
// is the alignment the take promises whatever they hold
static union {
	uint64_t align;
#if CONFIG_DSTAR
	radio_dstar_area_t dstar;	// D-STAR's engine and its codec's state (the codec's working memory is the lend's)
#endif
#if CONFIG_YSF
	radio_ysf_area_t ysf;		// Fusion's engine and Voice FR's codec state
#endif
} area;

static volatile uint8_t owner = RADIO_MODE_RAM_FREE;

void *radio_mode_ram_take(radio_mode_ram_owner_t who, size_t bytes)
{
	if (who == RADIO_MODE_RAM_FREE || bytes > sizeof area || (owner != RADIO_MODE_RAM_FREE && owner != who)) {
		return NULL;
	}
	if (owner == RADIO_MODE_RAM_FREE) {
		// Zeroed for each new holder: every engine reads all zeros as idle, so a mode that looks at its engine before
		// starting it never reads another mode's bytes
		memset(&area, 0, sizeof area);
		owner = (uint8_t)who;
	}
	return &area;
}

void radio_mode_ram_give(radio_mode_ram_owner_t who)
{
	if (owner == who) {
		owner = RADIO_MODE_RAM_FREE;
	}
}

radio_mode_ram_owner_t radio_mode_ram_owner(void)
{
	return (radio_mode_ram_owner_t)owner;
}

size_t radio_mode_ram_size(void)
{
	return sizeof area;
}

#endif
