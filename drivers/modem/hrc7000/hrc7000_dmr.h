// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// HR_C7000 in DMR: burst status decode, burst framing codes and the slot engine behind hal_dmr_modem.h
// (docs/spec/modem-hrc7000.md 2-7; docs/spec/dmr-modem.md 6; docs/spec/gotchas.md 2).
//
// The engine is board independent. The board wires the modem interrupts (system 61, TX timeslot 62, RX timeslot 63,
// RF TX 64) to hrc7000_dmr_isr_*(), runs a task that drains the engine's events (hrc7000_dmr_pop) into the DMR
// stack, and supplies the ops: the per-burst RF switch (FD6818 idle -> TX at the RF TX interrupt; PA off and back to
// RX after the burst), the PA one-shot (armed with every burst, it keys the PA "DMR PA" ms later: hardware-dm32.md
// 8.6), a microsecond clock, a lock that keeps the modem interrupts out, and "wake the task".
//
// Timing model (modem-hrc7000.md 3, 3.1): at every timeslot interrupt the CPU decides what the chip does in the coming
// slot: LAYER2_TXRX_CTRL = 0x80 sends the burst whose payload and LAYER2_SEND_TYPE were just written, 0x40 receives,
// 0x8040 receives with the voice-access flag, 0 does nothing. While transmitting the engine ticks on the TX timeslot
// interrupt, otherwise on the RX one (the working firmware's rule). A transmission uses every other tick (one burst
// per 60 ms), starting at the first tick after tx_begin.
//
// Receive: the chip receives every slot (0x40), and bursts are reported by the system interrupt (bit 3, deframe) with
// their status in RX_TYPE_INFO, which carries the slot (current_rx_slot), the CACH TC bit and the sync family. Slot
// numbers: through a repeater from TC; TDMA direct mode from the sync family; simplex as a label from the chip's slot
// number (1 or 2, stable while the timing holds).
#ifndef HRC7000_DMR_H
#define HRC7000_DMR_H

#include <stdint.h>
#include <stdbool.h>
#include "hrc7000.h"
#include "hal_dmr_modem.h"

// --- Pure helpers ---

// RX_TYPE_INFO fields (2.7)
typedef struct {
	uint8_t kind;		// dmr_burst_kind_t, HRC7000_KIND_NONE = not a usable burst (RC, no type)
	uint8_t voice_index;	// 0..5 for voice
	uint8_t sync;		// dmr_sync_family_t of the received sync (MS, BS, TS1, TS2)
	uint8_t cc;
	uint8_t tc;		// CACH TC (repeater: the timeslot of this burst, 0 = TS1)
	uint8_t chip_slot;	// current_rx_slot
	bool pi;
	bool crc_ok;
	uint8_t lcss;
} hrc7000_rx_status_t;

#define HRC7000_KIND_NONE	0xFFu

void hrc7000_dmr_decode(uint32_t rx_type_info, hrc7000_rx_status_t *st);
// Payload bytes in the RX / TX RAM for a burst kind (12, 18 for rate 3/4, 24 for rate 1, 27 for voice)
uint8_t hrc7000_dmr_payload_len(uint8_t kind);
// LAYER2_SEND_TYPE for a burst: data type << 4; voice 0x08 | index << 4 | LCSS (user guide LCSS when lcss is true:
// A 0, B 1, C 3, D 3, E 2, F 0)
uint32_t hrc7000_dmr_send_type(uint8_t kind, uint8_t voice_index, bool lcss);
// 5-bit checksum of a full LC for the embedded LC (TS 102 361-1 B.3.11: sum of the nine bytes mod 31)
uint8_t hrc7000_dmr_emb_checksum(const uint8_t lc[9]);

// --- Slot engine ---

#define HRC7000_DMR_TXQ_DEPTH	12
#define HRC7000_DMR_EVQ_DEPTH	24

typedef struct {
	// RF switch from the interrupts: tx = RF chip to TX for our burst (RF TX interrupt); else PA off, RF chip to RX
	void (*rf)(void *ctx, bool tx);
	// Start the PA one-shot (called whenever a burst is armed); the board keys the PA when it expires
	void (*pa_arm)(void *ctx);
	uint32_t (*now_us)(void *ctx);
	void (*lock)(void *ctx);	// keep the modem interrupts out (task-level calls)
	void (*unlock)(void *ctx);
	void (*notify)(void *ctx);	// events queued: wake the board's task
	void *ctx;
} hrc7000_dmr_ops_t;

typedef struct {
	uint32_t ticks;		// timeslot interrupts that ticked the engine
	uint32_t ts_tx_ints, ts_rx_ints, sys_ints, rftx_ints;
	uint32_t rx_voice, rx_lc_header, rx_terminator, rx_csbk, rx_data, rx_idle;
	uint32_t rx_crc_err;	// delivered with a bad flag (voice, blocks) or dropped (headers, CSBK)
	uint32_t rx_unusable;	// RC or no type: dropped
	uint32_t late_entry;	// system interrupt bit 2
	uint32_t slot_abnormal;	// bits 5, 6, 16
	uint32_t rx_restarts;
	uint32_t rx_lost, rx_stale;	// receiver restarts: reception over, stale slot timing
	uint32_t cc_scan_steps;
	uint32_t ev_overflow;
	uint32_t tx_bursts, tx_underrun, tx_rejected, tx_done;
	uint32_t rx_in_tx[2];	// bursts deframed while we send, by ticks since our last arming (even, odd)
	uint32_t stalls;
	uint32_t last_rx_type_info;
	uint32_t last_sys_list;
	uint32_t last_l2_status;
	uint8_t last_cc;
	int8_t last_ts;
	uint8_t tx_state;
	uint8_t last_pa_ms;	// RF TX interrupt to the PA off, last burst
} hrc7000_dmr_stats_t;

