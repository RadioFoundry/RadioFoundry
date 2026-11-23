// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// YSF receive (ysf.h): the framer's table for the mode and its hooks. The sync is 20 symbols; the head
// hook takes a frame once the FICH behind it decodes (four Golay words and a CRC), or, inside a call, places a frame
// whose FICH is lost in a fade by the call's order for a few frames.
#include "ysf.h"

#include "../../hal/hal_radio.h"

#define COAST_FRAMES	3	// frames in a row taken without a FICH inside a call

static uint16_t rx_head(const air_frame_t *f, void *ctx)
{
	ysf_rx_t *rx = ctx;
	int errs = ysf_fich_get(f->hard, f->soft, &rx->work, &rx->cur);

	if (errs < 0) {
		if (!rx->in_call || rx->coasted >= COAST_FRAMES)
			return 0;
		rx->coasted++;
		rx->cur_errs = -1;
		return YSF_FRAME_SYMS;
	}
	rx->coasted = 0;
	rx->cur_errs = (int8_t)(errs > 127 ? 127 : errs);
	return YSF_FRAME_SYMS;
}

static void rx_frame(const air_frame_t *f, void *ctx)
{
	ysf_rx_t *rx = ctx;
	ysf_rx_frame_t fr;

	fr.fich_errs = rx->cur_errs;
	fr.fich = rx->cur;
	fr.flags = f->flags;
	fr.t_us = f->t_us;
	fr.rssi_dbm = f->rssi_dbm;
	fr.hard = f->hard;
	fr.soft = f->soft;
	if (rx->cur_errs >= 0)
		rx->in_call = rx->cur.fi == YSF_FI_HEADER || rx->cur.fi == YSF_FI_COMMS;
	if (rx->frame)
		rx->frame(&fr, rx->ctx);
}

static void rx_lost(void *ctx)
{
	ysf_rx_t *rx = ctx;

	rx->in_call = false;
	rx->coasted = 0;
	if (rx->lost)
		rx->lost(rx->ctx);
}

const air_mode_t ysf_air_mode = {
	.name = "YSF",
	.profile = {
		.sym_rate = 4800,
		.levels = 4,
		.shape = AIR_SHAPE_RRC,		// ysf.md 2: root raised cosine 0.2
		.shape_param = 20,
		.dev_outer_hz = 2700,
		.timing = AIR_TIMING_CONTINUOUS,
		.bw = RADIO_BW_12K5,
		.sync_every_syms = 0,		// no 24-symbol pattern
		.sync_any_syms = YSF_FRAME_SYMS,	// a sync opens every frame
		.idle = 0x77,			// +3 -3 +3 -3: the preamble's pattern
		// A soft modem adds its equalizer and tone notch (ysf.md 5.4): repeaters cut the 4FSK's upper band and some
		// send a CTCSS tone under it
		.rx_opts = AIR_RX_EQ | AIR_RX_TONE,
		.nsyncs = 1,
		.sync = {
			// D4 71 C9 63 4D: 40 bits, 20 symbols
			{ .id = 0, .nsyms = 20, .max_errs = 3, .frame_at = 0, .syms = { 0xD4, 0x71, 0xC9, 0x63, 0x4D } },
		},
	},
	.frame_syms = YSF_FRAME_SYMS,
	.head_syms = YSF_HEAD_SYMS,
	.lost_frames = 4,			// 400 ms without a sync
	.head = rx_head,
	.rx_frame = rx_frame,
	.rx_lost = rx_lost,
};

void ysf_rx_init(ysf_rx_t *rx, void (*frame)(const ysf_rx_frame_t *f, void *ctx), void (*lost)(void *ctx), void *ctx)
{
	rx->frame = frame;
	rx->lost = lost;
	rx->ctx = ctx;
	rx->cur_errs = -1;
	rx->in_call = false;
	rx->coasted = 0;
}
