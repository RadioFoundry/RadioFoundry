// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// MD-380 DMR modem: the shared STM32 HR_C6000-family glue (platform/stm32f4/hrc/dmr_glue.h) runs the slot engine in
// its HR_C5000 variant, the interrupts and the deferred event task; radio_md380.c gives it the per-burst RF switch.
#ifndef DMR_MD380_H
#define DMR_MD380_H

#include <stdint.h>
#include <stdbool.h>
#include "hrc/dmr_glue.h"

// --- radio_md380.c, for the glue (ISR-safe: never blocks) ---

// Per-burst RF switch: tx = at the first burst the receiver off and the synthesizer to the armed TX frequency, then
// the PA supply and loop; else the PA off (the synthesizer stays on TX until the glue's tx_done hook or radio tx_off).
// False: no RF
bool md380_rf_dmr_switch(bool tx);

#endif
