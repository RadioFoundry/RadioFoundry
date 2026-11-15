// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// P25 receive (p25.h): the framer's table for the mode and its hooks. The head hook reads the NID behind a frame
// sync: it tells a P25 unit from a chance match of the sync, drops other systems' units (the NAC), and gives the
// unit's length. Clause numbers are TIA-102.BAAA-A's.

#include "p25.h"

#include "../../hal/hal_radio.h"

#define COAST_UNITS	2	// LDUs in a row placed by the call's order when their NID can't be read

static bool is_ldu(uint8_t duid)
{
	return duid == P25_DUID_LDU1 || duid == P25_DUID_LDU2;
}

static uint16_t rx_head(const air_frame_t *f, void *ctx)
{
	p25_rx_t *rx = ctx;
	uint16_t nac, n;
	uint8_t duid;
	int errs = p25_nid_get(f->hard, &nac, &duid);

	if (errs >= 0 && (n = p25_unit_syms(duid)) != 0) {
		if (rx->nac != P25_NAC_ANY && nac != rx->nac)
			return 0;	// another system on the channel
		rx->cur_nac = nac;
		rx->cur_duid = duid;
		rx->cur_errs = (int8_t)errs;
		rx->coasted = 0;
		return n;
	}
	// In a voice call the LDUs alternate (8.2.2): one whose NID is lost in a fade still carries voice, and dropping
	// the lock would cost the next one too. A unit of a kind we don't take (a packet) ends it at once
	if (errs < 0 && is_ldu(rx->last_duid) && rx->coasted < COAST_UNITS) {
		rx->cur_duid = rx->last_duid == P25_DUID_LDU1 ? P25_DUID_LDU2 : P25_DUID_LDU1;
		rx->cur_errs = -1;
		rx->coasted++;
		return P25_LDU_SYMS;
	}
	return 0;
}

static void rx_frame(const air_frame_t *f, void *ctx)
{
	p25_rx_t *rx = ctx;
	p25_rx_unit_t u;

	u.duid = rx->cur_duid;
	u.nac = rx->cur_nac;
	u.nid_errs = rx->cur_errs;
	u.flags = f->flags;
	u.t_us = f->t_us;
	u.rssi_dbm = f->rssi_dbm;
	u.nsyms = f->nsyms;
	u.air = f->hard;
	rx->last_duid = rx->cur_duid;
	if (rx->unit)
		rx->unit(&u, rx->ctx);
}

static void rx_lost(void *ctx)
{
	p25_rx_t *rx = ctx;

	rx->last_duid = 0xFF;
	rx->coasted = 0;
	if (rx->lost)
		rx->lost(rx->ctx);
}

const air_mode_t p25_air_mode = {
	.name = "P25",
	.profile = {
		.sym_rate = 4800,
		.levels = 4,
		.shape = AIR_SHAPE_C4FM,	// 9.3, 9.4: raised cosine 0.2 and the inverse sinc shaper
		.shape_param = 20,
		.dev_outer_hz = 1800,		// table 9-1
		.timing = AIR_TIMING_CONTINUOUS,
		.bw = RADIO_BW_12K5,
		.sync_every_syms = P25_LDU_SYMS,	// the longest unit: a frame sync opens every one
		.idle = 0x5F,			// +3 +3 -3 -3: the deviation test pattern (9.4.1), nothing a receiver frames
		.nsyncs = 1,
		.sync = { {
			.id = 0,
			.nsyms = 24,
			.max_errs = 4,
			.frame_at = 0,
			.syms = { 0x55, 0x75, 0xF5, 0xFF, 0x77, 0xFF },	// 8.3
		} },
	},
	.frame_syms = P25_LDU_SYMS,
	.head_syms = P25_HEAD_SYMS,
	.lost_frames = 2,
	.head = rx_head,
	.rx_frame = rx_frame,
	.rx_lost = rx_lost,
};

void p25_rx_init(p25_rx_t *rx, uint16_t nac, void (*unit)(const p25_rx_unit_t *u, void *ctx), void (*lost)(void *ctx),
		 void *ctx)
{
	rx->nac = nac;
	rx->unit = unit;
	rx->lost = lost;
	rx->ctx = ctx;
	rx->cur_nac = 0;
	rx->cur_duid = 0xFF;
	rx->cur_errs = 0;
	rx->last_duid = 0xFF;
	rx->coasted = 0;
}
