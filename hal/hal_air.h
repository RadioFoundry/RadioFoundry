// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The air interface (docs/spec/air-interface.md): symbols with time stamps between a provider (a modem chip in its
// physical layer mode, or the soft modem on a board's sample taps) and the framer (core/air). It knows no mode. A
// mode describes what it needs as a profile; a provider says how well it fits and then moves symbols in air order.
//
// Symbols are dibits everywhere: 01 = +3, 00 = +1, 10 = -1, 11 = -3 (the map every 4-level mode here shares), packed
// four to a byte, the first on air in bits 7-6 of byte 0. A 2-level mode uses only 01 (+) and 11 (-).
#ifndef HAL_AIR_H
#define HAL_AIR_H

#include <stdint.h>
#include <stdbool.h>

#define AIR_SYNC_SYMS_MAX	24	// 48 bits: the longest sync of any mode, and the width of a chip's correlator
#define AIR_SYNCS_MAX		8
#define AIR_CHUNK_SYMS_MAX	144	// one 30 ms record at 4800 symbols/s: the most a chunk holds, either direction
#define AIR_SOFT_OUTER		96	// soft value of a +3 symbol at the profile's deviation; +1 is a third of it

typedef enum { AIR_SHAPE_RRC, AIR_SHAPE_C4FM, AIR_SHAPE_GAUSS, AIR_SHAPE_NRZ } air_shape_t;
typedef enum { AIR_TIMING_CONTINUOUS, AIR_TIMING_TDMA } air_timing_t;

// air_sync_t.flags
#define AIR_SYNC_START_ONLY	0x01	// on air only at the start of a transmission (a preamble's tail + the first sync)

// One fixed pattern a mode puts on air. A provider with a correlator of its own takes the first ones it can use (a
// modem chip: 24 symbols, all outer, not START_ONLY), as many as it holds, so a mode lists its patterns most useful
// first; the framer searches for all of them in the symbol stream itself
typedef struct {
	uint8_t id;			// the mode's own number for it; comes back in air_chunk_t.sync_id
	uint8_t nsyms;			// 8..AIR_SYNC_SYMS_MAX
	uint8_t max_errs;		// symbols in error still accepted
	uint8_t flags;			// AIR_SYNC_xxx
	// Where the frame starts, in symbols from the pattern's first: 0 the pattern opens the frame, positive the
	// frame starts inside it (a preamble ahead of the frame's own sync), negative the pattern sits inside the frame
	int8_t frame_at;
	uint8_t syms[AIR_SYNC_SYMS_MAX / 4];
} air_sync_t;

typedef struct {
	uint16_t sym_rate;		// symbols per second: 4800, 2400, 1200
	uint8_t levels;			// 2 or 4
	uint8_t shape;			// air_shape_t
	uint8_t shape_param;		// roll-off or BT, x 100
	uint16_t dev_outer_hz;		// deviation of the outer symbol; the inner one is a third of it (4 levels)
	uint8_t timing;			// air_timing_t
	uint8_t bw;			// radio_bw_t the mode wants
	uint16_t slot_syms;		// TDMA: symbols per slot; continuous: 0
	// The longest run of symbols between two 24-symbol patterns inside a transmission, 0 when none recurs (only
	// START_ONLY ones, or shorter syncs). A provider that receives only while it sees such patterns compares this
	// with what it can coast
	uint16_t sync_every_syms;
	// The longest run of symbols between two syncs of any length inside a transmission (a mode whose recurring
	// sync is shorter than 24 symbols); 0 = sync_every_syms. The soft modem holds its timing this long
	uint16_t sync_any_syms;
	uint8_t idle;			// four dibits a provider sends when a continuous transmission runs dry
	uint8_t nsyncs;
	air_sync_t sync[AIR_SYNCS_MAX];
} air_profile_t;

// What a provider answers for a profile (fit). 0 = can't
enum {
	AIR_FIT_RX		= 1u << 0,
	AIR_FIT_TX		= 1u << 1,
	AIR_FIT_SOFT		= 1u << 2,	// a soft value per received symbol
	AIR_FIT_SHAPE_APPROX	= 1u << 3,	// the transmit pulse is the provider's own, not the profile's
	AIR_FIT_DEV_APPROX	= 1u << 4,	// transmit deviation off by more than a tenth
	AIR_FIT_RATE_DOUBLED	= 1u << 5,	// carries half the provider's rate: the framer sends each symbol twice
	AIR_FIT_NEEDS_SYNC	= 1u << 6,	// receive starts only at a 24-symbol pattern: no late entry on shorter syncs
	AIR_FIT_TDMA		= 1u << 7,	// slot clock and per-slot keying
	AIR_FIT_FULL_DUPLEX	= 1u << 8,
	AIR_FIT_UNTESTED	= 1u << 15,	// from register facts, never checked on air on this board
};

// air_chunk_t.flags
#define AIR_CHUNK_GAP		0x01	// symbols were lost between the chunk before and this one
#define AIR_CHUNK_FREE_RUN	0x02	// longer than the profile's sync spacing since the provider saw a sync: its timing coasts
#define AIR_CHUNK_SLOT		0x04	// TDMA: symbol 0 opens a slot
#define AIR_CHUNK_NO_SIGNAL	0x08	// the provider knows nothing is on air (squelched): symbols are noise

