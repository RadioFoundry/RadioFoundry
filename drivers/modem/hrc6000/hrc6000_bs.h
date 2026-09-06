// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The split duplex TX half's continuous BS outbound on the HR_C6000 (CONFIG_HOTSPOT_SPLIT; docs/spec/split-duplex.md
// 5.4, 10.1; modem-hrc6000.md 4.1, 6). While it runs it owns the chip in place of the slot engine of hrc6000_dmr.c,
// whose three ISRs and watchdog hand over to it (e->bs set): own timing (0x40 = 0xE3) gives a free-running slot clock,
// a burst is set up for every slot (0x41 = 0x80 and 0x50 each slot) with BS sync (page 1 0x04), the PA is keyed once at
// the first RF TX interrupt and stays up until unkeyed. Each outbound slot plays its own queue in order; an empty one
// gets an idle burst (counted). Key-up starts with an outbound TS1 slot.
//
// The CACH: the chip's CACH generation is undocumented, so the bits from the core are kept (stats) but not written
// anywhere; INFO reports "unknown" (HRC6000_BS_CACH_METHOD) until the bench experiment finds the setting.
// The experiment (split-duplex.md 10.1): variant 0 the operation values 0x10 = 0x6E, 0x11 = 0x80; 1 0x11 = 0x00; 2
// 0x10 = 0x4E and 0x11 = 0x00; 3 0x10 = 0x6A and 0x11 = 0x00; 255 up to four raw (page 4 register, value) pairs.
// Written at key-up after the variant's values, restored at unkey.
#ifndef HRC6000_BS_H
#define HRC6000_BS_H

#include <stdint.h>
#include <stdbool.h>
#include "hrc6000_dmr.h"

#if defined(CONFIG_HOTSPOT_SPLIT) && CONFIG_HOTSPOT_SPLIT

#define HRC6000_BS_Q		3	// bursts queued per slot (the core keeps two ahead)
#define HRC6000_BS_CACH_METHOD	DMR_BS_CACH_UNKNOWN
#define HRC6000_BS_VARIANT_RAW	255

typedef struct {
	uint32_t slots;		// slot boundaries since bs_start
	uint32_t bursts;	// bursts set up while keyed
	uint32_t underruns;	// a slot's queue empty while keyed: idle burst
	uint32_t keyups;
	uint32_t rf_fail;	// PA key / drop the board could not do
	uint32_t stalls;	// no timeslot interrupt for 200 ms while keyed: unkeyed
	uint32_t cach_given;	// CACH handed over by the core (not sent: HRC6000_BS_CACH_METHOD)
	uint32_t page2_lc;	// embedded LC rewrites (two slots share page 2)
} hrc6000_bs_stats_t;

typedef struct hrc6000_bs {
	dmr_bs_config_t cfg;
	volatile bool keyed;	// asked
	bool on_air;		// keyed and the first TS1 slot set up
	bool pa;
	bool regs_written;	// the variant's registers are in the chip
	uint8_t reg40;
	uint32_t base_tick;	// the tick at bs_start (slot base_tick + 1 is an outbound TS1 slot)
	int8_t ts_tick;		// the outbound slot that started at the last tick
	int8_t page2_slot;	// whose embedded LC page 2 0x00 holds (0 none)
	dmr_tx_burst_t q[2][HRC6000_BS_Q];
	uint8_t q_lc[2][HRC6000_BS_Q][9];
	bool q_lc_valid[2][HRC6000_BS_Q];
	volatile uint8_t head[2], tail[2];
	uint8_t lc[2][9];	// each slot's embedded LC (from its last voice A)
	bool lc_valid[2];
	uint8_t last_index[2];
	hrc6000_bs_stats_t st;
} hrc6000_bs_t;

// Task level (each takes the engine's lock)
bool hrc6000_bs_start(hrc6000_bs_t *bs, hrc6000_dmr_t *e, const dmr_bs_config_t *cfg);
void hrc6000_bs_key(hrc6000_dmr_t *e, bool on);
bool hrc6000_bs_queue(hrc6000_dmr_t *e, int8_t ts, const dmr_tx_burst_t *b, const dmr_cach_t *cach,
		      const uint8_t *emb_lc);
uint8_t hrc6000_bs_space(const hrc6000_dmr_t *e, int8_t ts);
void hrc6000_bs_stop(hrc6000_dmr_t *e);		// back to the slot engine, receiving
bool hrc6000_bs_slot_clock(const hrc6000_dmr_t *e, uint32_t *counter, uint32_t *t_us, int8_t *ts);
bool hrc6000_bs_keyed(const hrc6000_dmr_t *e);
static inline bool hrc6000_bs_active(const hrc6000_dmr_t *e) { return e->bs != NULL; }

// From hrc6000_dmr.c's ISRs and watchdog while e->bs is set
void hrc6000_bs_isr_timeslot(hrc6000_dmr_t *e);
void hrc6000_bs_isr_sys(hrc6000_dmr_t *e);
void hrc6000_bs_isr_rftx(hrc6000_dmr_t *e);
void hrc6000_bs_poll(hrc6000_dmr_t *e);

#endif
#endif
