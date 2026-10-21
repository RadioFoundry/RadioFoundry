// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// DMR modem glue shared by the STM32F4 radios with an HR_C6000 or HR_C5000 (DM-1701, MD-380, MD-9600): the slot engine
// (drivers/modem/hrc_common/hrc6000_dmr.c) on the chip's three interrupt lines, the deferred event task that calls the
// DMR stack, the hal_dmr_modem_t over them and the bring-up counters (docs/spec/dmr-modem.md 6). The three lines are
// pins 0, 1 and 2 of one port (EXTI0-2) on every one of these boards. A board supplies what differs: how its RF side
// switches for each burst (stm32_hrc_dmr_board_t). A board adds platform/stm32f4/hrc to BOARD_DIRS to use it.
#ifndef STM32_HRC_DMR_GLUE_H
#define STM32_HRC_DMR_GLUE_H

#include <stdint.h>
#include <stdbool.h>
#include "config.h"
#include "platform.h"
#include "hal_dmr_modem.h"
#include "modem/hrc_common/hrc6000_dmr.h"

typedef struct {
	// The port whose pins 0, 1, 2 carry the timeslot, system and RF TX interrupts (falling edge)
	GPIO_TypeDef *int_port;
	// Per-burst RF switch, from the modem interrupts (never blocks): tx = key the PA for our burst, else the PA off and
	// the RF side back to receive. False when it could not be done now; the engine counts it
	bool (*rf_switch)(bool tx);
	// Optional: at each timeslot interrupt, before the engine (a board retries a skipped back-to-RX switch here)
	void (*ts_service)(void);
	// Optional: the board's RF switch counters (bursts it could not switch, RF TX interrupts with nothing armed, RX
	// switches retried). Any pointer may be ignored
	void (*rf_counters)(uint32_t *skipped, uint32_t *unarmed, uint32_t *rx_retries);
} stm32_hrc_dmr_board_t;

typedef struct {
	bool active;			// the chip is in DMR and its interrupts are on
	bool stack_attached;		// start() has given a callback
	hrc6000_dmr_stats_t modem;	// slot engine counters (hrc6000_dmr.h)
	uint32_t rf_skipped;		// the board's RF switch counters (rf_counters), since the last reset
	uint32_t rf_unarmed;
	uint32_t rf_rx_retries;
} stm32_hrc_dmr_stats_t;

// Lines stm32_hrc_dmr_stats_line formats (at most about 26 characters each, the diagnostics page)
#define STM32_HRC_DMR_DIAG_LINES	22

#if CONFIG_DMR
// Once, from the radio init, after the chip is attached: interrupt lines (left off), task
void stm32_hrc_dmr_init(hrc_t *modem, const stm32_hrc_dmr_board_t *b);
void stm32_hrc_dmr_enter(void);		// the chip has just been put in DMR (hrc6000_dmr_enter): engine on, interrupts on
void stm32_hrc_dmr_leave(void);		// leaving DMR: transmission dropped, interrupts off, engine off
void stm32_hrc_dmr_rx_restart(void);	// new channel in DMR
bool stm32_hrc_dmr_tx_active(void);	// the engine's slot state says a transmission is in progress
void stm32_hrc_dmr_tx_drop(void);	// abort a transmission now (radio retune or tx_off while one runs)

extern const hal_dmr_modem_t stm32_hrc_dmr;

// Counters (no bus traffic). Usable without the DMR stack: entering DMR mode (radio set_mode + rx) starts the receiver
void stm32_hrc_dmr_stats(stm32_hrc_dmr_stats_t *out);
// The counters as text for the diagnostics page: line i (0..), false past the last
bool stm32_hrc_dmr_stats_line(uint8_t i, char *buf, uint8_t len);
// Zero the counters (slot engine and RF switch); the last_* values and the engine state are kept
void stm32_hrc_dmr_stats_reset(void);
// RX trace into the debug link's log ("drx" lines: every burst event the engine delivered; CONFIG_DEBUGLINK builds).
// On by default
void stm32_hrc_dmr_set_trace(bool on);
#else	// built without DMR (CONFIG_DMR): the radio never enters DMR
static inline void stm32_hrc_dmr_init(hrc_t *modem, const stm32_hrc_dmr_board_t *b) { (void)modem; (void)b; }
static inline void stm32_hrc_dmr_enter(void) {}
static inline void stm32_hrc_dmr_leave(void) {}
static inline void stm32_hrc_dmr_rx_restart(void) {}
static inline bool stm32_hrc_dmr_tx_active(void) { return false; }
static inline void stm32_hrc_dmr_tx_drop(void) {}
#endif

#endif
