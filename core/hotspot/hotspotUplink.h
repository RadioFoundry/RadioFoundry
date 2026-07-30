// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
/*
 * Hotspot uplink: what the simplex MMDVM hotspot hears on RF, as MMDVMHost wants it (RF -> host)
 */

#ifndef ODR_HOTSPOTUPLINK_H
#define ODR_HOTSPOTUPLINK_H

#include <stdbool.h>
#include <stdint.h>

#define HOTSPOT_UPLINK_LOST_MS			500U	// a call without a burst for this long is lost (8 missed bursts)
#define HOTSPOT_UPLINK_DATA_GAP_MS		400U	// data blocks this long after the last data burst aren't taken
#define HOTSPOT_UPLINK_BURST_MS			60U		// one voice burst per superframe slot pair
#define HOTSPOT_UPLINK_QUEUE_SIZE		8		// bursts between the Sys interrupt and the hotspot task (480 ms)
#define HOTSPOT_UPLINK_LC_LENGTH		9
#define HOTSPOT_UPLINK_AMBE_LENGTH		27

// What the driver heard (hotspotUplinkBurst_t.kind)
#define HOTSPOT_UPLINK_VOICE			0
#define HOTSPOT_UPLINK_HEADER			1		// voice LC header, its LC checked (RS(12,9))
#define HOTSPOT_UPLINK_TERMINATOR		2		// terminator with LC, its LC checked

// hotspotUplinkBurst_t.flags
#define HOTSPOT_UPLINK_FLAG_LC_VALID	0x01	// lc[] holds the call's LC (header, terminator, or the voice's embedded LC)
#define HOTSPOT_UPLINK_FLAG_SYNC_OK		0x02	// voice: the HR-C6000 checked its EMB (needed to open a call)

typedef struct
{
	uint8_t  kind;								// HOTSPOT_UPLINK_xxx
	uint8_t  seq;								// voice: burst A-F, 0-5
	uint8_t  flags;
	uint8_t  lc[HOTSPOT_UPLINK_LC_LENGTH];		// FLCO (with PF), FID, service options, destination, source
	uint8_t  ambe[HOTSPOT_UPLINK_AMBE_LENGTH];	// voice: the burst's 216 payload bits, as the HR-C6000 gives them
	uint32_t ms;								// when it was heard
} hotspotUplinkBurst_t;

// The queue (ISR -> task)
void hotspotUplinkRxReset(void);
bool hotspotUplinkRxPush(const hotspotUplinkBurst_t *b);	// false if full (the burst is dropped)
bool hotspotUplinkRxPop(hotspotUplinkBurst_t *b);
uint32_t hotspotUplinkRxOverflows(void);

// What to send up for a burst (hotspotUplinkBurst())
#define HOTSPOT_UP_LOST					0x01	// DMR_LOST first: the call that was up is over
#define HOTSPOT_UP_HEADER				0x02	// a voice LC header from the call's LC (hotspotUplinkLc())
#define HOTSPOT_UP_FILL					0x04	// *fillFirst..seq-1: silence bursts, before the voice
#define HOTSPOT_UP_VOICE				0x08	// the voice burst
#define HOTSPOT_UP_TERMINATOR			0x10	// the terminator, from the call's LC

typedef struct
{
	bool     open;			// a call is being heard
	bool     headerSent;	// MMDVMHost has its header (it's in its RF audio state)
	uint8_t  lastSeq;		// the last voice burst sent (or 5 after a header: A is next)
	uint32_t lastMs;		// the last burst heard in the call
	uint8_t  lc[HOTSPOT_UPLINK_LC_LENGTH];
	uint32_t dataLastMs;	// the last data burst (header, CSBK, MBC, block) heard
	bool     dataOpen;
} hotspotUplink_t;

void    hotspotUplinkReset(hotspotUplink_t *u);
uint8_t hotspotUplinkBurst(hotspotUplink_t *u, const hotspotUplinkBurst_t *b, uint8_t *fillFirst);
bool    hotspotUplinkLost(hotspotUplink_t *u, uint32_t nowMs);	// true once: send DMR_LOST
bool    hotspotUplinkData(hotspotUplink_t *u, uint8_t dataType, uint32_t nowMs);	// true: send the data burst up

static inline const uint8_t *hotspotUplinkLc(const hotspotUplink_t *u)
{
	return u->lc;
}

static inline bool hotspotUplinkCallOpen(const hotspotUplink_t *u)
{
	return u->open && u->headerSent;
}

// The burst A-F (0-5) of a voice interrupt, from reg 0x51's data type: 1-6 in its low 3 bits (bit 3 is set at times
// for real bursts: on air, strict 1-6 took no voice at all). A second interrupt within HOTSPOT_UPLINK_VOICE_REPEAT_MS of
// the last voice burst is the EMB parsing one for the same burst: -1, as for anything not 1-6. *lastVoiceMs is updated
#define HOTSPOT_UPLINK_VOICE_REPEAT_MS	45U
int hotspotUplinkVoiceSeq(uint8_t dataType, uint32_t nowMs, uint32_t *lastVoiceMs);

// A voice LC that can start a call: group or private voice, with a source and a destination
bool hotspotUplinkLcIsVoice(const uint8_t *lc);

// The silence burst (three AMBE+2 silence frames, 216 bits, as the HR-C6000 gives a burst)
extern const uint8_t HOTSPOT_UPLINK_SILENCE[HOTSPOT_UPLINK_AMBE_LENGTH];

#endif
