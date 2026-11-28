// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Copyright (c) 2026 RadioFoundry Theme Pack contributors
// Bounded data-only theme pack transfer. The host never supplies a flash address.
#include "config.h"
#include "debuglink/debuglink.h"
#include "debuglink/debuglink_proto.h"
#include "radio/radio.h"
#include "ui/theme_pack.h"

static size_t reply_status(uint8_t *out, uint8_t value)
{
	out[0] = value;
	return 1u;
}

#if CONFIG_THEME_PACKS
static uint16_t get16(const uint8_t *p)
{
	return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
#endif

size_t debuglink_cmd_theme_pack(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap)
{
	if (cap == 0u)
		return 0u;
#if CONFIG_THEME_PACKS
	if (len == 0u)
		return reply_status(out, DL_ST_BAD_ARG);
	if (!ui_theme_pack_supported())
		return reply_status(out, DL_ST_UNAVAILABLE);
	switch (req[0]) {
	case DL_THEME_PACK_INFO:
		if (len != 1u || cap < 5u)
			return reply_status(out, DL_ST_BAD_ARG);
		out[0] = DL_ST_OK;
		out[1] = 1u;	// pack transfer protocol version
		out[2] = 0x00u;	// maximum pack length, 4096 bytes
		out[3] = 0x10u;
		out[4] = ui_theme_pack_count();
		return 5u;
	case DL_THEME_PACK_BEGIN:
		if (len != 3u)
			return reply_status(out, DL_ST_BAD_ARG);
		if (radio_get_state() == RS_TX)
			return reply_status(out, DL_ST_BUSY);
		return reply_status(out, ui_theme_pack_transfer_begin(get16(req + 1u)) ? DL_ST_OK : DL_ST_REFUSED);
	case DL_THEME_PACK_DATA:
		if (len < 4u || len > 64u)
			return reply_status(out, DL_ST_BAD_ARG);
		if (radio_get_state() == RS_TX)
			return reply_status(out, DL_ST_BUSY);
		return reply_status(out, ui_theme_pack_transfer_data(get16(req + 1u), req + 3u, len - 3u) ?
			DL_ST_OK : DL_ST_REFUSED);
	case DL_THEME_PACK_COMMIT:
		if (len != 1u)
			return reply_status(out, DL_ST_BAD_ARG);
		if (radio_get_state() == RS_TX)
			return reply_status(out, DL_ST_BUSY);
		return reply_status(out, ui_theme_pack_transfer_commit() ? DL_ST_OK : DL_ST_REFUSED);
	case DL_THEME_PACK_REMOVE:
		if (len != 1u)
			return reply_status(out, DL_ST_BAD_ARG);
		if (radio_get_state() == RS_TX)
			return reply_status(out, DL_ST_BUSY);
		return reply_status(out, ui_theme_pack_remove() ? DL_ST_OK : DL_ST_REFUSED);
	default:
		return reply_status(out, DL_ST_BAD_ARG);
	}
#else
	(void)req;
	(void)len;
	return reply_status(out, DL_ST_UNAVAILABLE);
#endif
}
