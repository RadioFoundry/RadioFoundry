// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The transmit side of an air-framed digital mode's radio core (P25, NXDN: radio_p25.h, radio_nxdn.h), as radio_sm.c
// drives it: one way to key, time, end and refuse a transmission for every such mode (docs/spec/air-interface.md 6).
// Each mode's module fills one table; radio_sm.c picks the table of the mode that runs.
#ifndef RADIO_AIR_H
#define RADIO_AIR_H

#include <stdbool.h>
#include <stdint.h>

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

#endif
