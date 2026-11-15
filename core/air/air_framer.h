// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The framer (docs/spec/air-interface.md 5): between the air interface's symbol chunks (hal_air.h) and a mode's
// frames. Receive: it finds the mode's sync patterns in the symbol stream, slices symbols from the soft values with
// the levels it measures on each sync, cuts frames wherever they fall in the chunks, coasts over missed syncs by
// counting, and says when the signal is lost. Transmit: it packs the mode's symbols into the provider's chunks.
//
// It knows a mode's geometry (air_mode_t), nothing of its content, and makes no RTOS or board call: the host tests
// run it as it is.
#ifndef RFOUNDRY_AIR_FRAMER_H
#define RFOUNDRY_AIR_FRAMER_H

#include <stdbool.h>
#include <stdint.h>

#include "../../hal/hal_air.h"

// air_frame_t.flags
#define AIR_FRAME_NO_SYNC	0x01	// no sync seen in this frame: placed by counting from the last one
#define AIR_FRAME_MORE		0x02	// a piece of a unit longer than the buffer: the next piece follows
#define AIR_FRAME_HEAD		0x04	// the head hook's view: the frame's first symbols, the rest not received yet

// One frame (a mode's unit between two frame starts) of channel symbols as they were on air, sync included, before
// any FEC. Valid during the callback
typedef struct {
	uint32_t t_us;			// the provider's clock at the center of the frame's first symbol
	uint8_t sync_id, sync_errs;	// the pattern seen (the last frame's while AIR_FRAME_NO_SYNC)
	uint8_t flags;			// AIR_FRAME_xxx
	uint16_t nsyms;			// symbols at hard / soft
	uint16_t offset;		// where they sit in the unit: 0, or more in the later pieces of a long unit
	int16_t rssi_dbm;		// 0 unknown
	const uint8_t *hard;		// dibits (hal_air.h), symbol offset first
	const int8_t *soft;		// one per symbol, scaled so +3 is AIR_SOFT_OUTER whatever the deviation; NULL
					// when the provider has none or the mode gave no buffer
} air_frame_t;

// The head hook's answer: the unit's length in symbols; 0 = not a frame of this mode (the framer searches on); or
// AIR_HEAD_AGAIN | n = ask again once n symbols are in (a length field deeper in the unit)
#define AIR_HEAD_AGAIN		0x8000u

typedef struct {
	const char *name;
	air_profile_t profile;
	uint16_t frame_syms;		// a frame's length; with a head hook, the length it may change
	// Symbols of a frame the head hook wants first, 0 = no hook. The hook confirms a sync (a short sync pattern also
	// turns up inside payload and noise) from the fields behind it and gives the unit's length where units differ
	uint16_t head_syms;
	uint8_t lost_frames;		// frames in a row without a sync before the signal counts as lost (1 or more)
	uint16_t (*head)(const air_frame_t *f, void *ctx);
	void (*rx_frame)(const air_frame_t *f, void *ctx);
	// The signal is gone (no syncs, a gap in the symbols, the head hook's 0 on a unit behind one it took): searching
	// again. Not called for a sync the head hook never agreed to
	void (*rx_lost)(void *ctx);
} air_mode_t;

typedef struct {
	uint16_t syncs;			// sync patterns seen where expected (the first of a lock included)
	uint16_t misses;		// frames placed by counting
	uint16_t locks;			// times a search ended in a lock
	uint16_t rejects;		// sync candidates dropped: the soft levels or the head hook said no
	uint16_t frames;		// frames handed to the mode
} air_framer_stats_t;

typedef struct {
	const air_mode_t *mode;
	void *ctx;
	uint8_t *hard;			// the mode's frame buffer: buf_syms dibits
	int8_t *soft;			// buf_syms soft values, or NULL
	uint16_t buf_syms;
	uint64_t reg;			// the last 24 symbols as sliced, the newest in bits 1-0
	int8_t hist[AIR_SYNC_SYMS_MAX];	// their soft values, a ring
	uint8_t hist_at;		// where the next one goes
	uint8_t state;
	uint8_t misses;			// frames in a row without a sync
	uint8_t sync_id, sync_errs;
	bool sync_seen;			// in this frame
	bool confirmed;			// the lock in hand: the head hook has agreed to a frame (always, without a hook)
	bool soft_in;			// the chunk in hand has soft values
	bool soft_out;			// ... and frames cut from it carry them (soft_in is off while a replay runs)
	bool relock;			// a replay after the head hook said no is running: no second one inside it
	uint16_t run;			// symbols in a row since the last reset or gap (saturates): how much history is valid
	uint16_t ring;			// search: the history ring's length in the frame buffer, 0 none kept
	uint16_t ring_at;
	uint16_t unit;			// the unit's length, once known
	uint16_t upos;			// symbols of the unit received
	uint16_t bpos;			// of them, in the buffer (after a piece went out: fewer than upos)
	uint16_t head_at;		// call the head hook when upos reaches this; 0 = done
	int16_t lvl, off;		// the outer symbol's soft level and the zero offset, as measured on syncs
	int16_t rssi_dbm;
	uint32_t t_frame;		// time of the unit's first symbol
	uint32_t t_sym;			// time of the symbol in hand
	air_framer_stats_t stats;
	// Transmit: the chunk being filled
	bool tx_double;			// each symbol goes out twice (a profile at half the provider's rate)
	air_tx_chunk_t txc;
} air_framer_t;

// hard: the frame buffer, (buf_syms + 3) / 4 bytes; soft: buf_syms bytes or NULL (frames then carry no soft values).
// buf_syms: the mode's longest unit, or less when it takes long units in pieces; never under 2 x AIR_SYNC_SYMS_MAX
void air_framer_init(air_framer_t *fr, const air_mode_t *mode, void *ctx, uint8_t *hard, int8_t *soft,
		     uint16_t buf_syms);
// Back to searching with nothing remembered (a retune, the start of a session). Never calls rx_lost
void air_framer_rx_reset(air_framer_t *fr);
// A received chunk (the provider's AIR_EV_RX). Calls the mode's hooks from inside
void air_framer_rx(air_framer_t *fr, const air_chunk_t *c);
// True while frames are being cut (a sync was found and the signal is not lost)
bool air_framer_locked(const air_framer_t *fr);

// Transmit. The mode sends everything on air as symbols, its preamble and end marker included; the framer packs
// them into the provider's chunks. air_framer_tx_start before the first symbol (fit: the provider's answer for the
// mode's profile); the caller keys and calls the provider's tx_begin once tx_lead_ms worth is queued.
void air_framer_tx_start(air_framer_t *fr, uint32_t fit);
// Symbols the provider's queue takes now
uint16_t air_framer_tx_space(const air_framer_t *fr, const hal_air_t *air);
// Queue nsyms dibits starting at symbol `first` of syms. False, and nothing taken, when they don't all fit
bool air_framer_tx(air_framer_t *fr, const hal_air_t *air, const uint8_t *syms, uint16_t first, uint16_t nsyms);
// The last symbol is in: send the open chunk (filled up with the profile's idle symbols where the provider needs
// whole chunks) as the transmission's last. False when the queue is full: call again
bool air_framer_tx_finish(air_framer_t *fr, const hal_air_t *air);

#endif
