// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The DMR modem contract (docs/spec/dmr-modem.md section 6): the driver owns the chip, its interrupts and the per-slot
// register work; the DMR stack (core/dmr) owns calls, filters and policy. They meet through a TX queue and RX events,
// so the stack runs at task level and never inside the slot interrupt.
#ifndef HAL_DMR_MODEM_H
#define HAL_DMR_MODEM_H

#include <stdint.h>
#include <stdbool.h>

typedef enum { DMR_SYNC_NONE, DMR_SYNC_MS, DMR_SYNC_BS, DMR_SYNC_TS1, DMR_SYNC_TS2 } dmr_sync_family_t;

typedef enum {
	DMR_ACCESS_IMPOLITE,
	DMR_ACCESS_POLITE_CC,	// polite to own color code
	DMR_ACCESS_POLITE_ALL,
} dmr_access_t;

typedef enum {
	DMR_TIMING_OWN,		// direct mode, own timing
	DMR_TIMING_ALIGNED,	// aligned to the received signal (repeater), on the slot with the given TS
	DMR_TIMING_ASAP,
} dmr_timing_t;

// Burst kinds. Data types follow ETSI TS 102 361-1 9.3.6 (slot type); voice is separate
typedef enum {
	DMR_BURST_VOICE = 0x10,	// voice burst, index 0..5 (A..F) in dmr_burst_t.voice_index
	DMR_BURST_PI_HEADER = 0x0,
	DMR_BURST_VOICE_LC_HEADER = 0x1,
	DMR_BURST_TERMINATOR_LC = 0x2,
	DMR_BURST_CSBK = 0x3,
	DMR_BURST_MBC_HEADER = 0x4,
	DMR_BURST_MBC_CONT = 0x5,
	DMR_BURST_DATA_HEADER = 0x6,
	DMR_BURST_RATE_12_DATA = 0x7,
	DMR_BURST_RATE_34_DATA = 0x8,
	DMR_BURST_IDLE = 0x9,
	DMR_BURST_RATE_1_DATA = 0xA,
	DMR_BURST_USBD = 0xB,		// unified single block data
	// Not a burst: our slot is left empty, and received with tx_listen (an SFR's gap inside an inbound call: an idle
	// burst framed there corrupts the inbound burst decoded next to it, gotchas 14.4). Counts as taken (TX_SLOT_STARTED)
	DMR_BURST_EMPTY = 0x1F,
} dmr_burst_kind_t;

#define DMR_VOICE_BYTES		27	// three 72-bit AMBE+2 frames
#define DMR_PAYLOAD_MAX		27

typedef struct {
	uint8_t kind;		// dmr_burst_kind_t
	uint8_t voice_index;	// 0..5 for voice
	uint8_t sync;		// dmr_sync_family_t
	uint8_t cc;
	int8_t ts;		// 1 or 2, 0 = unknown
	bool crc_ok;
	bool pi;
	bool in_step;		// reported in time to be trusted (HR_C6000: unreliable while we transmit)
	bool lc_valid;		// lc[] holds a complete embedded LC (voice F) or the header / terminator LC
	int16_t rssi_dbm;	// 0 = unknown
	uint8_t len;		// payload bytes: 27 voice, 12 rate 1/2 (LC/CSBK/header 9+3 CRC), 18 rate 3/4, 24 rate 1
	uint8_t payload[DMR_PAYLOAD_MAX];
	uint8_t lc[9];		// full LC without CRC
} dmr_burst_t;

typedef enum {
	DMR_EV_SLOT_TICK,	// arg = slot counter; ts = TS if known
	// burst = the burst heard; arg = the slot counter (as in DMR_EV_SLOT_TICK, never 0) when it was heard, 0 if the
	// driver keeps none. One event per burst on air, in air order; events may reach the stack in batches, so the
	// stack orders and de-duplicates by voice_index and arg, never by arrival time
	DMR_EV_BURST_RX,
	DMR_EV_VOICE_LOST,	// ts = the slot
	DMR_EV_TX_SLOT_STARTED,
	DMR_EV_TX_SLOT_ENDED,
	DMR_EV_TX_REJECTED,	// polite access refused
	DMR_EV_TX_DONE,		// the last queued burst's slot has ended
	DMR_EV_SYNC_LOST,	// receiver restarted after a stall
} dmr_event_kind_t;

typedef struct {
	uint8_t kind;		// dmr_event_kind_t
	int8_t ts;
	uint32_t arg;
	const dmr_burst_t *burst;	// DMR_EV_BURST_RX only, valid during the callback
} dmr_event_t;

// Called by the driver from task level (its own deferred handler), never from the slot interrupt. Must not block long
typedef void (*dmr_event_cb_t)(const dmr_event_t *ev, void *ctx);

typedef struct {
	uint32_t own_id;
	int8_t cc;		// -1 = any
	dmr_access_t access;
	bool repeater;		// RMO framing (BS sync expected) vs DMO
} dmr_config_t;

typedef struct {
	uint8_t kind;		// dmr_burst_kind_t
	uint8_t voice_index;	// voice: 0..5
	bool host_crc;		// payload already carries its CRC (CRC_MCU_Control)
	uint8_t sync_override;	// dmr_sync_family_t, DMR_SYNC_NONE = normal MS sync
	uint8_t len;
	uint8_t payload[DMR_PAYLOAD_MAX];
} dmr_tx_burst_t;

