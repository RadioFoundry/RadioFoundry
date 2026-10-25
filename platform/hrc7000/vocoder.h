// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// HR_C7000 boards' stock codec host (vocoder.c) as hal_vocoder_t
#ifndef HRC_VOCODER_H
#define HRC_VOCODER_H

#include <stdint.h>
#include "config.h"
#include "hal_vocoder.h"

#if CONFIG_DMR
extern const hal_vocoder_t hrc_vocoder;
uint16_t hrc_vocoder_tag(void);	// the stock codec in the slot: its version number, 0 none
// Stop using the codec for good (until the next boot): waits for a call in progress, after which the codec's RAM is
// free (the stub entry loads the RAM monitor there)
void hrc_vocoder_park(void);
#endif

#endif
