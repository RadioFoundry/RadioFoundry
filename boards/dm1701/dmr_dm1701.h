// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// DM-1701 DMR modem glue between radio_dm1701.c (RF side, mode changes) and dmr_dm1701.c (HR_C6000 interrupts, the
// slot engine, the deferred event task), plus the bring-up counters for the diagnostics page.
#ifndef DMR_DM1701_H
#define DMR_DM1701_H

#include <stdint.h>
#include <stdbool.h>
#include "modem/hrc6000/hrc6000_dmr.h"

// --- Bring-up ---

typedef struct {
	bool active;			// the HR_C6000 is in DMR and its interrupts are on
	bool stack_attached;		// dm1701_dmr.start() has given a callback
	hrc6000_dmr_stats_t modem;	// slot engine counters (hrc6000_dmr.h)
	uint32_t rf_skipped;		// per-burst AT1846S switches skipped: I2C3 busy or not on page 0
	uint32_t rf_unarmed;		// RF TX interrupts with DMR TX not armed by radio tx() (PA left off)
	uint32_t rf_rx_retries;		// AT1846S back-to-RX writes retried at a later timeslot interrupt
} dm1701_dmr_stats_t;

// Counters (no bus traffic). Usable without the DMR stack: entering DMR mode (radio set_mode + rx) starts the receiver
void dm1701_dmr_stats(dm1701_dmr_stats_t *out);
// The counters as short text lines for the diagnostics page: line i (0..), false past the last
bool dm1701_dmr_stats_line(uint8_t i, char *buf, uint8_t len);
#define DM1701_DMR_DIAG_LINES	17	// lines dm1701_dmr_stats_line formats
// Zero the counters (slot engine and RF switch); the last_* values and the engine state are kept
void dm1701_dmr_stats_reset(void);

// --- radio_dm1701.c -> dmr_dm1701.c ---

#include "config.h"
#if CONFIG_DMR
void dm1701_dmr_board_init(hrc6000_t *modem);	// once, from the radio init: pins, task
void dm1701_dmr_enter(void);	// the HR_C6000 has just been put in DMR (hrc6000_dmr_enter): engine on, interrupts on
void dm1701_dmr_leave(void);	// leaving DMR: transmission dropped, interrupts off, engine off
void dm1701_dmr_rx_restart(void);	// new channel in DMR
bool dm1701_dmr_tx_active(void);	// the engine's slot state says a transmission is in progress
void dm1701_dmr_tx_drop(void);	// abort a transmission now (radio retune or tx_off while one runs)
#else	// built without DMR (CONFIG_DMR): the radio never enters DMR
static inline void dm1701_dmr_board_init(hrc6000_t *modem) { (void)modem; }
static inline void dm1701_dmr_enter(void) {}
static inline void dm1701_dmr_leave(void) {}
static inline void dm1701_dmr_rx_restart(void) {}
static inline bool dm1701_dmr_tx_active(void) { return false; }
static inline void dm1701_dmr_tx_drop(void) {}
#endif

// --- dmr_dm1701.c -> radio_dm1701.c (ISR-safe: never blocks, never uses the task-level AT1846S driver) ---

// Per-burst RF switch: tx = AT1846S reg 0x30 to DMR TX (retuned to the armed TX frequency first if needed), then the
// PA; else the PA off, then reg 0x30 back to RX. False if the AT1846S could not be written now
bool dm1701_rf_dmr_switch(bool tx);
// From the timeslot interrupt: retry a back-to-RX write that was skipped
void dm1701_rf_dmr_service(void);
void dm1701_rf_dmr_counters(uint32_t *skipped, uint32_t *unarmed, uint32_t *rx_retries);

#endif