typedef struct {
	dmr_event_t ev;
	dmr_burst_t burst;
} hrc7000_dmr_evslot_t;

typedef struct {
	hrc7000_t *dev;
	const hrc7000_dmr_ops_t *ops;
	bool running;
	// FM receive audio half ready (SYS_INTERP bit 14, modem-hrc7000.md 8.5): called from the system interrupt whether
	// or not the engine runs, NULL for none. Set it after hrc7000_dmr_init
	void (*fm_rx)(void);
	bool lcss;		// LCSS on voice bursts B-E (user guide); false = 0 on every burst like the 2026 builds
	dmr_config_t cfg;
	int8_t cc_filter;	// -1 any (scan)
	int8_t lock_ts;

	uint32_t tick;
	uint32_t t_tick;
	bool timing;
	// Receive
	uint32_t rx_tick;	// tick of the last burst taken
	bool rx_any;
	uint32_t resync_tick;	// tick of the last receiver restart for lost or stale timing
	int8_t voice_chip_slot;	// chip slot of the voice call being received, -1 none
	uint32_t voice_tick;
	int8_t tc_slot[2];	// TS (1/2) of each chip slot, learned from BS-sync bursts (TC), 0 unknown
	uint8_t cc_cur;		// LOCAL_CC now
	uint32_t cc_tick;	// last CC change or burst heard (scan)
	bool emb_pending;	// late entry: an embedded LC waits for the next voice burst
	uint8_t emb_rx[9];
	uint8_t rx_restart_ticks;	// tail of the restart after a transmission through a repeater (6.3)
	// Transmit
	uint8_t tx_state;
	uint8_t tx_timing;
	int8_t tx_ts;
	uint8_t tx_access;
	bool tx_our_next;	// the coming slot (the one being decided) is ours
	bool tx_end_req, tx_abort_req;
	uint8_t tx_wait;
	uint32_t tx_last_arm;	// tick of the last burst armed
	bool tx_any;
	bool tx_listen;		// the other slot is received while we send (an SFR's inbound slot)
	uint8_t send_sync;	// dmr_sync_family_t in SEND_DATA_SYNC now, DMR_SYNC_NONE = not known
	dmr_tx_burst_t txq[HRC7000_DMR_TXQ_DEPTH];
	volatile uint8_t txq_head, txq_tail;
	dmr_tx_burst_t last_hdr;
	uint8_t last_kind;
	uint8_t next_voice;
	uint8_t emb_lc[9];
	bool emb_set;
	uint8_t call_lc[9];	// LC of the last voice header sent (embedded LC when none was set)
	// RF / PA
	bool rf_tx;
	uint32_t t_rf_tx;

	hrc7000_dmr_evslot_t evq[HRC7000_DMR_EVQ_DEPTH];
	volatile uint8_t evq_head, evq_tail;

	hrc7000_dmr_stats_t st;
} hrc7000_dmr_t;

void hrc7000_dmr_init(hrc7000_dmr_t *e, hrc7000_t *dev, const hrc7000_dmr_ops_t *ops);

// The chip has just entered DMR (hrc7000_dmr_enter): engine on, receiver (re)started (5.2). Task level
void hrc7000_dmr_run(hrc7000_dmr_t *e);
// Leaving DMR: any transmission dropped at once (RF to RX, PA off), engine off. Task level
void hrc7000_dmr_halt(hrc7000_dmr_t *e);

// ISR entry points (one priority, never nested)
void hrc7000_dmr_isr_sys(hrc7000_dmr_t *e);
void hrc7000_dmr_isr_timeslot(hrc7000_dmr_t *e, bool tx_vector);
void hrc7000_dmr_isr_rftx(hrc7000_dmr_t *e);

// Task side
bool hrc7000_dmr_pop(hrc7000_dmr_t *e, dmr_event_t *ev, dmr_burst_t *burst);
void hrc7000_dmr_poll(hrc7000_dmr_t *e);	// every few ms: stall watchdog

// hal_dmr_modem.h commands (task level; each takes the lock)
void hrc7000_dmr_configure(hrc7000_dmr_t *e, const dmr_config_t *cfg);
void hrc7000_dmr_rx_restart(hrc7000_dmr_t *e);
void hrc7000_dmr_rx_lock_slot(hrc7000_dmr_t *e, int8_t ts);
bool hrc7000_dmr_tx_begin(hrc7000_dmr_t *e, dmr_timing_t timing, int8_t ts, dmr_access_t access);
// After tx_begin: receive the other slot between our bursts until the transmission ends
void hrc7000_dmr_tx_listen(hrc7000_dmr_t *e);
bool hrc7000_dmr_tx_queue(hrc7000_dmr_t *e, const dmr_tx_burst_t *b);
uint8_t hrc7000_dmr_tx_queue_space(const hrc7000_dmr_t *e);
void hrc7000_dmr_tx_end(hrc7000_dmr_t *e);
void hrc7000_dmr_tx_abort(hrc7000_dmr_t *e);
void hrc7000_dmr_set_embedded_lc(hrc7000_dmr_t *e, const uint8_t lc[9]);
void hrc7000_dmr_set_cc_filter(hrc7000_dmr_t *e, int8_t cc);
bool hrc7000_dmr_is_transmitting(const hrc7000_dmr_t *e);

#endif