// Received symbols, in air order. A chunk starts wherever the provider's record does, never at a frame boundary
typedef struct {
	uint32_t t_us;			// the provider's clock (now_us) at the center of symbol 0
	uint16_t nsyms;			// 1..AIR_CHUNK_SYMS_MAX
	int16_t sync_at;		// symbol where the provider's correlator saw a profile sync start; -1 none. A hint
	uint8_t sync_id;
	uint8_t flags;			// AIR_CHUNK_xxx
	int16_t rssi_dbm;		// 0 unknown
	const uint8_t *hard;		// dibits, as the provider sliced them
	const int8_t *soft;		// one per symbol, AIR_SOFT_OUTER = +3 at nominal; NULL without AIR_FIT_SOFT
} air_chunk_t;

// air_tx_chunk_t.flags
#define AIR_TX_LAST		0x01	// the transmission ends when this chunk has left the air
#define AIR_TX_SLOT_EMPTY	0x02	// TDMA: nothing is sent in this slot

typedef struct {
	uint16_t nsyms;			// tx_chunk_syms, or less in the AIR_TX_LAST chunk
	uint8_t flags;			// AIR_TX_xxx
	uint32_t at_slot;		// TDMA: the slot counter this chunk is for, 0 = the next free one
	uint8_t syms[AIR_CHUNK_SYMS_MAX / 4];
} air_tx_chunk_t;

typedef enum {
	AIR_EV_RX,		// chunk
	AIR_EV_TX_STARTED,	// t_us = the first queued symbol on air
	AIR_EV_TX_UNDERRUN,	// the queue ran dry before an AIR_TX_LAST chunk: idle symbols went out
	AIR_EV_TX_DONE,		// the AIR_TX_LAST chunk has left the air (or tx_abort is through); receiving again
	AIR_EV_TX_SPACE,	// a chunk left the queue for the air: tx_space grew
	AIR_EV_SLOT,		// TDMA: a slot boundary, arg = its counter, t_us = its time
} air_event_kind_t;

typedef struct {
	uint8_t kind;			// air_event_kind_t
	uint32_t t_us;
	uint32_t arg;
	const air_chunk_t *chunk;	// AIR_EV_RX only, valid during the callback
} air_event_t;

// Called by the provider at task level (its deferred handler), never from an interrupt. Must not block
typedef void (*air_event_cb_t)(const air_event_t *ev, void *ctx);

// A provider. One session at a time: open ... close. The core stops the board's DMR stack before open (the provider
// may run on its chip, interrupts and task) and starts it again after close. What every provider keeps while a
// session is open:
//  1. Raw receive never reaches the speaker: the speaker path carries only what the core plays through hal_audio_t.
//     open fails when the board can't guarantee that.
//  2. The microphone, tones and beeps never reach the modulator.
//  3. It keys the transmitter only inside the core's permission: between hal_radio_t.tx() (the core's inhibit, band
//     and power checks; on some boards it only arms the PA and the modem keys it) and tx_off(), which the core calls
//     at AIR_EV_TX_DONE. Never by itself.
//  4. close puts the board back so FM and DMR work as before, without a restart.
typedef struct {
	uint32_t (*fit)(const air_profile_t *p);
	// Start a session on the tuned channel (the board's RF side is already in its digital mode): receiving, events to
	// cb. p stays valid until close. False: no fit, or rule 1 or 2 can't be kept
	bool (*open)(const air_profile_t *p, air_event_cb_t cb, void *ctx);
	void (*close)(void);
	// Drop what the receiver holds and search again (after a retune; the framer calls it when it loses a signal)
	void (*rx_restart)(void);
	// Transmit: queue at least one chunk, hal_radio_t.tx(), then tx_begin. Modulation starts with the oldest queued chunk
	// (AIR_EV_TX_STARTED) and runs until the AIR_TX_LAST one. tx_queue is false when the queue is full; tx_space is the
	// number of chunks it takes now. tx_end: no more will come: as AIR_TX_LAST on the newest queued chunk. tx_abort:
	// stop at once, drop the queue; AIR_EV_TX_DONE follows
	bool (*tx_begin)(void);
	bool (*tx_queue)(const air_tx_chunk_t *c);
	uint8_t (*tx_space)(void);
	void (*tx_end)(void);
	void (*tx_abort)(void);
	uint32_t (*now_us)(void);	// free-running microseconds (wraps): the clock of every t_us here
	// Optional, TDMA providers (NULL: can't). The latest slot boundary; one slot from the next boundary us longer
	// (negative: shorter), returns the change it will make
	bool (*slot_clock)(uint32_t *counter, uint32_t *t_us);
	int32_t (*slot_slew)(int32_t us);
	// Optional (NULL: not counted, a chip does the work). What receive costs this MCU since open: the microseconds
	// the provider spent turning samples into symbols, and the microseconds of signal they covered. false: a chip
	// does the work for the profile that is open (nothing is counted, the two are 0)
	bool (*rx_load)(uint32_t *work_us, uint32_t *span_us);
	uint16_t tx_chunk_syms;		// symbols per transmit chunk (every chunk but the last is full)
	uint8_t tx_queue_depth;		// chunks the queue holds
	uint16_t tx_lead_ms;		// how far ahead of the air the queue must be kept: the framer holds this much queued
	uint16_t time_accuracy_us;	// of t_us
} hal_air_t;

#endif
