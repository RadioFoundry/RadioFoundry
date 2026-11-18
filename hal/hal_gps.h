// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// GPS receiver: a module whose NMEA output the board listens to (docs/spec/position.md 2). Every sentence goes to the
// position service (core/position); this is the rest the core needs from the board.
//
// Radios sold with and without a module share a board, so the module is powered and listened to at start-up whatever
// Settings > Location > GPS says, until it has been heard or the board gives up. Once heard, its supply and receive
// path follow the setting (enable). A board whose module shares a supply it can't cut stops the receive path only.
#ifndef HAL_GPS_H
#define HAL_GPS_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
	// Optional: the module has been heard. NULL = CAP_GPS alone says (the module is always fitted)
	bool (*present)(void);
	// Settings > Location > GPS: power the module and its receive path on or off, within one poll. Any task; called
	// with the stored value at start-up and on each change. NULL = the module is always on
	void (*enable)(bool on);
	// Optional: the module is powered and listened to now (the start-up probe, then the setting)
	bool (*running)(void);
	// Optional, diagnostics: valid sentences, bad ones (checksum, length, UART errors), bytes received
	void (*counts)(uint32_t *good, uint32_t *bad, uint32_t *bytes);
	// Optional, diagnostics: the rate listened at, 0 once given up
	uint32_t (*baud)(void);
} hal_gps_t;

#endif
