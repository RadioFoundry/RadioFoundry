// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
/*
 * DMR single frequency repeater (SFR): the HR-C6000 glue for functions/sfr.h (every MCU)
 */

#ifndef ODR_HRC_SFR_H
#define ODR_HRC_SFR_H

#if defined(HAS_SFR)

#include <stdbool.h>
#include <stdint.h>
#include "dmr/sfr.h"
#include "functions/codeplug.h"

// The repeater's power when the channel's is the radio's default (Master): low, the PA works half the time
#define SFR_REPEATER_DEFAULT_POWER_LEVEL	4 // 1 W

// The user roles: an RX call is over after this many slots without a burst (instead of 13), so the receiver moves
// to the other copy (direct or repeated) soon after it loses one
#define SFR_USER_END_TICK_TIMEOUT			5

// Task context
void hrcSfrChannelChanged(CodeplugChannel_t *channel, uint32_t rxFreq, uint32_t txFreq);// from trxSetFrequency()
void hrcSfrTick(void);// from the driver's task

// The channel's role (SFR_ROLE_OFF when it isn't an SFR channel or the radio isn't on one)
uint8_t hrcSfrRole(void);
bool    hrcSfrIsKeyed(void);// the repeater is sending (its transmission, from the first burst to the end of the hang)
bool    hrcSfrRepeaterActive(void);// repeater role or hotspot SFR: the receiver must stay on

// The driver's ISRs
bool hrcSfrTimeslotStart(void);// first thing in the timeslot ISR: true if it ended one of our bursts
void hrcSfrTimeslot(int stateAtEntry, bool localTx);// after the driver's state machine
bool hrcSfrTxInterrupt(void);// true if the burst starting is ours
// Radios with the HR-C6000's RF RX interrupt routed: true while the repeater is keyed (the driver leaves the RF
// alone); the burst on for at least BURST_MIN_MS ends here (else at the next timeslot interrupt, as without it)
bool hrcSfrRxInterrupt(void);
void hrcSfrRxBurst(int syncClass, int dataType, int syncType, bool crcValid, uint8_t colorCode);
void hrcSfrRxLc(const uint8_t *lc);
// The user roles
bool hrcSfrUserVoiceAccept(int seq);// seq: 0-5 (A-F)
void hrcSfrUserRxColorCode(uint8_t rxCC, bool idle);
void hrcSfrUserTxColorCode(void);

// MMDVM hotspot (task context)
void    hrcSfrHotspotSet(bool on, uint8_t colorCode);
bool    hrcSfrHotspotActive(void);
void    hrcSfrHotspotConfig(uint8_t colorCode, sfrConfig_t *cfg);// what the hotspot SFR runs (or would run) with
uint8_t hrcSfrHotspotNetBurst(const sfrBurst_t *burst, const uint8_t *lc);// 0 or the NAK reason
void    hrcSfrHotspotNetAbort(void);
bool    hrcSfrRfBusy(void);
uint8_t hrcSfrSpace(void);
// The hotspot's uplink: what was heard on the inbound slot, burst by burst (the ISR queues them)
bool           hrcSfrUplinkPop(sfrBurst_t *burst);
const uint8_t *hrcSfrRfLc(void);// the inbound call's LC (9 bytes), all 0 until known

// On-air debugging (hotspot SFR, docs/sfr.md): counters for GET_STATUS, and experiment switches (MMDVM command 0xD5:
// flags, framing, reg 0x10, reg 0x12)
#define HRC_SFR_DEBUG_PHASE_FLIP		0x01	// send in the other slot than the DMR_STATE_RX_1 one
#define HRC_SFR_DEBUG_SYNC_PER_BURST	0x02	// the outbound sync only from the outbound slot's set up until its burst starts
#define HRC_SFR_DEBUG_POLITE			0x04	// leave the HR-C6000's channel access policy (polite to CC) as the driver set it
#define HRC_SFR_DEBUG_ACTIVE_TIMING		0x08	// the HR-C6000's own slot timing (0x40 = 0xE3) while it sends
#define HRC_SFR_DEBUG_NO_I2C_CHECK		0x10	// switch the RF even when the task holds the I2C bus
#define HRC_SFR_DEBUG_BEACON			0x20	// TX test, for as long as it's set: a group call from 9999 to TG 9 (2 headers, 4 superframes of
										// silence, a terminator) over and over on the outbound slot, nothing heard needed
#define HRC_SFR_DEBUG_TX_OFFSET			0x40	// add the driver's +3 kHz TX offset (on-air test 5: the repeater's bursts were +2.93 kHz off
										// with it; without it the TX frequency is the RX one, no AT1846S retune per burst)
#define HRC_SFR_DEBUG_PA_NEXT_SLOT		0x80	// the PA on at the timeslot interrupt after the RF TX interrupt, off at the next one

