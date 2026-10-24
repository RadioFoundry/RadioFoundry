// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The board's hardware options on the debug link (DL_CMD_HWOPT, docs/spec/debuglink.md 3.23, hal/hal_hwopt.h): list
// them with their choices and stored values, or store one. A radio without options answers an empty list.
#include "board.h"
#include "debuglink/debuglink.h"
#include "debuglink/debuglink_proto.h"
#include "debuglink/dl_fmt.h"
#include "radio/radio.h"

static void list(dl_text_t *t)
{
	dl_text(t, "hwopt.count=%u\n", board.hwopts != NULL ? board.hwopt_count : 0u);
	for (uint8_t i = 0; board.hwopts != NULL && i < board.hwopt_count; i++) {
		const hal_hwopt_t *o = &board.hwopts[i];
		char key[24];
		dl_text(t, "hwopt.%u.value=%d\nhwopt.%u.restart=%u\n", i, o->get(), i, o->restart);
		(void)dl_fmt(key, sizeof(key), "hwopt.%u.name", i);
		dl_text_kv_str(t, key, o->name);
		dl_text(t, "hwopt.%u.choices=", i);
		for (uint8_t c = 0; c < o->count; c++) {
			dl_text(t, c ? "|%s" : "%s", o->choices[c]);
		}
		dl_text(t, "\n");
	}
}

size_t debuglink_cmd_hwopt(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap)
{
	if (len >= 1 && req[0] == DL_HWOPT_OP_LIST && len == 1) {
		dl_text_t t;
		out[0] = DL_ST_OK;
		dl_text_init(&t, (char *)out + 1, cap - 1);
		list(&t);
		return 1 + t.len;
	}
	if (len == 3 && req[0] == DL_HWOPT_OP_SET) {
		if (board.hwopts == NULL || req[1] >= board.hwopt_count || req[2] >= board.hwopts[req[1]].count) {
			out[0] = DL_ST_BAD_ARG;
			return 1;
		}
		radio_view_t v;
		radio_get_view(&v);
		if (v.state == RS_TX) {
			out[0] = DL_ST_REFUSED;		// never change the hardware's description under a transmission
			return 1;
		}
		out[0] = board.hwopts[req[1]].set(req[2]) ? DL_ST_OK : DL_ST_REFUSED;
		return 1;
	}
	out[0] = DL_ST_BAD_ARG;
	return 1;
}
