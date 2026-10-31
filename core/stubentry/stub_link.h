// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The stub entry's own listener, for a build without the debug link (docs/spec/stub-entry.md 2): the commands the
// flashing and web tools need, on the debug link's framing (dl_frame.h) and with its bytes, so the same tools work with
// either listener. PING echoes, VERSION says what the radio and its build are, STUB loads and enters the RAM monitor,
// CODEPLUG is the fast codeplug protocol (cps-fast.md); any other command answers DL_ST_UNKNOWN_CMD. No RTOS or board
// here: the host tests run it.
#ifndef STUB_LINK_H
#define STUB_LINK_H

#include <stddef.h>
#include <stdint.h>
#include "debuglink/dl_frame.h"

// Reply payload bytes the listener's buffer holds: the debug link's, for the VERSION text with its feature lists and a
// fast codeplug protocol block
#define STUB_LINK_REPLY_MAX	DL_REPLY_MAX

typedef struct {
	const char *board;	// VERSION board= (board.name)
	const char *git;	// VERSION git=
	uint32_t git_time;	// VERSION git_time= (Unix seconds); 0: not sent
	const char *release;	// VERSION release=; NULL or empty: not sent
	const char *features;	// VERSION features= (build_info.h); NULL: not sent
	const char *preset;	// VERSION preset= and off= (build_info.h); either NULL: neither sent
	const char *off;
	const char *(*codec)(void);	// VERSION codec= (hal_vocoder_id), asked at each VERSION; may be NULL
	// The STUB command (stub_entry_request): reply payload in out, its length returned
	size_t (*stub)(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap);
	// The CODEPLUG command (cps_fast_cmd); NULL: unavailable
	size_t (*codeplug)(const uint8_t *req, uint16_t len, uint8_t *out, size_t cap);
} stub_link_t;

// One request's reply payload (out[0] = status), at most cap bytes; returns its length (>= 1)
size_t stub_link_handle(const stub_link_t *l, uint8_t cmd, const uint8_t *req, uint16_t len, uint8_t *out, size_t cap);

#endif
