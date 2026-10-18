// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
// Key frames from a hand mic with its own MCU that shares a UART receive line with the PC link (the DB25-D's: "K" and
// the key number as two ASCII digits, repeated every few milliseconds while the key is held; hardware-db25d.md 3.4).
// Independent of the board: the UART's receive interrupt passes every byte in with a microsecond time stamp, and the
// key scan asks which key is held.
//
// The PC's traffic arrives on the same line, so a frame only counts as a key when it can't be host data:
// - exactly three bytes with a quiet gap of SERIALMIC_GAP_US before and after (host packets are longer and start with
//   the sync A5 5A, but a USB-serial adapter can pause anywhere inside one);
// - the same frame twice, less than SERIALMIC_REPEAT_US apart, as the mic repeats it while the key is held;
// - no host sync (A5 5A) for SERIALMIC_HOST_QUIET_US.
// A key counts as held until no frame for it has arrived for SERIALMIC_HELD_US.
#ifndef SERIALMIC_H
#define SERIALMIC_H

#include <stdint.h>
#include <stdbool.h>

#define SERIALMIC_GAP_US	500u
#define SERIALMIC_REPEAT_US	10000u
#define SERIALMIC_HELD_US	40000u
#define SERIALMIC_HOST_QUIET_US	1000000u
#define SERIALMIC_NONE		0xFFu

typedef struct {
	uint8_t frame[3];
	uint8_t len;
	bool error;
	uint8_t prev;
	bool host_seen;
	uint8_t cand;		// a valid frame seen once, waiting for its repeat
	uint8_t held;
	uint32_t last_us, host_us, cand_us, held_us;
} serialmic_t;

void serialmic_init(serialmic_t *m);
// One received byte (error: the UART flagged it)
void serialmic_byte(serialmic_t *m, uint8_t b, bool error, uint32_t now_us);
// The key number held now, or SERIALMIC_NONE. Call it regularly: it also ends the last frame once its gap has passed
uint8_t serialmic_key(serialmic_t *m, uint32_t now_us);

#endif
