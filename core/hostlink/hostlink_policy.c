// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// When the host link leaves hotspot mode (see hostlink.h). DTR is only an edge: a clear DTR from the start (MMDVMHost
// on Windows) never ends the session; a set DTR that drops (a POSIX host closing the port) does
// (docs/spec/mmdvm-protocol.md 5.1, test T-USB-2).
#include "hostlink/hostlink.h"
#include "hotspot/mmdvm.h"

#define MMDVM_CONFIG_LEN_MAX	64	// longer than any SET_CONFIG, SET_MODE or SET_FREQ

bool hostlink_mmdvm_first_frame(uint8_t len, uint8_t cmd)
{
	if ((len < 3) || (cmd >= MMDVM_ACK)) {
		return false;
	}
	switch (cmd) {
	case MMDVM_GET_VERSION:
	case MMDVM_GET_STATUS:
		return len == 3;
	case MMDVM_SET_CONFIG:
	case MMDVM_SET_MODE:
	case MMDVM_SET_FREQ:
		return len <= MMDVM_CONFIG_LEN_MAX;
	default:
		return true;
	}
}

bool hostlink_kiss_second(uint8_t b)
{
	return (b == 0xC0) || (b <= 0x06);
}

void hostlink_side_hold_set(hostlink_side_hold_t *h, bool on, uint32_t now_ms)
{
	h->held = on;
	h->last_ms = now_ms;
}

void hostlink_side_hold_frame(hostlink_side_hold_t *h, uint32_t now_ms)
{
	if (h->held) {
		h->last_ms = now_ms;
	}
}

bool hostlink_side_hold_active(hostlink_side_hold_t *h, uint32_t now_ms)
{
	if (h->held && ((now_ms - h->last_ms) >= HOSTLINK_SIDE_HOLD_MS)) {
		h->held = false;
	}
	return h->held;
}

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
