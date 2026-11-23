// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// NXDN receive (nxdn.h): the framer's tables for the mode and its hooks. The FSW is ten symbols, so it also turns
// up in payload and noise: the head hook takes a frame only when the LICH behind it reads as a conventional
// channel's (parity, RF channel type, all outer symbols) and drops frames of another RAN (a SACCH whose CRC holds).
// Section numbers are NXDN TS 1-A's.

#include "nxdn.h"

#include "../air/air_syms.h"
#include "../../hal/hal_radio.h"

#define COAST_FRAMES	2	// superframe frames in a row placed by the call's order when their LICH can't be read
#define LICH_INNER_MAX	1	// LICH symbols are all outer (figure 4.5-7): more inner ones than this is not a LICH

static bool superframe(int16_t lich)
{
	return lich >= 0 && (NXDN_LICH_USC(lich) == NXDN_USC_SACCH_SF || NXDN_LICH_USC(lich) == NXDN_USC_SACCH_SF_IDLE);
}

// A conventional voice channel's frame: RDCH, SACCH (not UDCH)
static bool lich_ok(const air_frame_t *f, int lich)
{
	uint8_t inner = 0;

	if (lich < 0 || NXDN_LICH_RF(lich) != NXDN_RF_RDCH || NXDN_LICH_USC(lich) == NXDN_USC_UDCH)
		return false;
	for (uint8_t i = 0; i < 8; i++) {
		if (f->soft) {
			int8_t v = f->soft[NXDN_LICH_SYM + i];

			inner += (v < 0 ? -v : v) < 2 * AIR_SOFT_OUTER / 3;
		} else {
			inner += !(air_sym_get(f->hard, (uint16_t)(NXDN_LICH_SYM + i)) & 1);
		}
	}
	return inner <= LICH_INNER_MAX;
}

static uint16_t rx_head(const air_frame_t *f, void *ctx)
{
	nxdn_rx_t *rx = ctx;
	int lich = nxdn_lich_get(f->hard, f->soft);
	bool ok = lich_ok(f, lich);

	// A frame whose LICH is lost in a fade, in a call's superframes: its voice is still worth trying
	if (!ok && !(superframe(rx->last_lich) && rx->coasted < COAST_FRAMES))
		return 0;
	rx->cur_errs = (int8_t)nxdn_sacch_get(f->hard, f->soft, &rx->work, rx->cur_sacch);
	if (rx->cur_errs >= 0 && rx->ran != NXDN_RAN_ANY && NXDN_SR_RAN(rx->cur_sacch[0]) != rx->ran) {
		rx->ran_rejects++;
		return 0;	// another system on the channel (6.3.2)
	}
	if (ok) {
		rx->cur_lich = (int16_t)lich;
		rx->coasted = 0;
	} else {
		rx->cur_lich = -1;
		rx->coasted++;
	}
	return NXDN_FRAME_SYMS;
}

static void rx_frame(const air_frame_t *f, void *ctx)
{
	nxdn_rx_t *rx = ctx;
	nxdn_rx_frame_t fr;

	fr.lich = rx->cur_lich;
	fr.sacch_errs = rx->cur_errs;
	for (uint8_t i = 0; i < NXDN_SACCH_BYTES; i++)
		fr.sacch[i] = rx->cur_sacch[i];
	fr.flags = f->flags;
	fr.t_us = f->t_us;
	fr.rssi_dbm = f->rssi_dbm;
	fr.hard = f->hard;
	fr.soft = f->soft;
	if (rx->cur_lich >= 0)
		rx->last_lich = rx->cur_lich;
	if (rx->frame)
		rx->frame(&fr, rx->ctx);
}

static void rx_lost(void *ctx)
{
	nxdn_rx_t *rx = ctx;

	rx->last_lich = -1;
	rx->coasted = 0;
	if (rx->lost)
		rx->lost(rx->ctx);
}

// 4.4.4: the FSW, -3 +1 -3 +3 -3 -3 +3 +3 -1 +3; 4.4.3 and 5.3.1.2: the preamble's last 9 symbols with it, which
// a receiver may lock on at once (N6 = 1, 3 bit errors) where the FSW alone needs two (N5 = 2, 2 bit errors). The
// head hook stands in for the second FSW
#define NXDN_SYNCS \
	.idle = 0x5F,			/* +3 +3 -3 -3: the reference deviation pattern (3.6) */ \
	.nsyncs = 2, \
	.sync = { \
		{ .id = 0, .nsyms = 10, .max_errs = 2, .frame_at = 0, .syms = { 0xCD, 0xF5, 0x90 } }, \
		{ .id = 1, .nsyms = 19, .max_errs = 3, .flags = AIR_SYNC_START_ONLY, .frame_at = 9, \
		  .syms = { 0xDD, 0x7F, 0x73, 0x7D, 0x64 } }, \
	}

const air_mode_t nxdn_air_mode = {
	.name = "NXDN",
	.profile = {
		.sym_rate = 2400,
		.levels = 4,
		.shape = AIR_SHAPE_RRC,		// 3.4: root raised cosine 0.2 (and a sinc shaper, see nxdn.md 2)
		.shape_param = 20,
		.dev_outer_hz = 1050,		// table 3.3-1, 4800 bit/s
		.timing = AIR_TIMING_CONTINUOUS,
		.bw = RADIO_BW_12K5,		// the narrowest the radios have; the channel is 6.25 kHz
		.sync_every_syms = 0,		// no 24-symbol pattern
		.sync_any_syms = NXDN_FRAME_SYMS,	// an FSW opens every frame
		NXDN_SYNCS,
	},
	.frame_syms = NXDN_FRAME_SYMS,
	.head_syms = NXDN_HEAD_SYMS,
	.lost_frames = 5,			// N7 (table 5.9-3)
	.head = rx_head,
	.rx_frame = rx_frame,
	.rx_lost = rx_lost,
};

const air_mode_t nxdn_air_mode_9600 = {
	.name = "NXDN 9600",
	.profile = {
		.sym_rate = 4800,
		.levels = 4,
		.shape = AIR_SHAPE_RRC,
		.shape_param = 20,
		.dev_outer_hz = 2400,		// table 3.3-1, 9600 bit/s
		.timing = AIR_TIMING_CONTINUOUS,
		.bw = RADIO_BW_12K5,
		.sync_every_syms = 0,
		.sync_any_syms = NXDN_FRAME_SYMS,
		NXDN_SYNCS,
	},
	.frame_syms = NXDN_FRAME_SYMS,
	.head_syms = NXDN_HEAD_SYMS,
	.lost_frames = 5,
	.head = rx_head,
	.rx_frame = rx_frame,
	.rx_lost = rx_lost,
};

void nxdn_rx_init(nxdn_rx_t *rx, uint8_t ran, void (*frame)(const nxdn_rx_frame_t *f, void *ctx), void (*lost)(void *ctx),
		  void *ctx)
{
	rx->ran = ran;
	rx->frame = frame;
	rx->lost = lost;
	rx->ctx = ctx;
	rx->cur_lich = -1;
	rx->cur_errs = -1;
	rx->last_lich = -1;
	rx->coasted = 0;
	rx->ran_rejects = 0;
}
