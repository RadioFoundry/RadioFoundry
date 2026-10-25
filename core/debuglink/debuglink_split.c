// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The debug link's SPLIT command (0x15, docs/spec/debuglink.md 3.15, docs/spec/split-duplex.md 10.1), built only with
// CONFIG_HOTSPOT_SPLIT (without it the command is unknown, as before):
//   op 1  variant u8, nregs u8 (<= 4), nregs x (page 4 register u8, value u8) -> status: the BS framing experiment's
//         variant the TX half's next key-up uses (raw pairs written after any variant's values)
//   op 2  -> status, then the split view as key=value text (role, epoch, offset, buffer levels, underruns, CACH)
#include "config.h"

#if CONFIG_HOTSPOT_SPLIT	// the whole file
#include <string.h>
#include "debuglink/debuglink.h"
#include "debuglink/debuglink_proto.h"
#include "hotspot/hotspotSplit.h"

enum { SPLIT_OP_VARIANT = 1, SPLIT_OP_VIEW = 2 };

void debuglink_view_split(dl_text_t *t)
{
	static const char *const roles[] = { "normal", "tx", "rx" };
	static const char *const cach[] = { "none", "ours", "chip", "unknown" };
	hotspot_split_view_t v;

	hotspot_split_get_view(&v);
	dl_text(t, "split.built=1\nsplit.role=%s\nsplit.offered=%u\nsplit.flags=0x%02x\nsplit.variant=%u\n",
		v.role < 3 ? roles[v.role] : "?", v.roles_offered, v.flags, v.variant);
	dl_text(t, "split.cach_method=%s\nsplit.keyed_s=%u\nsplit.lease_expiries=%u\nsplit.time_requests=%u\n",
		v.cach_method < 4 ? cach[v.cach_method] : "?", v.keyed_s, v.lease_expiries, (unsigned)v.time_requests);
	dl_text(t, "split.buf1=%u\nsplit.buf2=%u\nsplit.underruns1=%u\nsplit.underruns2=%u\n", v.buffered[0],
		v.buffered[1], v.underruns[0], v.underruns[1]);
	dl_text(t, "split.at_host=%u\nsplit.at_on_air=%u\nsplit.cach=%u\nsplit.idle=%u\nsplit.silence=%u\n"
		"split.closed=%u\nsplit.slc_taken=%u\nsplit.slc_bad=%u\n", v.at_host, v.at_on_air, v.cach, v.idle,
		v.silence, v.closed, v.slc_taken, v.slc_bad);
	dl_text(t, "split.epoch_us=%u\nsplit.period_ns=%u\nsplit.epoch_age_s=%u\nsplit.offset_us=%d\nsplit.track_us=%d\n",
		(unsigned)v.epoch_us, (unsigned)v.period_ns, v.epoch_age_s, (int)v.residual_us, (int)v.track_us);
	dl_text(t, "split.busy=%u\nsplit.labeled1=%u\nsplit.labeled2=%u\nsplit.unlabeled=%u\n", v.inbound_busy,
		v.labeled[0], v.labeled[1], v.unlabeled);
	dl_text(t, "split.stream=%u\nsplit.align=0x%02x\nsplit.phase_us=%d\nsplit.snap_adj_us=%d\nsplit.cbase=%u\n",
		v.stream, v.align_flags, (int)v.phase_us, (int)v.snap_adj_us, (unsigned)v.cbase);
	dl_text(t, "split.sched_sent=%u\nsplit.sched_late=%u\nsplit.sched_refused=%u\nsplit.sched_replaced=%u\n"
		"split.sched_cooldown=%u\n", v.sch_sent, v.sch_late, v.sch_refused, v.sch_replaced, v.sch_cool);
	dl_text(t, "split.slew_steps=%u\nsplit.slew_us=%d\nsplit.meta_sent=%u\n", v.slew_steps, (int)v.slew_us,
		v.meta_sent);
}

size_t debuglink_cmd_split(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap)
{
	if ((len >= 3) && (req[0] == SPLIT_OP_VARIANT)) {
		uint8_t n = req[2];
		uint8_t regs[4][2];

		if ((n > 4) || (len != 3u + 2u * n)) {
			out[0] = DL_ST_BAD_ARG;
			return 1;
		}
		for (uint8_t i = 0; i < n; i++) {
			regs[i][0] = req[3 + 2 * i];
			regs[i][1] = req[4 + 2 * i];
		}
		hotspot_split_set_variant(req[1], (const uint8_t (*)[2])regs, n);
		out[0] = DL_ST_OK;
		return 1;
	}
	if ((len == 1) && (req[0] == SPLIT_OP_VIEW)) {
		dl_text_t t;

		out[0] = DL_ST_OK;
		dl_text_init(&t, (char *)out + 1, cap - 1);
		debuglink_view_split(&t);
		return 1 + t.len;
	}
	out[0] = DL_ST_BAD_ARG;
	return 1;
}

#endif	// CONFIG_HOTSPOT_SPLIT
