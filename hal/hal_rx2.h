// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A second receiver: radios with a receiver per dual watch channel (a second SoC). The radio core keeps it on the
// dual watch channel that isn't selected, so both channels are heard at once instead of in turn; the main receiver
// (hal_radio_t) stays on the selected one, which is also the one PTT transmits on. Without it (board.rx2 NULL, or
// tune() refused) dual watch alternates the main receiver as on every other radio (docs/spec/radio-features.md 2.1).
#ifndef HAL_RX2_H
#define HAL_RX2_H

#include <stdint.h>
#include <stdbool.h>
#include "hal_radio.h"

#define RX2_GROUPS	16u	// DMR groups a channel listens to, beyond its talk group

// What to receive. DMR: the second receiver decodes the calls itself and plays the ones for us (a group in groups[],
// the talk group, or a private call to own_id); the core only shows them
typedef struct {
	uint32_t rx_hz;
	uint8_t mode;		// radio_mode_t
	uint8_t bw;		// radio_bw_t
	uint8_t squelch;	// 0 = open .. 9 = tight, as the main receiver's
	subtone_t rx_tone;	// FM
	uint8_t color_code;	// DMR
	uint8_t timeslot;	// DMR: 1 or 2
	uint8_t group_count;	// DMR: entries used in groups[]
	uint32_t own_id;	// DMR: our ID (private calls to it are ours)
	uint32_t talk_group;	// DMR: the channel's talk group, 0 = none (a private contact)
	uint32_t groups[RX2_GROUPS];	// DMR: the channel's listen list
} rx2_tune_t;

typedef struct {
	bool up;		// it answers (the link to it is alive)
	bool tuned;		// it is on the last tune()'s settings
	bool active;		// a signal (FM: squelch open with the right tone) or a call for us (DMR)
	bool dmr;		// active is a DMR call
	bool group;		// ... to a group (else private)
	bool heard;		// its audio goes to the speaker now (audio(true) and active)
	int16_t rssi_dbm;
	uint8_t noise;
	uint32_t src, dst;	// DMR: the call's source and destination IDs
} rx2_status_t;

typedef struct {
	// Receive with these settings. False: it can't now (not up, out of band); the core falls back to alternating
	bool (*tune)(const rx2_tune_t *t);
	// Stop receiving (dual watch ended); its audio off
	void (*stop)(void);
	// Its state now, from the last report (any task; never waits)
	void (*status)(rx2_status_t *st);
	// Its received audio to the speaker (true) or muted. The core gives it the speaker while the main receiver is
	// quiet: the selected channel's calls always win
	void (*audio)(bool on);
	// Optional, NULL where there is no route: the second SoC's update path (docs/spec/rx2-link.md 4). Send it RX2_STUB,
	// so it restarts into its first stage's long window, then copy bytes between the PC cable's UART and the link both
	// ways, with the transmitter off and the watchdog fed. It ends 10 s after the host's last byte (90 s if none came;
	// bytes from the second SoC don't count). Never returns: the radio restarts when the bridge ends
	void (*bridge)(void);
	// Optional: restart the second SoC (RX2_RESET)
	void (*restart)(void);
	// Optional: one debug link command to the second SoC's own server (docs/spec/rx2-link.md 2.3): req is the command
	// and its payload. Waits for the whole reply, up to about a second. Returns the reply payload's length in out (its
	// status first), 0 when it is down or did not answer. One caller at a time (the debug link's task)
	size_t (*forward)(const uint8_t *req, uint8_t len, uint8_t *out, size_t cap);
} hal_rx2_t;

#endif
