// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// An air interface provider on a board's sample taps (docs/spec/air-interface.md 6): for a board whose only way into
// the air interface is the discriminator its MCU samples (hal_baseband_t receive), and the samples it feeds its
// modulator if it can (hal_baseband_t transmit), with no modem chip of its own to run a session on. The soft modem
// (air_softmodem.h) turns the samples into symbols and back; this file is the hal_air_t around it: fit, open and
// close, a restart, the transmit queue, the counters, and the poll that moves samples through.
//
// The board sets it up once (air_tap_setup) and calls air_tap_poll from a task of its own, often enough that the tap
// never fills or runs dry (hal_baseband.h: 60 ms or more): the session's events come from inside the poll, so the
// framer and the mode's hooks run on that task's stack. No RTOS call here: the board's lock hooks (optional, and
// recursive: a mode's hooks queue from inside a poll) keep another task's call from running while a poll is inside
// the soft modem.
//
// Half duplex: a transmission stops the receive tap and starts it again after (fit never says AIR_FIT_FULL_DUPLEX), so
// the receiver's and the transmitter's state may share memory. Without a transmit tap (or tx NULL) fit never says
// AIR_FIT_TX and the transmit calls refuse. Rule 1 (hal_air.h) is the board's: its tap's rx_start fails while raw
// receive could reach the speaker; rule 2 is too: nothing but tx_write reaches its modulator while a tap transmits.
#ifndef RFOUNDRY_AIR_TAP_H
#define RFOUNDRY_AIR_TAP_H

#include <stdbool.h>
#include <stdint.h>

#include "../../hal/hal_air.h"
#include "../../hal/hal_baseband.h"
#include "air_softmodem.h"

typedef struct {
	const hal_baseband_t *bb;	// the taps
	air_softmodem_t *sm;		// the soft modem's state, the board's memory (used only while a session is open)
	air_softmodem_tx_t *tx;		// the transmitter's, NULL: receive only. May be the same memory as sm
	uint32_t fit_flags;		// added to every fit (AIR_FIT_UNTESTED until the tap is checked on air)
	void (*lock)(void);		// optional, both or neither: around a poll's work and around open and close
	void (*unlock)(void);
} air_tap_cfg_t;

extern const hal_air_t air_tap;

// Before the first fit or open; cfg stays valid
void air_tap_setup(const air_tap_cfg_t *cfg);
// The board's task: moves what the tap read through the soft modem to the session (AIR_EV_RX events), and while
// transmitting the queued symbols to the transmit tap (the AIR_EV_TX events). Returns whether a session is open (the
// task may sleep longer when none is)
bool air_tap_poll(void);
// Whether a session is open, and whether it transmits now (from tx_begin to its AIR_EV_TX_DONE): a board keys its
// transmitter for the tap's path while a session is open
bool air_tap_is_open(void);
bool air_tap_transmitting(void);

#endif
