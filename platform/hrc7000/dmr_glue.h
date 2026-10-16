// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// HR_C7000 DMR modem glue shared by the boards (dmr_glue.c): the slot engine on the SoC's modem interrupts, the PA
// one-shot, the deferred event task and the hal_dmr_modem_t over them. A board supplies what differs: how its RF chip
// switches between receive and transmit for each burst and how its PA is keyed
#ifndef HRC_DMR_GLUE_H
#define HRC_DMR_GLUE_H

#include <stdint.h>
#include <stdbool.h>
#include "config.h"
#include "hal_dmr_modem.h"
#include "modem/hrc7000/hrc7000.h"

typedef struct {
	// From the modem interrupts (ISR context, interrupts off): the RF chip to TX for our burst or back to RX after it.
	// After a burst the board also drops the PA drive
	void (*rf_switch)(bool tx);
	// The PA one-shot expired (ISR context): key the PA if the transmission is still on
	void (*pa_on)(void);
	// Microseconds from the start of a burst to the PA one-shot (the DM-32UV's "DMR PA" setting, default 28 ms)
	uint32_t pa_delay_us;
} hrc_dmr_board_t;

#if CONFIG_DMR
// Once, from the radio init, after the modem is attached: interrupts, PA timer, task
void hrc_dmr_board_init(hrc7000_t *modem, const hrc_dmr_board_t *hooks);
void hrc_dmr_enter(void);		// the modem has just entered DMR: engine on, interrupts on
void hrc_dmr_leave(void);
void hrc_dmr_rx_restart(void);
bool hrc_dmr_tx_active(void);
void hrc_dmr_tx_drop(void);
// FM receive audio (fm_rx_audio.c): fn runs from the modem's system interrupt for each half the modem fills; NULL
// stops it. The system interrupt stays on for it outside DMR
bool hrc_dmr_fm_rx_hook(void (*fn)(void));

extern const hal_dmr_modem_t hrc_dmr;

// Bring-up counters: lines of at most about 20 characters, and the reset
#define HRC_DMR_DIAG_LINES	9
bool hrc_dmr_stats_line(uint8_t i, char *buf, uint8_t len);
void hrc_dmr_stats_reset(void);
#else
static inline void hrc_dmr_board_init(hrc7000_t *modem, const hrc_dmr_board_t *hooks) { (void)modem; (void)hooks; }
static inline void hrc_dmr_enter(void) {}
static inline void hrc_dmr_leave(void) {}
static inline void hrc_dmr_rx_restart(void) {}
static inline bool hrc_dmr_tx_active(void) { return false; }
static inline void hrc_dmr_tx_drop(void) {}
static inline bool hrc_dmr_fm_rx_hook(void (*fn)(void)) { (void)fn; return false; }
#endif

#endif
