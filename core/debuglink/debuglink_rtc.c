// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// DL_CMD_RTC (docs/spec/debuglink.md 3.31): read or set the board's hardware clock (board.rtc, hal_rtc.h). This task
// is the only caller of the clock's bus, so a read or a write never meets another.
#include "config.h"

#if CONFIG_RTC && CONFIG_DEBUGLINK
#include "board.h"
#include "debuglink/debuglink.h"
#include "debuglink/debuglink_proto.h"
#include "debuglink/dlog.h"

static size_t status_only(uint8_t *out, uint8_t status)
{
	out[0] = status;
	return 1;
}

// GET (no argument): valid u8 (0 the clock holds no time), unix u32. SET: unix u32. The time is UTC seconds since 1970
size_t debuglink_cmd_rtc(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap)
{
	if (cap < 6u) {
		return status_only(out, DL_ST_BAD_ARG);
	}
	if (len < 1u) {
		return status_only(out, DL_ST_BAD_ARG);
	}
	if (board.rtc == NULL) {
		return status_only(out, DL_ST_UNAVAILABLE);
	}
	if (req[0] == DL_RTC_GET && len == 1u && board.rtc->read != NULL) {
		int64_t t;
		bool valid = board.rtc->read(&t) && t >= 0 && t <= 0xFFFFFFFFLL;
		uint32_t v = valid ? (uint32_t)t : 0u;
		out[0] = DL_ST_OK;
		out[1] = valid;
		out[2] = (uint8_t)v;
		out[3] = (uint8_t)(v >> 8);
		out[4] = (uint8_t)(v >> 16);
		out[5] = (uint8_t)(v >> 24);
		return 6;
	}
	if (req[0] == DL_RTC_SET && len == 5u && board.rtc->set != NULL) {
		uint32_t v = (uint32_t)req[1] | ((uint32_t)req[2] << 8) | ((uint32_t)req[3] << 16) | ((uint32_t)req[4] << 24);
		if (!board.rtc->set((int64_t)v)) {
			return status_only(out, DL_ST_REFUSED);	// out of the clock's range, or its bus did not answer
		}
		dlog("debuglink: rtc set %lu", (unsigned long)v);
		return status_only(out, DL_ST_OK);
	}
	return status_only(out, DL_ST_BAD_ARG);
}

#endif
