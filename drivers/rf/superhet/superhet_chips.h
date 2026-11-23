// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A superhet's two RF chips, whichever parts a radio has: register 0 of each read at start-up, the part set up and
// bound to its ops (superhet.h). One image serves every build of a radio:
//
//   main transceiver: BK4819 (0x4819), FD6818 (0x6818) or the 0x7813 part, driven as an FD6818
//   LO:               BK4819, FD6818, the 0x7813 part (its own LO set) or an AT1141 PLL (0x1100)
//
// The board gives each chip's register bus and its wiring (which GPIO does what, the FD6818's sub-audio line) and keeps
// the radio's sequences; this file holds only what every radio with these chips does the same.
#ifndef SUPERHET_CHIPS_H
#define SUPERHET_CHIPS_H

#include <stdint.h>
#include <stdbool.h>
#include "rf/rfbus/rfbus.h"
#include "rf/superhet/superhet.h"
#include "rf/bk4819/bk4819.h"
#include "rf/fd6818/fd6818sh.h"
#include "rf/at1141/at1141.h"

typedef enum {
	SUPERHET_CHIP_NONE,
	SUPERHET_CHIP_BK4819,
	SUPERHET_CHIP_FD6818,
	SUPERHET_CHIP_7813,
	SUPERHET_CHIP_AT1141,
} superhet_chip_t;

typedef struct {
	rfbus_t *bus_main, *bus_lo;
	// FD6818 main: the chip's GPIO0, the sub-audio compare result (FD6818SH_GPIO_CSS), as the board reads it. NULL: no
	// line, a sub-audio channel never opens
	bool (*css_line)(void);
	// AT1141 LO: true = off for a transmission and restarted after (at1141_superhet_lo_restart); false = disabled at
	// the end of its init and only gated (at1141_superhet_lo)
	bool at1141_restart;
	const fd6818sh_cfg_t *fd_cfg;	// the board's FD6818 main set (fd6818sh_set_cfg); NULL = the driver's
} superhet_chips_cfg_t;

typedef struct {
	const superhet_chips_cfg_t *cfg;
	superhet_chip_t main_kind, lo_kind;
	uint16_t main_id, lo_id;	// register 0 as read, also for a part not driven
	union {
		bk4819_t bk;
		fd6818sh_t fd;
	} m;
	union {
		bk4819_t bk;
		fd6818sh_t fd;
		at1141_t at;
	} lo_dev;
	superhet_main_t main;		// bound by superhet_chips_init; ops NULL until then
	superhet_lo_t lo;
} superhet_chips_t;

// The part an ID names in a role (lo: the LO chip), SUPERHET_CHIP_NONE for one not driven there
superhet_chip_t superhet_chip_kind(uint16_t id, bool lo);
// Register 0 until it names a part (kind), with a soft reset between tries. The ID last read
uint16_t superhet_chip_identify(rfbus_t *bus, bool lo, superhet_chip_t *kind);
// Both chips identified and, when both are known, set up in the order these radios need: the main chip's init, the
// LO's, then the main chip's analog mode; both end idle. False: a part not driven (nothing but the reset is written)
bool superhet_chips_init(superhet_chips_t *c, const superhet_chips_cfg_t *cfg);
// "BK4819", "FD6818", "7813", "AT1141", "none"; and the one-letter code ('B', '6', '4', 'T', '?')
const char *superhet_chip_name(superhet_chip_t k);
char superhet_chip_letter(superhet_chip_t k);

// An LO above the signal turns the IF spectrum over: DCS arrives inverted
void superhet_chips_rx_inverted(superhet_chips_t *c, bool inverted);
// The main chip's status: RSSI (0x67), noise (0x65), sub-audio flags (0x0C). The same registers on every main part
void superhet_chips_status(superhet_chips_t *c, uint16_t *r67, uint16_t *r65, uint16_t *r0c);
// The sub-audio set by rx_tone heard (always for none): r0c on a BK4819, the board's line on an FD6818
bool superhet_chips_tone_heard(const superhet_chips_t *c, uint16_t r0c, superhet_tone_t type);
// The main chip's GPIO output word as last written (BK4819 0x33, FD6818 0x25), for a board's diagnostics
uint16_t superhet_chips_main_gpio_word(const superhet_chips_t *c);

#endif
