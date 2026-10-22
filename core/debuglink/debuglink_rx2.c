// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// DL_CMD_RX2 (docs/spec/debuglink.md 3.22): the second receiver's SoC on a radio with two (board.rx2). INFO says what is
// there; BRIDGE hands the PC cable to the second SoC so the host can load its stub and write its flash
// (docs/spec/rx2-link.md 4); RESTART restarts it. The bridge starts after the reply has gone out, as cmd_reboot's reset
// does, and never returns.
#include "config.h"
#include "board.h"
#include "hal_time.h"
#include "debuglink/debuglink.h"
#include "debuglink/debuglink_proto.h"
#include "debuglink/dlog.h"
#include "radio/radio.h"

#define RX2_BRIDGE_DELAY_MS	100u	// let the reply leave the UART first

static bool bridge_pending;

static bool on_air(void)
{
	radio_view_t v;
	radio_get_view(&v);
	return v.state == RS_TX;
}

static uint8_t flags(void)
{
	const hal_rx2_t *r = board.rx2;
	if (r == NULL) {
		return 0;
	}
	rx2_status_t st = { 0 };
	if (r->status != NULL) {
		r->status(&st);
	}
	return DL_RX2_F_PRESENT | (st.up ? DL_RX2_F_UP : 0) | (r->bridge != NULL ? DL_RX2_F_BRIDGE : 0)
	       | (r->restart != NULL ? DL_RX2_F_RESTART : 0);
}

size_t debuglink_cmd_rx2(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap)
{
	(void)cap;
	if (len < 1) {
		out[0] = DL_ST_BAD_ARG;
		return 1;
	}
	if (req[0] == DL_RX2_OP_INFO && len == 1) {
		out[0] = DL_ST_OK;
		out[1] = flags();
		return 2;
	}
	if ((req[0] != DL_RX2_OP_BRIDGE && req[0] != DL_RX2_OP_RESTART) || len != 5) {
		out[0] = DL_ST_BAD_ARG;
		return 1;
	}
	uint32_t token = (uint32_t)req[1] | (uint32_t)req[2] << 8 | (uint32_t)req[3] << 16 | (uint32_t)req[4] << 24;
	const hal_rx2_t *r = board.rx2;
	void (*op)(void) = r == NULL ? NULL : req[0] == DL_RX2_OP_BRIDGE ? r->bridge : r->restart;
	if (op == NULL) {
		out[0] = DL_ST_UNAVAILABLE;
		return 1;
	}
	if (token != DL_RX2_TOKEN || on_air()) {
		out[0] = DL_ST_REFUSED;	// a stray frame, or the transmitter is keyed
		return 1;
	}
	if (req[0] == DL_RX2_OP_RESTART) {
		dlog("debuglink: second receiver restart");
		op();
	} else {
		dlog("debuglink: bridging the cable to the second receiver in %u ms", (unsigned)RX2_BRIDGE_DELAY_MS);
		bridge_pending = true;
	}
	out[0] = DL_ST_OK;
	return 1;
}

void debuglink_rx2_service(void)
{
	if (!bridge_pending) {
		return;
	}
	bridge_pending = false;
	hal_delay_ms(RX2_BRIDGE_DELAY_MS);
	if (on_air()) {
		return;		// a PTT came in meanwhile
	}
	board.rx2->bridge();	// does not return
}
