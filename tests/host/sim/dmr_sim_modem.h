// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A simulated hal_dmr_modem_t for host tests: keeps a slot clock, sends queued bursts one per frame on our slot
// (recording what went "on air", with the embedded LC fragments it would spread over B-E), and lets a test inject
// received bursts and repeater activity.
#ifndef DMR_SIM_MODEM_H
#define DMR_SIM_MODEM_H

#include <stdint.h>
#include <stdbool.h>
#include "hal_dmr_modem.h"

#define SIM_AIR_MAX	512
#define SIM_QUEUE_DEPTH	8

typedef struct {
	uint8_t kind;		// dmr_burst_kind_t
	uint8_t voice_index;
	bool host_crc;
	uint8_t sync;		// dmr_tx_burst_t.sync_override
	uint8_t len;
	uint8_t payload[DMR_PAYLOAD_MAX];
	uint8_t lcss;		// voice: the EMB LCSS sent
	uint8_t emb[4];		// voice B-E: the 32 embedded signaling bits
	uint32_t slot;		// slot counter when sent
} sim_air_t;

typedef struct {
	// What the stack did
	bool started;
	dmr_config_t cfg;
	int8_t cc_filter;
	int8_t locked_ts;
	unsigned tx_begins;
	unsigned tx_listens;
	unsigned empty_slots;
	unsigned fills;		// tx_fill: a placeholder taken by a burst	// DMR_BURST_EMPTY taken: our slot left empty	// tx_listen calls during a transmission
	dmr_timing_t first_timing;	// of the first tx_begin since sim_reset
	int8_t first_ts;
	dmr_timing_t last_timing;
	int8_t last_ts;
	dmr_access_t last_access;
	unsigned tx_ends, tx_aborts, underflows;
	uint8_t emb_lc[9];
	bool emb_lc_set;

	// Behavior knobs
	bool refuse_tx;		// tx_begin returns false
	bool refuse_start;	// start returns false (a board whose modem is not in DMR mode)
	bool reject_next;	// send DMR_EV_TX_REJECTED at the next slot
	bool repeater;		// emit slot ticks with the slot number alternating (a repeater's CACH)
	bool wake_repeater_on_csbk;	// a BS_Dwn_Act on air starts the repeater (repeater = true)
	bool stalled;		// no slot timing: no ticks, nothing sent, no tx_done (a modem chip that doesn't answer)

	// State
	dmr_event_cb_t cb;
	void *ctx;
	uint32_t slot;
	bool tx_active;
	bool tx_ending;
	int8_t tx_parity;	// which slot parity is ours (0 / 1), set at tx_begin
	dmr_tx_burst_t q[SIM_QUEUE_DEPTH];
	uint8_t q_head, q_count;
	uint8_t cur_emb_lc[9];
	uint8_t pending_emb_lc[9];
	bool pending_emb;
	sim_air_t air[SIM_AIR_MAX];
	unsigned air_count;
} sim_modem_t;

extern sim_modem_t sim;
extern const hal_dmr_modem_t sim_modem;

void sim_reset(void);
// Advance one 30 ms slot: tick, send our burst if it is our slot, tx_done when drained
void sim_slot(void);
// Deliver a received burst to the stack
void sim_rx(const dmr_burst_t *b);
void sim_event(uint8_t kind, int8_t ts);

#endif
