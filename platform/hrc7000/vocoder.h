// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// HR_C7000 boards' stock codec host (vocoder.c) as hal_vocoder_t
#ifndef HRC_VOCODER_H
#define HRC_VOCODER_H

#include <stdint.h>
#include "config.h"
#include "hal_vocoder.h"

#if CONFIG_DMR
extern const hal_vocoder_t hrc_vocoder;
uint8_t hrc_vocoder_tag(void);	// 49 or 46: the stock codec in the slot, 0 none
#endif

#endif
