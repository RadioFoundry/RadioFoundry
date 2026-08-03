// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// When the host link leaves hotspot mode (see hostlink.h). DTR is only an edge: a clear DTR from the start (MMDVMHost
// on Windows) never ends the session; a set DTR that drops (a POSIX host closing the port) does
// (docs/spec/mmdvm-protocol.md 5.1, test T-USB-2).
#include "hostlink/hostlink.h"

void hostlink_session_start(hostlink_session_t *s, bool connected)
{
	s->was_connected = connected;
}

hostlink_leave_t hostlink_session_check(hostlink_session_t *s, bool connected, bool present, bool exit_requested,
				       uint32_t idle_ms)
{
	bool dropped = s->was_connected && !connected;

	s->was_connected = connected;
	if (exit_requested) {
		return HOSTLINK_LEAVE_USER;
	}
	if (!present) {
		return HOSTLINK_LEAVE_GONE;
	}
	if (dropped) {
		return HOSTLINK_LEAVE_CLOSED;
	}
	if (idle_ms > HOSTLINK_HOTSPOT_IDLE_MS) {
		return HOSTLINK_LEAVE_IDLE;
	}
	return HOSTLINK_STAY;
}
