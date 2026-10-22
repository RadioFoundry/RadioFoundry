// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// The stub entry's own listener, for a build without the debug link (docs/spec/stub-entry.md 2): the commands the
// flashing tools send before the hand-over, on the debug link's framing (dl_frame.h) and with its bytes, so the same
// tools work with either listener. PING echoes, VERSION says what the radio is, STUB loads and enters the RAM monitor;
// any other command answers DL_ST_UNKNOWN_CMD. No RTOS or board here: the host tests run it.
#ifndef STUB_LINK_H
#define STUB_LINK_H

#include <stddef.h>
#include <stdint.h>
#include "debuglink/dl_frame.h"

// Reply payload bytes the listener's buffer holds: a PING echo (1 + DL_REQ_MAX) and the short VERSION text fit
#define STUB_LINK_REPLY_MAX	(1u + DL_REQ_MAX + 16u)

typedef struct {
	const char *board;	// VERSION board= (board.name)
	const char *git;	// VERSION git=
	const char *(*codec)(void);	// VERSION codec= (hal_vocoder_id), asked at each VERSION; may be NULL
	// The STUB command (stub_entry_request): reply payload in out, its length returned
	size_t (*stub)(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap);
} stub_link_t;

// One request's reply payload (out[0] = status), at most cap bytes; returns its length (>= 1)
size_t stub_link_handle(const stub_link_t *l, uint8_t cmd, const uint8_t *req, uint16_t len, uint8_t *out, size_t cap);

#endif
