// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The split duplex TX half's continuous BS outbound on the HR_C7000 (CONFIG_HOTSPOT_SPLIT; docs/spec/split-duplex.md
// 5.4, 10.1; modem-hrc7000.md 2.9, 2.10). While it runs it owns the chip in place of the slot engine of
// hrc7000_dmr.c, whose ISRs and watchdog hand over to it (e->bs set).
//
// Framing: the chip's layer 2 BS mode frames Idle bursts with a CACH of valid parity, but its AT, TC and short LC are
// not under our control (2.9, bench 2026-10-20). So the chip runs in the physical layer mode (DS 9.5), where it sends
// the CPU's 288 bits per slot without coding: the core builds each slot (bsOutboundRawSlot: the CACH, then the burst
// with BS sync, slot type and coded payload) and the driver writes it to TX RAM. Own timing (LAYER2_CONTROL txen and
// tx_master_mode) gives a free-running slot clock; a slot is sent when LAYER2_TXRX_CTRL b7 is set at its slot
// interrupt (PHY_RDY or a timeslot vector), which comes about 5 ms before the slot goes on air. The PA is keyed once at the first RF TX interrupt and stays up until unkeyed. Each outbound
// slot plays its own queue in order; an empty one gets an idle burst (counted). Key-up starts with an outbound TS1
// slot.
//
// The variant (dmr_bs_config_t.variant) is a bench experiment's bit field, 0 the default: b0 continuous mode
// (WORK_MODE 0x02) instead of timeslot mode (0x22); b1 LAYER2_TXRX_CTRL b0 autotest set with the send bit, as the
// datasheet's set-up table has it (0x81); b3:2 the TX RAM halves: 0 the slot goes to both (as on the HR_C6000), 1
// alternate, the first slot in SEND_DATA0 (0x00), 2 alternate, the first in SEND_DATA1 (0x24).
#ifndef HRC7000_BS_H
#define HRC7000_BS_H

#include <stdint.h>
#include <stdbool.h>
#include "hrc7000_dmr.h"

#if defined(CONFIG_HOTSPOT_SPLIT) && CONFIG_HOTSPOT_SPLIT

#define HRC7000_BS_Q		3	// bursts queued per slot (the core keeps two ahead)
#define HRC7000_BS_CACH_METHOD	DMR_BS_CACH_OURS
#define HRC7000_BS_RAW_BYTES	36	// one slot: CACH (24 bits) and burst (264)

#define HRC7000_BS_V_CONTINUOUS	0x01u
#define HRC7000_BS_V_AUTOTEST	0x02u
#define HRC7000_BS_V_HALF_MASK	0x0Cu
#define HRC7000_BS_V_HALF_BOTH	0x00u
#define HRC7000_BS_V_HALF_ALT0	0x04u
#define HRC7000_BS_V_HALF_ALT1	0x08u

// Builds one slot (core/dmr/bsOutbound.h bsOutboundRawSlot, passed by the glue). False: the burst couldn't be coded
// and the idle message is in its place
typedef bool (*hrc7000_bs_raw_fn)(const dmr_tx_burst_t *b, const dmr_cach_t *cach, int8_t ts, uint8_t cc,
				  const uint8_t *emb_lc, uint8_t *out);

typedef struct {
	uint32_t slots;		// slot boundaries since bs_start
	uint32_t bursts;	// slots written while keyed
	uint32_t underruns;	// a slot's queue empty while keyed: idle burst
	uint32_t keyups;
	uint32_t stalls;	// no slot boundary for 200 ms while keyed: unkeyed
	uint32_t cach_given;	// CACH handed over by the core
	uint32_t cach_written;	// ... sent; an underrun has none
	uint32_t uncoded;	// a burst the builder couldn't code (idle sent instead)
	uint32_t ts_ints;	// timeslot interrupts (both vectors), and those that made a slot boundary
	uint32_t ts_ticks;
	uint32_t phy_ints;	// system interrupt PHY_RDY (b12): a slot boundary too when no timeslot one came
	uint32_t rftx_ints;
	uint32_t late;		// a slot boundary more than 33 ms after the last (an interrupt missed or late)
	uint32_t max_gap_us;	// the longest time between two slot boundaries while keyed
} hrc7000_bs_stats_t;

typedef struct hrc7000_bs {
	dmr_bs_config_t cfg;
	hrc7000_bs_raw_fn raw_slot;
	volatile bool keyed;	// asked
	bool on_air;		// keyed and the first TS1 slot set up
	bool pa;
	uint8_t half;		// the TX RAM half the next slot goes to (alternating variants)
	uint32_t base_tick;	// the tick at bs_start: boundary base_tick + 1 sets up TS2, + 2 TS1
	int8_t ts_tick;		// the outbound slot set up at the last tick, on air from about 5 ms after it
	dmr_tx_burst_t q[2][HRC7000_BS_Q + 1];		// a ring of Q + 1 places: q_count tells full from empty
	dmr_cach_t q_cach[2][HRC7000_BS_Q + 1];
	bool q_cach_valid[2][HRC7000_BS_Q + 1];
	uint8_t q_lc[2][HRC7000_BS_Q + 1][9];
	bool q_lc_valid[2][HRC7000_BS_Q + 1];
	volatile uint8_t head[2], tail[2];
	uint8_t lc[2][9];	// each slot's embedded LC (from its last voice A)
	bool lc_valid[2];
	hrc7000_bs_stats_t st;
} hrc7000_bs_t;

// Task level (each takes the engine's lock)
bool hrc7000_bs_start(hrc7000_bs_t *bs, hrc7000_dmr_t *e, const dmr_bs_config_t *cfg, hrc7000_bs_raw_fn raw_slot);
void hrc7000_bs_key(hrc7000_dmr_t *e, bool on);
bool hrc7000_bs_queue(hrc7000_dmr_t *e, int8_t ts, const dmr_tx_burst_t *b, const dmr_cach_t *cach,
		      const uint8_t *emb_lc);
uint8_t hrc7000_bs_space(const hrc7000_dmr_t *e, int8_t ts);
void hrc7000_bs_stop(hrc7000_dmr_t *e);		// back to the slot engine, receiving
bool hrc7000_bs_keyed(const hrc7000_dmr_t *e);
static inline bool hrc7000_bs_active(const hrc7000_dmr_t *e) { return e->bs != NULL; }

// From hrc7000_dmr.c while e->bs is set: the ISRs, the watchdog, and leaving DMR (the chip back to layer 2 and the
// PA off, without restarting the receiver; the engine's lock held)
void hrc7000_bs_isr_timeslot(hrc7000_dmr_t *e);
void hrc7000_bs_isr_sys(hrc7000_dmr_t *e, uint32_t list);
void hrc7000_bs_isr_rftx(hrc7000_dmr_t *e);
void hrc7000_bs_poll(hrc7000_dmr_t *e);
void hrc7000_bs_release(hrc7000_dmr_t *e);

#endif
#endif
