// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Power tuning on the debug link (DL_CMD_TUNE, docs/spec/debuglink.md 3.31, docs/spec/power-tune.md): what the radio
// has, the record in force, each level's limits at a frequency, a try-out offset, and the edits. Binary, little endian.
#include <string.h>
#include "board.h"
#include "hal_time.h"
#include "debuglink/debuglink.h"
#include "debuglink/debuglink_proto.h"
#include "radio/power_tune.h"
#include "radio/radio.h"

#if CONFIG_POWER_TUNE

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static size_t put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	return 2;
}

static size_t put32(uint8_t *p, uint32_t v)
{
	put16(p, (uint16_t)v);
	put16(p + 2, (uint16_t)(v >> 16));
	return 4;
}

// A name as a length byte and its characters; false when it doesn't fit
static bool put_name(uint8_t *out, size_t cap, size_t *n, const char *s)
{
	size_t l = strlen(s);
	if (l > 0xFFu || *n + 1u + l > cap) {
		return false;
	}
	out[(*n)++] = (uint8_t)l;
	memcpy(out + *n, s, l);
	*n += l;
	return true;
}

// version, flags (bit 0 the store, 1 the board's floor, 2 a preview), copies, seq u32, points and names the radio
// keeps, name length, levels, transmit bands; then each band's edges in kHz; then each band's factory level names
static size_t info(uint8_t *out, size_t cap)
{
	power_tune_info_t i;
	uint8_t bands = 0;
	size_t n = 1;
	power_tune_info(&i, hal_millis());
	out[n++] = DL_TUNE_VERSION;
	out[n++] = (uint8_t)((i.store ? 1u : 0u) | (board.radio != NULL && board.radio->tx_drive_floor != NULL ? 2u : 0u) |
			     (i.preview ? 4u : 0u));
	out[n++] = i.copies;
	n += put32(out + n, i.seq);
	out[n++] = POWER_TUNE_POINTS;
	out[n++] = POWER_TUNE_LABELS;
	out[n++] = POWER_TUNE_LABEL_LEN;
	out[n++] = board.power_count;
	size_t nb = n++;
	for (uint8_t b = 0; b < board_band_count() && n + 8u <= cap; b++) {
		if (board.bands[b].tx) {
			n += put32(out + n, board.bands[b].lo_hz / 1000u);
			n += put32(out + n, board.bands[b].hi_hz / 1000u);
			bands++;
		}
	}
	out[nb] = bands;
	for (uint8_t b = 0; b < board_band_count(); b++) {
		for (uint8_t l = 0; board.bands[b].tx && l < board.power_count; l++) {
			if (!put_name(out, cap, &n, board_power_name(l, board.bands[b].lo_hz))) {
				return 0;
			}
		}
	}
	return n;
}

// Each level at khz: factory, tuned, lowest and highest drive (u16 each), then its name as shown
static size_t limits(uint32_t khz, uint8_t *out, size_t cap)
{
	size_t n = 1;
	out[n++] = board.power_count;
	for (uint8_t l = 0; l < board.power_count; l++) {
		power_tune_limits_t lim;
		if (n + 8u > cap) {
			return 0;
		}
		power_tune_limits(khz * 1000u, l, &lim);
		n += put16(out + n, lim.factory);
		n += put16(out + n, lim.tuned);
		n += put16(out + n, lim.lo);
		n += put16(out + n, lim.hi);
	}
	for (uint8_t l = 0; l < board.power_count; l++) {
		if (!put_name(out, cap, &n, power_tune_name(l, khz * 1000u))) {
			return 0;
		}
	}
	return n;
}

static uint8_t edit_status(power_tune_result_t r)
{
	switch (r) {
	case POWER_TUNE_OK:
		return DL_ST_OK;
	case POWER_TUNE_BAD:
		return DL_ST_BAD_ARG;
	case POWER_TUNE_FULL:
		return DL_ST_REFUSED;
	case POWER_TUNE_NO_STORE:
		return DL_ST_UNAVAILABLE;
	default:
		return DL_ST_BUSY;
	}
}

size_t debuglink_cmd_tune(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap)
{
	power_tune_edit_t e;
	size_t n = 0;
	out[0] = DL_ST_BAD_ARG;
	if (len == 0u) {
		return 1;
	}
	memset(&e, 0, sizeof(e));
	switch (req[0]) {
	case DL_TUNE_OP_INFO:
		n = len == 1u ? info(out, cap) : 0u;
		break;
	case DL_TUNE_OP_READ:
		if (len == 1u) {
			n = 1u + (cap > 1u ? power_tune_read(out + 1, cap - 1u) : 0u);
		}
		break;
	case DL_TUNE_OP_LIMITS:
		n = len == 5u ? limits(get32(req + 1), out, cap) : 0u;
		break;
	case DL_TUNE_OP_PREVIEW:
		if (len == 4u && power_tune_preview(req[1], (int16_t)(req[2] | req[3] << 8), hal_millis())) {
			n = 1;
		}
		break;
	case DL_TUNE_OP_PUT_POINT:
	case DL_TUNE_OP_DEL_POINT:
	case DL_TUNE_OP_PUT_LABEL:
	case DL_TUNE_OP_CLEAR:
		if (req[0] == DL_TUNE_OP_CLEAR ? len != 1u : len < 6u) {
			return 1;
		}
		if (req[0] != DL_TUNE_OP_CLEAR) {
			e.khz = get32(req + 1);
			e.level = req[5];
		}
		if (req[0] == DL_TUNE_OP_PUT_POINT) {
			if (len != 12u) {
				return 1;
			}
			e.op = POWER_TUNE_PUT_POINT;
			e.delta = (int16_t)(req[6] | req[7] << 8);
			e.mw = get32(req + 8);
		} else if (req[0] == DL_TUNE_OP_DEL_POINT) {
			if (len != 6u) {
				return 1;
			}
			e.op = POWER_TUNE_DEL_POINT;
		} else if (req[0] == DL_TUNE_OP_PUT_LABEL) {
			if (len < 10u || len > 10u + POWER_TUNE_LABEL_LEN) {
				return 1;
			}
			e.op = POWER_TUNE_PUT_LABEL;
			e.mw = get32(req + 6);
			memcpy(e.name, req + 10, (size_t)(len - 10u));
			if (strlen(e.name) != (size_t)(len - 10u)) {
				return 1;	// a NUL inside
			}
		} else {
			e.op = POWER_TUNE_CLEAR;
		}
		if (radio_get_state() == RS_TX) {
			out[0] = DL_ST_REFUSED;		// no flash erase under a transmission
			return 1;
		}
		out[0] = edit_status(power_tune_edit(&e));
		return 1;
	default:
		out[0] = DL_ST_UNKNOWN_CMD;
		return 1;
	}
	if (n == 0u) {
		return 1;
	}
	out[0] = DL_ST_OK;
	return n;
}

#endif