typedef struct
{
	uint8_t txInterrupts;		// 13: RF TX interrupts for our bursts (mod 256)
	uint8_t burstsWritten;		// 14: bursts and idle bursts set up (mod 256)
	uint8_t rxBursts;			// 15: bursts the Sys interrupt passed to the SFR (mod 256)
	uint8_t rxRepeated;			// 16: of those, queued for the repeat (mod 256)
	uint8_t sendRejected;		// 17: the HR-C6000's "send request rejected" interrupts (channel busy) (mod 256)
	uint8_t switchSkipped;		// 18: RF switches skipped, I2C busy (mod 256)
	uint8_t sysAfterTsMs;		// 19: last burst heard: ms from the timeslot interrupt before its Sys interrupt
	uint8_t txAfterTsMs;		// 20: last burst of ours: ms from the timeslot interrupt before its RF TX interrupt
	uint8_t burstOnMs;			// 21: last burst of ours: ms its PA was on
	uint8_t tsPeriodMs;			// 22: ms between the last two timeslot interrupts
	uint8_t reg42Rx;			// 23: reg 0x42 (bits 7-5 101 TX, 011 RX, 001 closed) in the slot before our burst's
	uint8_t state;				// 24: 0x01 keyed, 0x02 own timing, 0x04 outbound phase, 0x08 burst on, 0x10 RF call, 0x20 busy
	uint8_t flags;				// 25: HRC_SFR_DEBUG_xxx
	uint8_t uplinkOverflow;		// 26: (mod 256)
	uint8_t reg42Tx;			// 27: reg 0x42 in our burst's slot (expect 101xxxxx when the HR-C6000 sends it)
	uint8_t rxEcho;				// 28: bursts heard in or right after a slot we sent in, dropped (mod 256)
	uint8_t rxIgnored;			// 29: bursts not for the repeater (color code, sync, idle) (mod 256)
	uint8_t rxDuplicate;		// 30: voice bursts heard twice (mod 256)
	uint8_t lastRx;				// 31: last burst heard: sync type (bits 7-6, 0 MS 1 BS 2 TS1 3 TS2), sync class (5-4), data type (3-0)
	uint8_t lastRxCC;			// 32: its color code (low nibble), 0x80 CRC good
	uint8_t txSwitch10us;		// 33: last RX to TX switch of the AT1846S, 10 us units
	uint8_t rxSwitch10us;		// 34: last TX to RX switch, 10 us units
	uint8_t reg10;				// 35: reg 0x10 used while sending (0: the driver's, kept)
	uint8_t reg12;				// 36: reg 0x12 (RF TX advance, 100 us units) as set, 0xFF: the driver's
	uint8_t rxIgnoredCC;		// 37: of 29, other color code (mod 256)
	uint8_t rxIgnoredSync;		// 38: of 29, BS sync or the outbound slot's (mod 256)
	uint8_t rxIgnoredNoCrc;		// 39: of 29, CRC bad (mod 256)
	uint8_t rejectedType;		// 40: the last burst dropped as CRC bad: data type (0x80: while keyed)
	uint8_t rejected[12];		// 41-52: its info bits, as read from the HR-C6000
	uint8_t rxWeak;				// 53: bursts reported while we send in an inbound slot without a signal (RSSI), dropped (mod 256)
	uint8_t lastRssi;			// 54: the last inbound slot's RSSI (AT1846S reg 0x1B high byte), read at our RF TX interrupt
	uint8_t rssiMin;			// 55: the RSSI an inbound slot needs while we send
	uint8_t rereadDiffers;		// 56: bursts heard while we send whose status read again at the next timeslot differed (mod 256)
	uint8_t pendingWas;			// 57: the last of those: sync class (bits 5-4) and data type (3-0) at the Sys interrupt
	uint8_t pendingIs;			// 58: and at the timeslot interrupt
	uint8_t rawKinds;			// 59: the last burst heard while sending, sfrRawKind_t of raw area 0 (bits 3-0) and 1 (7-4)
	int8_t rawArea;				// 60: the raw area the inbound slot was found in (-1: none yet)
	uint8_t rawData;			// 61: data bursts decoded from the raw bits (mod 256)
	uint8_t rawVoiceA;			// 62: voice A (mod 256)
	uint8_t rawEmb;				// 63: voice B-F (mod 256)
	uint8_t rawNone;			// 64: neither (noise, not ours), dropped (mod 256)
	uint8_t rxEchoContent;		// 65: voice bursts "heard" that were ours (same AMBE and position, sent < 250 ms before) (mod 256)
	uint8_t raw[33];			// 66-98: the last raw burst nothing was found in (area 0, or the known one)
	uint8_t rxLcRebuilt;		// 99: headers / terminators heard while keyed, their LC taken from the call's (mod 256)
	uint8_t rxNoCall;			// 100: voice bursts heard while keyed outside an inbound call, dropped (mod 256)
	uint8_t spiFail;			// 101: SPI transfers of the SFR refused (the bus in use: SPI0inUse) (mod 256)
	uint8_t reg56;				// 102: reg 0x56 at the last Sys interrupt while keyed (undocumented; per slot RX status?)
	uint8_t reg57;				// 103: reg 0x57 then (the official firmware's timecode register, per the driver)
	uint8_t outAtReject;		// 104: what we sent in the slot the last bad header / terminator was decoded in: 1 voice,
								//      2 header / terminator / data, 3 idle, 0 nothing
	uint8_t outAtRebuild;		// 105: the same for the last one rebuilt from the call's LC
	uint8_t reg11;				// 106: reg 0x11 while sending (0: the driver's 0x80, left)
	uint8_t flags2;				// 107: HRC_SFR_DEBUG2_xxx
	uint8_t headerAsVoiceA;		// 108: voice A claims in a call's header phase that were headers (reg 0x51 data after) (mod 256)
	uint8_t voiceAWas;			// 109: the last voice A claim in a header phase: reg 0x51 class (bits 5-4), type (3-0) then
	uint8_t voiceAIs;			// 110: and reg 0x51 at the next timeslot interrupt
	uint8_t rxSameSlot;			// 111: second bursts reported for one inbound slot (< 45 ms after the one taken), dropped (mod 256)
	uint8_t headerFilled;		// 112: headers repeated with the call's LC for an unusable report in its header phase (mod 256)
	uint8_t staleVoice;			// 113: voice A right after F with F's AMBE (stale voice data), dropped (mod 256)
	uint8_t resyncs;			// 114: bursts heard out of step while we sent (a call with its own slot timing): transmission ended,
								//      receiver resynchronized (mod 256)
	uint8_t outOfStepMs;		// 115: the last burst heard out of step: ms from the timeslot interrupt before its Sys interrupt
	uint8_t relocks;			// 116: bursts heard out of step while not sending (the HR-C6000's timing off the call's, from
								//      noise): not repeated, receiver resynchronized (mod 256)
	uint8_t lateEntryHeld;		// 117: voice A heard outside a call when idle, waiting for its B to start a call (mod 256)
	uint8_t dataHeld;			// 118: data calls (CSBK, data) held until they were over, then repeated (mod 256)
	uint8_t hangCut;			// 119: hang times ended for a data burst heard in them (the rest of the call heard unkeyed) (mod 256)
	uint8_t rxDataEcho;			// 120: data bursts heard while keyed that were ours (same 12 bytes, sent < 250 ms before) (mod 256)
} hrcSfrDebug_t;