#if defined(CONFIG_HOTSPOT_SPLIT) && CONFIG_HOTSPOT_SPLIT
// Split duplex TX half (docs/spec/split-duplex.md 5): the CACH before one outbound burst (core/dmr/fec/dmrCach.h)
typedef struct {
	uint8_t at;		// access type of the inbound slot with the same number: 1 busy
	uint8_t tc;		// the outbound slot after the CACH: 0 TS1, 1 TS2
	uint8_t lcss;		// short LC fragment: 1 first, 3 continuation, 2 last
	uint32_t payload;	// 17 bits, P(16) in bit 16
	uint32_t bits;		// all 24 bits interleaved (figure B.9), TX(23) in bit 23
} dmr_cach_t;

#define DMR_BS_REGS_MAX		4

// The continuous BS outbound's setup (bs_start)
typedef struct {
	uint8_t cc;
	uint8_t variant;	// BS framing experiment (split-duplex.md 10.1): 0 the driver default
	uint8_t nregs;		// variant 255: raw (register, value) pairs written at key-up, restored at unkey
	uint8_t regs[DMR_BS_REGS_MAX][2];
} dmr_bs_config_t;

// What the driver does with the CACH bits (split-duplex.md 3.1, INFO byte 4)
enum { DMR_BS_CACH_NONE, DMR_BS_CACH_OURS, DMR_BS_CACH_CHIP, DMR_BS_CACH_UNKNOWN };
#endif

typedef struct {
	// Chip on and in DMR mode (after the board's RF side is in DMR mode); events start flowing to cb
	bool (*start)(const dmr_config_t *cfg, dmr_event_cb_t cb, void *ctx);
	void (*stop)(void);		// back to the FM / idle state of the chip
	void (*configure)(const dmr_config_t *cfg);
	void (*rx_restart)(void);
	void (*rx_lock_slot)(int8_t ts);	// 1, 2, or 0 for both
	bool (*tx_begin)(dmr_timing_t timing, int8_t ts, dmr_access_t access);
	// Queue one burst for our next free slot; false if the queue is full (depth >= 8)
	bool (*tx_queue)(const dmr_tx_burst_t *b);
	uint8_t (*tx_queue_space)(void);
	void (*tx_end)(void);		// send what is queued, then DMR_EV_TX_DONE
	void (*tx_abort)(void);
	void (*set_embedded_lc)(const uint8_t lc[9]);	// spread over B-E from the next superframe
	void (*set_cc_filter)(int8_t cc);
	bool (*is_transmitting)(void);	// slot state, never the PA
	// Optional (NULL: the driver can't). For the transmission begun last: receive in the slots between our bursts and
	// report what is heard there, with the RF switched back to receive per burst (an SFR's inbound slot, sfr.md 3)
	void (*tx_listen)(void);
	// Optional: b takes the place of the oldest DMR_BURST_EMPTY still queued (not yet set up). Returns how many queued
	// entries come after it, -1 when none is left (an SFR's repeat then goes out 90 ms after the inbound burst instead
	// of behind the placeholders queued for the gap)
	int8_t (*tx_fill)(const dmr_tx_burst_t *b);
#if defined(CONFIG_HOTSPOT_SPLIT) && CONFIG_HOTSPOT_SPLIT
	// Split duplex (docs/spec/split-duplex.md), all optional (NULL: the driver can't; the role is not offered).
	// The modem clock: free-running microseconds (wraps), the clock slot_clock is stamped in. RX half and TIME
	uint32_t (*now_us)(void);
	// The latest slot boundary (timeslot interrupt): its count (as DMR_EV_SLOT_TICK's arg) and modem clock time; ts:
	// the outbound slot that started there in the BS outbound (1 or 2), else 0. False while no slot clock runs
	bool (*slot_clock)(uint32_t *counter, uint32_t *t_us, int8_t *ts);
	// TX half: the continuous BS outbound. bs_start: receiver off, own slot clock running, not keyed yet (true when
	// running). bs_key: key the continuous outbound, starting with an outbound TS1 slot (idle bursts where nothing
	// is queued) / unkey after the slot on air, the queues dropped. bs_queue: the burst and its CACH for the next free outbound slot ts (1 or 2), in order per slot, and for
	// voice burst A the embedded LC for B-E of its superframe (else NULL); false when that slot's queue is full.
	// bs_stop: unkey at once, back to normal receive (the hotspot restarts it)
	bool (*bs_start)(const dmr_bs_config_t *cfg);
	void (*bs_key)(bool on);
	bool (*bs_queue)(int8_t ts, const dmr_tx_burst_t *b, const dmr_cach_t *cach, const uint8_t *emb_lc);
	uint8_t (*bs_space)(int8_t ts);
	void (*bs_stop)(void);
	bool (*bs_keyed)(void);		// the PA is up for the outbound (keyed and on air)
	uint8_t bs_cach;		// DMR_BS_CACH_xxx
#endif
} hal_dmr_modem_t;

#endif
