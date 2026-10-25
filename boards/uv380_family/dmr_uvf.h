// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// UV380 family DMR modem: the shared STM32 HR_C6000 glue (platform/stm32f4/hrc/dmr_glue.h) runs the slot engine, the
// interrupts and the deferred event task; radio_uvf.c gives it the per-burst RF switch below.
#ifndef DMR_UVF_H
#define DMR_UVF_H

#include <stdint.h>
#include <stdbool.h>
#include "hrc/dmr_glue.h"

// --- radio_uvf.c, for the glue (ISR-safe: never blocks, never uses the task-level AT1846S driver) ---

// Per-burst RF switch: tx = AT1846S reg 0x30 to DMR TX (retuned to the armed TX frequency first if needed), then the
// PA; else the PA off, then reg 0x30 back to RX. False if the AT1846S could not be written now
bool uvf_rf_dmr_switch(bool tx);
// From the timeslot interrupt: retry a back-to-RX write that was skipped
void uvf_rf_dmr_service(void);
// Per-burst AT1846S switches skipped (I2C3 busy or not on page 0), RF TX interrupts with DMR TX not armed by radio tx()
// (PA left off), back-to-RX writes retried at a later timeslot interrupt
void uvf_rf_dmr_counters(uint32_t *skipped, uint32_t *unarmed, uint32_t *rx_retries);

#endif
