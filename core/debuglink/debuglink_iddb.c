// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// DL_CMD_IDDB (docs/spec/debuglink.md 3.23): the DMR ID database as the radio sees it, its format and records, and the
// text the radio shows for an ID. A HIL test compares that with the database the host wrote, in either format
// (open-firmware-cps.md 7.5, iddb-compact.md), without a call on the air.
#include <string.h>
#include "config.h"
#include "debuglink/debuglink.h"
#include "debuglink/debuglink_proto.h"
#if CONFIG_DMR
#include "codeplug/dmr_iddb.h"
#endif

size_t debuglink_cmd_iddb(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap)
{
#if CONFIG_DMR
	if (len != 4u || cap < 7u + DMR_IDDB_TEXT_MAX) {
		out[0] = DL_ST_BAD_ARG;
		return 1;
	}
	uint32_t id = (uint32_t)req[0] | (uint32_t)req[1] << 8 | (uint32_t)req[2] << 16 | (uint32_t)req[3] << 24;
	uint32_t count = dmr_iddb_count();
	char text[DMR_IDDB_TEXT_MAX + 1];
	bool found = dmr_iddb_lookup(id, text, sizeof(text));
	size_t n = found ? strlen(text) : 0u;

	out[0] = DL_ST_OK;
	out[1] = (uint8_t)dmr_iddb_format();
	out[2] = (uint8_t)count;
	out[3] = (uint8_t)(count >> 8);
	out[4] = (uint8_t)(count >> 16);
	out[5] = (uint8_t)(count >> 24);
	out[6] = found ? 1u : 0u;
	memcpy(out + 7, text, n);
	return 7u + n;
#else
	(void)req;
	(void)len;
	(void)cap;
	out[0] = DL_ST_UNAVAILABLE;
	return 1;
#endif
}
