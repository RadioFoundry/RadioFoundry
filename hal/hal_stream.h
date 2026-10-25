// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A byte stream to a host computer: a USB CDC-ACM port, a UART (programming cable) or, later, a UDP socket on the
// network adapter. The core's host link (core/hostlink) owns it and dispatches the bytes to the MMDVM hotspot or the
// CPS protocol. All calls are from one task, the host link's.
#ifndef HAL_STREAM_H
#define HAL_STREAM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
	// Bring the port up (USB: start the device stack and enumerate). Called once, after the settings are loaded
	bool (*start)(void);
	// A host has the port open (USB: configured and DTR set; a UART: always true)
	bool (*connected)(void);
	// Up to max bytes received, without blocking; 0 if none
	size_t (*read)(uint8_t *buf, size_t max);
	// Wait up to timeout_ms for received bytes; true if there are some
	bool (*wait_rx)(uint32_t timeout_ms);
	// Queue len bytes and send them; blocks briefly while the transmit buffer is full. Returns the bytes taken
	// (fewer than len when the host stopped reading or went away)
	size_t (*write)(const uint8_t *buf, size_t len);
} hal_stream_t;

#endif
