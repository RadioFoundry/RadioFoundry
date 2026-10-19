// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// MD-380 DMR modem glue between radio_md380.c (RF side, mode changes) and dmr_md380.c (HR_C5000 interrupts, the
// slot engine, the deferred event task), plus the bring-up counters for the diagnostics page.
#ifndef DMR_MD380_H
#define DMR_MD380_H

#include <stdint.h>
#include <stdbool.h>
#include "modem/hrc_common/hrc6000_dmr.h"

// --- Bring-up ---

typedef struct {
	bool active;			// the HR_C5000 is in DMR and its interrupts are on
	bool stack_attached;		// md380_dmr.start() has given a callback
	hrc6000_dmr_stats_t modem;	// slot engine counters (hrc6000_dmr.h)
	uint32_t rf_skipped;		// first bursts refused: the synthesizer could not take the TX frequency
	uint32_t rf_unarmed;		// RF TX interrupts with DMR TX not armed by radio tx() (PA left off)
} md380_dmr_stats_t;

// Counters (no bus traffic). Usable without the DMR stack: entering DMR mode (radio set_mode + rx) starts the receiver
void md380_dmr_stats(md380_dmr_stats_t *out);
// The counters as short text lines for the diagnostics page: line i (0..), false past the last
bool md380_dmr_stats_line(uint8_t i, char *buf, uint8_t len);
#define MD380_DMR_DIAG_LINES	21	// lines md380_dmr_stats_line formats
// Zero the counters (slot engine and RF switch); the last_* values and the engine state are kept
void md380_dmr_stats_reset(void);
// RX trace into the debug link's log ("drx" lines: every burst event the engine delivered; CONFIG_DEBUGLINK builds).
// On by default
void md380_dmr_set_trace(bool on);

// --- radio_md380.c -> dmr_md380.c ---

#include "config.h"
#if CONFIG_DMR
void md380_dmr_board_init(hrc_t *modem);	// once, from the radio init: pins, task
void md380_dmr_enter(void);	// the HR_C5000 has just been put in DMR (hrc6000_dmr_enter): engine on, interrupts on
void md380_dmr_leave(void);	// leaving DMR: transmission dropped, interrupts off, engine off
void md380_dmr_rx_restart(void);	// new channel in DMR
bool md380_dmr_tx_active(void);	// the engine's slot state says a transmission is in progress
void md380_dmr_tx_drop(void);	// abort a transmission now (radio retune or tx_off while one runs)
#else	// built without DMR (CONFIG_DMR): the radio never enters DMR
static inline void md380_dmr_board_init(hrc_t *modem) { (void)modem; }
static inline void md380_dmr_enter(void) {}
static inline void md380_dmr_leave(void) {}
static inline void md380_dmr_rx_restart(void) {}
static inline bool md380_dmr_tx_active(void) { return false; }
static inline void md380_dmr_tx_drop(void) {}
#endif

// --- dmr_md380.c -> radio_md380.c (ISR-safe: never blocks) ---

// Per-burst RF switch: tx = at the first burst the receiver off and the synthesizer to the armed TX frequency, then
// the PA supply and loop; else the PA off (the synthesizer stays on TX until radio tx_off). False: no RF
bool md380_rf_dmr_switch(bool tx);
void md380_rf_dmr_counters(uint32_t *skipped, uint32_t *unarmed);

#endif