const hrcSfrDebug_t *hrcSfrDebug(void);
// framing 0: the settings'; reg10 0, reg12 0xFF: the driver's; rssiMin 0: HRC_SFR_RSSI_MIN_DEFAULT
void hrcSfrDebugSet(uint8_t flags, uint8_t framing, uint8_t reg10, uint8_t reg12, uint8_t rssiMin, uint8_t reg11, uint8_t flags2);
#define HRC_SFR_DEBUG2_CC_ANY			0x01	// 0x41 bit 3 (CC_Match_Ctrl: deframe whatever the color code) while keyed
#define HRC_SFR_DEBUG2_NO_CC_WRITES		0x02	// leave reg 0x1F (the color code, RX and TX) alone while keyed
#define HRC_SFR_DEBUG2_IDLE_IN_CALL		0x04	// idle bursts in the gaps of an inbound call too (the default leaves them empty)
#define HRC_SFR_DEBUG2_IDLE_MCU_CRC		0x08	// idle bursts with reg 0x40 bit 3 set, as before (the default now clears it)
#define HRC_SFR_DEBUG2_DEFER_READ		0x10	// a burst heard while keyed read at the next timeslot interrupt (as 198349d-1da3197)
#define HRC_SFR_DEBUG2_VOICE_A_IN_HEADERS_IS_HEADER 0x20 // a voice A in a call's header phase (keyed) is always taken as a header
#define HRC_SFR_DEBUG2_NO_DATA_HOLD		0x40	// data calls repeated as they're heard, 90 ms later (as up to 800b57e): no hold, no hang cut
#define HRC_SFR_RSSI_MIN_DEFAULT		0x00	// 0: off (it read noise during a real call, on air: the sampling time is wrong)
void hrcSfrSendRejected(void);// the driver's Sys ISR: reg 0x82 bit 7

// Provided by the radio: the RF switched between the outbound burst (tx, at the given PA DAC drive) and the
// receiver, on the channel's frequency, fast enough to be done between two slots. Called from the ISRs. False if it
// didn't (the task holds the I2C bus, unless force)
bool radioSfrSwitch(bool tx, uint16_t paDac, bool force, bool txOffset);
// Provided by the radio: the receiver's RSSI now, from the ISRs (false if the I2C bus is busy)
bool radioSfrReadRssi(uint8_t *rssi);
// Provided by the radio: the PA drive (radioSfrSwitch()'s paDac) for a power level on the channel's TX frequency
uint16_t radioSfrPaDrive(uint8_t powerLevel);

#endif

#endif
