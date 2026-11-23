// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The transmit side of an air-framed digital mode's radio core (P25, NXDN: radio_p25.h, radio_nxdn.h), as radio_sm.c
// drives it: one way to key, time, end and refuse a transmission for every such mode (docs/spec/air-interface.md 6).
// Each mode's module fills one table; radio_sm.c picks the table of the mode that runs.
#ifndef RADIO_AIR_H
#define RADIO_AIR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "config.h"

typedef struct {
	bool (*running)(void);		// the engine runs on this channel
	// The provider can't transmit this mode's profile (it opened for receive only): PTT is refused with
	// TXDENY_AIR_NO_TX before anything is queued. NULL: not checked (the prepare refuses instead)
	bool (*no_tx)(void);
	bool (*tx_prepare)(void);	// the preamble and first frame queued; false: refused
	bool (*tx_go)(void);		// after hal_radio_t.tx(): the provider starts
	void (*tx_stop)(void);		// PTT released: the frame on air is finished, then the end of the transmission
	void (*tx_abort)(void);
	uint8_t (*tx_state)(void);	// air_tx_state_t
	void (*tx_clear)(void);		// after AIR_TX_DONE and hal_radio_t.tx_off()
} radio_air_ops_t;

// The counters every air voice mode keeps (radio_nxdn_stats_t, radio_ysf_stats_t): its engine's and its session's
typedef struct {
	uint32_t rx_calls, tx_calls, tx_filler;
	uint32_t rx_chunks, tx_underruns, tx_syms;
} radio_air_counts_t;

// Where the NXDN and YSF engines run: P25's memory while P25 idles, where the board's vocoder doesn't lend P25 its
// own; else one block both share (radio_air_mem.c), as only one air mode runs at a time. *len: its size; NULL: none
#if CONFIG_NXDN || CONFIG_YSF
#include "air/air_session.h"

void *radio_air_engine_mem(size_t *len);
// That memory for an engine of `need` bytes; NULL: none, too small or not 8-aligned
void *radio_air_engine_get(size_t need);

// The DMR side's voice path (ambeVoice.h) as NXDN and YSF use it (radio_air_voice.c). The receive and the frame
// source run in the provider's task, the only one feeding the path while such a mode runs (the DMR stack is stopped)
void radio_air_voice_rx_start(void);
void radio_air_voice_rx_frame(const uint8_t *v);
void radio_air_voice_rx_end(void);
bool radio_air_voice_tx_next(uint8_t *v);
// The encoder from the header on (on), so the first voice frames are ready when the engine asks, or off
void radio_air_voice_tx(bool on);

// The engine's counters into *total. They restart with each engine (*base zeroed at its begin), so what moved since
// the last look is added
void radio_air_count(radio_air_counts_t *total, radio_air_counts_t *base, uint32_t rx_calls, uint32_t tx_calls,
		     uint32_t tx_filler, const air_session_stats_t *a);
#endif

#endif
