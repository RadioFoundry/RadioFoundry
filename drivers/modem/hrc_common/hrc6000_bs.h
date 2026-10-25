// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The split duplex TX half's continuous BS outbound on the HR_C6000 (CONFIG_HOTSPOT_SPLIT; docs/spec/split-duplex.md
// 5.4, 10.1; modem-hrc6000.md 4.1, 6). While it runs it owns the chip in place of the slot engine of hrc6000_dmr.c,
// whose three ISRs and watchdog hand over to it (e->bs set): own timing (0x40 = 0xE3) gives a free-running slot clock,
// a burst is set up for every slot (0x41 = 0x80 and 0x50 each slot) with BS sync (page 1 0x04), the PA is keyed once at
// the first RF TX interrupt and stays up until unkeyed. Each outbound slot plays its own queue in order; an empty one
// gets an idle burst (counted). Key-up starts with an outbound TS1 slot.
//
// Framing (modem-hrc6000.md 9.2, bench 2026-10-12): in layer 2 the chip frames the bursts but its CACH stays all zero
// whatever is written to the inputs the manual names (0x52 AT / TC, TX RAM 0x24 short LC). So by default the chip
// runs in the physical layer mode (0x10 bits 4-3 = 00) and only modulates: the core builds each slot's 288 bits (CACH
// and burst, bsOutboundRawSlot) and they go to TX RAM page 2, where the chip sends 0x00-0x23 and 0x24-0x47 in
// alternate slots. Variant 1 is the chip's layer 2 framing (0x52 and the short LC written for every burst), 255 our
// framing with up to four raw (page 4 register, value) pairs written at key-up (0x10 restored at unkey, 0x11 and
// 0x40 too when pairs were given).
#ifndef HRC6000_BS_H
#define HRC6000_BS_H

#include <stdint.h>
#include <stdbool.h>
#include "hrc6000_dmr.h"

#if defined(CONFIG_HOTSPOT_SPLIT) && CONFIG_HOTSPOT_SPLIT

#define HRC6000_BS_Q		3	// bursts queued per slot (the core keeps two ahead)
#define HRC6000_BS_CACH_METHOD	DMR_BS_CACH_OURS
#define HRC6000_BS_VARIANT_CHIP	1	// the chip's layer 2 framing (its CACH is empty)
#define HRC6000_BS_VARIANT_RAW	255
#define HRC6000_BS_RAW_BYTES	36	// one slot: CACH (24 bits) and burst (264)
#define HRC6000_BS_AIR_DELAY_US	2200	// a slot boundary to that slot on air (SDR bench 2026-10-21, split-duplex.md 11)

// Builds one slot (core/dmr/bsOutbound.h bsOutboundRawSlot, passed by the board; NULL: the chip's framing). False:
// the burst couldn't be coded and the idle message is in its place
typedef bool (*hrc6000_bs_raw_fn)(const dmr_tx_burst_t *b, const dmr_cach_t *cach, int8_t ts, uint8_t cc,
				  const uint8_t *emb_lc, uint8_t *out);

typedef struct {
	uint32_t slots;		// slot boundaries since bs_start
	uint32_t bursts;	// bursts set up while keyed
	uint32_t underruns;	// a slot's queue empty while keyed: idle burst
	uint32_t keyups;
	uint32_t rf_fail;	// PA key / drop the board could not do
	uint32_t stalls;	// no timeslot interrupt for 200 ms while keyed: unkeyed
	uint32_t cach_given;	// CACH handed over by the core
	uint32_t cach_written;	// ... sent (in our framing, or 0x52 / short LC in the chip's); an underrun has none
	uint32_t page2_lc;	// embedded LC rewrites (two slots share page 2)
	uint32_t raw;		// slots of our framing written
	uint32_t uncoded;	// ... of those, a burst the builder couldn't code (idle sent instead)
	uint32_t at_late;	// a burst for a numbered slot that had passed: dropped
	uint32_t at_wait;	// a slot whose queue head is for a later slot: idle
	uint32_t restarts;	// slot clock restarts (ALIGN)
	uint32_t rs_ignored;	// timeslot interrupts while a restart waited
	int32_t rs_dt[3];	// the first three timeslot interrupts after the last restart's 0x40 write, µs after it
} hrc6000_bs_stats_t;

typedef struct hrc6000_bs {
	dmr_bs_config_t cfg;
	hrc6000_bs_raw_fn raw_slot;	// NULL: the chip's framing
	volatile bool keyed;	// asked
	bool on_air;		// keyed and the first TS1 slot set up
	bool pa;
	bool regs_written;	// the variant's registers are in the chip
	uint8_t reg40;
	uint32_t base_tick;	// the tick at bs_start (slot base_tick + 1 is an outbound TS1 slot)
	int8_t ts_tick;		// the outbound slot that started at the last tick
	volatile bool restarting;	// bs_restart waits: no boundary is taken
	int8_t restart_ts;	// the slot that starts at the first boundary after a restart (0 none)
	uint32_t rs_t;		// when the last restart wrote 0x40 = 0xE3
	uint8_t rs_n;		// timeslot interrupts recorded since (st.rs_dt)
	int8_t page2_slot;	// whose embedded LC page 2 0x00 holds (0 none)
	dmr_tx_burst_t q[2][HRC6000_BS_Q + 1];		// a ring of Q + 1 places: q_count tells full from empty
	dmr_cach_t q_cach[2][HRC6000_BS_Q + 1];
	bool q_cach_valid[2][HRC6000_BS_Q + 1];
	uint8_t q_lc[2][HRC6000_BS_Q + 1][9];
	bool q_lc_valid[2][HRC6000_BS_Q + 1];
	bool q_at[2][HRC6000_BS_Q + 1];		// for one numbered slot only (bs_queue_at)
	uint32_t q_at_c[2][HRC6000_BS_Q + 1];
	volatile uint8_t head[2], tail[2];
	uint8_t lc[2][9];	// each slot's embedded LC (from its last voice A)
	bool lc_valid[2];
	uint8_t last_index[2];
	hrc6000_bs_stats_t st;
} hrc6000_bs_t;

// Task level (each takes the engine's lock)
bool hrc6000_bs_start(hrc6000_bs_t *bs, hrc6000_dmr_t *e, const dmr_bs_config_t *cfg, hrc6000_bs_raw_fn raw_slot);
void hrc6000_bs_key(hrc6000_dmr_t *e, bool on);
bool hrc6000_bs_queue(hrc6000_dmr_t *e, int8_t ts, const dmr_tx_burst_t *b, const dmr_cach_t *cach,
		      const uint8_t *emb_lc);
// For the slot slot_clock numbers counter only (it starts at that boundary; set up at the one before)
bool hrc6000_bs_queue_at(hrc6000_dmr_t *e, uint32_t counter, int8_t ts, const dmr_tx_burst_t *b,
			 const dmr_cach_t *cach, const uint8_t *emb_lc);
// Not keyed: the slot clock restarts with a boundary at the next t_us + k x 30 ms at least 2 ms ahead (waits for it,
// up to 32 ms), slot ts starting there (hal_dmr_modem.h bs_restart): 0x40 with TxEn and RxEn clear, then own timing
// (0xE3) at that time
bool hrc6000_bs_restart(hrc6000_dmr_t *e, uint32_t t_us, int8_t ts);
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
