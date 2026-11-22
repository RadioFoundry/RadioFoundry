// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// One RAM area for the digital voice modes that run on the flat FM paths (D-STAR, Fusion), lent by the radio core to
// the one that runs (docs/spec/dstar-modem.md 1.5, ysf-modem.md 1.4).
//
// A channel has one kind, and tune() stops every mode before it starts one, so at most one is ever live. What each
// keeps here: D-STAR its engine (about 670 B) and its software codec's state from frame to frame (3.2 KB; the codec's
// working memory is the DMR vocoder's, lent: hal_vocoder_t.lend, 7 KB on the HR_C7000, which the whole codec no
// longer fits); Fusion its engine (about 970 B), and Voice FR's codec state (about 4 KB) only on a platform whose
// vocoder lends nothing (radio_ysf.h). The voice rings, views and statistics stay per mode, since the screens and the
// debug link read them while the mode is off.
//
// A take by a mode while the other holds the area is refused, never granted by stealing: the holder may be part way
// through an over, and its transmit interrupt may still pull from the engine. So the order is always the holder's stop
// (which aborts its transmission, stops the board's pull and gives the area back), then the next mode's start. The
// voice codec's working memory is a separate loan (hal_vocoder_t.lend: D-STAR borrows the DMR vocoder's state while
// DMR voice is off; the vocoder rebuilds its own state at its next call, so there is nothing to give back). Fusion
// runs DN on the vocoder itself, so it takes the loan only for the length of a VW over, in its audio callback.
//
// Threading: take and give in the radio task only. The owner may be read from any task (the debug link).
#ifndef RADIO_MODE_RAM_H
#define RADIO_MODE_RAM_H

#include <stddef.h>
#include <stdint.h>
#include "config.h"

typedef enum {
	RADIO_MODE_RAM_FREE = 0,
	RADIO_MODE_RAM_DSTAR,
	RADIO_MODE_RAM_YSF,
} radio_mode_ram_owner_t;

#if CONFIG_DSTAR || CONFIG_YSF
// The area for who, at least bytes long (8-byte aligned) and zeroed, or NULL when another mode holds it or it is too
// small. Taken again by its holder: the same area, its contents as they were
void *radio_mode_ram_take(radio_mode_ram_owner_t who, size_t bytes);
// Back from who; nothing when who doesn't hold it
void radio_mode_ram_give(radio_mode_ram_owner_t who);
radio_mode_ram_owner_t radio_mode_ram_owner(void);
// The area's size: the largest engine built
size_t radio_mode_ram_size(void);
#else
static inline radio_mode_ram_owner_t radio_mode_ram_owner(void) { return RADIO_MODE_RAM_FREE; }
#endif

#endif
