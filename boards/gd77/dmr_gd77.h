// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// GD-77 DMR modem glue between radio_gd77.c (RF side, mode changes) and dmr_gd77.c (HR_C6000 interrupts, the shared
// slot engine, the deferred event task), plus its counters for the diagnostics page (docs/spec/hardware-gd77.md 5.3).
#ifndef DMR_GD77_H
#define DMR_GD77_H

#include <stdint.h>
#include <stdbool.h>
#include "config.h"
#include "modem/hrc_common/hrc.h"

// A timeslot interrupt closer than this to the one before is the same slot boundary (both edges of the line)
#define GD77_SLOT_MIN_US	5000u

// Pure: whether a timeslot interrupt at now_us starts a new slot, given the last accepted one (0 = none yet)
static inline bool gd77_slot_edge_new(uint32_t now_us, uint32_t last_us, bool have_last)
{
	return !have_last || (uint32_t)(now_us - last_us) >= GD77_SLOT_MIN_US;
}

typedef struct {
	bool active;		// the HR_C6000 is in DMR and its interrupts are on
	bool stack_attached;	// gd77_dmr.start() has given a callback
	uint32_t slot_ints, slot_dups, sys_ints, rftx_ints, rfrx_ints;
	uint32_t ticks, rx_voice, rx_data, rx_crc_err, tx_bursts;
	uint32_t rf_skipped, rf_unarmed;
	uint32_t voice_errors;	// voice port transfers that timed out
} gd77_dmr_stats_t;

void gd77_dmr_stats(gd77_dmr_stats_t *out);

#if CONFIG_DMR
void gd77_dmr_board_init(hrc_t *modem);	// once, from the radio init: pins, voice port, interrupts, task
void gd77_dmr_enter(void);	// the HR_C6000 has just been put in DMR: engine on, interrupts on
void gd77_dmr_leave(void);	// leaving DMR: transmission dropped, interrupts off, engine off
void gd77_dmr_rx_restart(void);	// new channel in DMR
bool gd77_dmr_tx_active(void);	// the engine's slot state says a transmission is in progress
void gd77_dmr_tx_drop(void);	// abort a transmission now
// The voice port (V_SPI) as the HR_C6000 driver's hrc_bus_t.voice
bool gd77_vspi_xfer(void *ctx, bool write, uint8_t *buf, uint8_t len);
#else
static inline void gd77_dmr_board_init(hrc_t *modem) { (void)modem; }
static inline void gd77_dmr_enter(void) {}
static inline void gd77_dmr_leave(void) {}
static inline void gd77_dmr_rx_restart(void) {}
static inline bool gd77_dmr_tx_active(void) { return false; }
static inline void gd77_dmr_tx_drop(void) {}
#endif

// --- dmr_gd77.c -> radio_gd77.c (from the modem interrupts: never blocks, never uses the task-level AT1846S driver) ---

// Per-burst RF switch: tx = the AT1846S to transmit on the armed frequency, the TX path and the drive up; else the
// drive down, the TX path off and the receiver back. False when it couldn't be done now (bus busy, not armed)
bool gd77_rf_dmr_switch(bool tx);
// From the timeslot interrupt: retry a back-to-receive switch that was skipped
void gd77_rf_dmr_service(void);
// Whether the PA drive is up for a DMR burst now
bool gd77_rf_dmr_keyed(void);
void gd77_rf_dmr_counters(uint32_t *skipped, uint32_t *unarmed);

#endif
