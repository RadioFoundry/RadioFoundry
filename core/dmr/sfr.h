// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
/*
 * DMR single frequency repeater (SFR): the hardware independent parts
 */

#ifndef ODR_SFR_H
#define ODR_SFR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Roles, as stored per channel (sfrChannelSettings_t.role)
typedef enum
{
	SFR_ROLE_OFF = 0,
	SFR_ROLE_USER_STANDARD,		// a radio using a standard SFR: direct mode, both copies heard, replies on the inbound slot
	SFR_ROLE_USER_ERDM,			// a radio using a Motorola ERDM repeater: as above, and the outbound colour code heard too
	SFR_ROLE_RPT_STANDARD,		// this radio is a standard SFR
	SFR_ROLE_RPT_MOTOROLA,		// this radio is an ERDM style SFR
	SFR_ROLE_NUM_STORED,
	SFR_ROLE_HOTSPOT = SFR_ROLE_NUM_STORED // not stored: the MMDVM hotspot's SFR mode (the host chose it)
} sfrRole_t;

// Outbound framing: the sync pattern the outbound slot's bursts carry
typedef enum
{
	SFR_FRAMING_AUTO = 0,		// Motorola: BS, else TS
	SFR_FRAMING_MS,				// MS sourced sync: plain direct mode radios hear it, the slot isn't in the burst
	SFR_FRAMING_TS,				// ETSI direct mode TS1 / TS2 sync of the outbound slot: the slot is in the burst
	SFR_FRAMING_BS,				// BS sourced sync (ERDM, most likely), guard time framing: no CACH unless built with
								// HRC_SFR_BS_CACH (HR-C6000 relay mode, experimental)
	SFR_FRAMING_NUM
} sfrFraming_t;

// Sync types, in the HR-C6000's order (reg 0x5F bits 1-0)
typedef enum
{
	SFR_SYNC_MS = 0,
	SFR_SYNC_BS,
	SFR_SYNC_TS1,
	SFR_SYNC_TS2
} sfrSync_t;

#define SFR_HANG_NUM				4			// hang time choices, see sfrHangMs()
#define SFR_TOT_DEFAULT_MS			180000U		// repeater TOT when the channel has none
#define SFR_RF_LOSS_MS				360U		// an inbound call without its terminator is over after this long without a burst
#define SFR_NET_LOSS_MS				1000U		// a network stream with nothing for this long is over
#define SFR_DEDUP_RESET_MS			400U		// voice dedup: a gap this long starts over
#define SFR_QUEUE_SIZE				16			// bursts waiting for the outbound slot (repeats and network)
#define SFR_REPEAT_DELAY_MS			90			// the HR-C6000 can't repeat in the next slot: the one after, 90 ms later
#define SFR_DATA_HOLD_GAP_MS		130U		// a held data call (sfrEngine_t.rfHold) is over after this long without a burst

// Channel codeplug encoding (the CPS doesn't know these; set on the radio, Channel details):
//   LibreDMR_flag1 bit 0x02: SFR set for this channel (the other two bytes are only read with it)
//   sfr byte A (was _UNUSED_1): bits 2-0 role (sfrRole_t), bits 4-3 framing (sfrFraming_t), bit 5 inbound slot TS2,
//                               bits 7-6 hang (index of sfrHangMs())
//   sfr byte B (was _UNUSED_2): bit 7 outbound colour code set, bits 3-0 that colour code (else the channel's)
#define SFR_CHANNEL_FLAG1_ENABLED	0x02

typedef struct
{
	uint8_t role;				// sfrRole_t (stored roles only)
	uint8_t framing;			// sfrFraming_t
	uint8_t inboundSlot;		// 0: TS1 (default), 1: TS2
	uint8_t hangIndex;			// 0..SFR_HANG_NUM-1
	bool    outboundCCSet;		// Motorola: the outbound colour code differs from the channel's
	uint8_t outboundCC;
} sfrChannelSettings_t;

// What the radio runs with (sfrConfigBuild)
typedef struct
{
	uint8_t  role;				// sfrRole_t
	uint8_t  framing;			// resolved: MS, TS or BS
	uint8_t  inboundSlot;		// 0: TS1, 1: TS2; the outbound slot is the other one
	uint8_t  inboundCC;			// what the radios send with (the channel's colour code)
	uint8_t  outboundCC;		// what the repeat goes out with
	uint16_t hangMs;			// idle bursts after the last traffic, so the radios stay aligned to the slots
	uint32_t totMs;				// longest inbound call repeated
} sfrConfig_t;

bool        sfrChannelDecode(uint8_t flag1, uint8_t byteA, uint8_t byteB, sfrChannelSettings_t *s);
void        sfrChannelEncode(const sfrChannelSettings_t *s, uint8_t *flag1, uint8_t *byteA, uint8_t *byteB);
void        sfrConfigBuild(const sfrChannelSettings_t *s, uint8_t channelCC, uint8_t channelTot15s, sfrConfig_t *cfg);
uint16_t    sfrHangMs(uint8_t hangIndex);
uint8_t     sfrResolveFraming(uint8_t role, uint8_t framing);
uint8_t     sfrOutboundSync(const sfrConfig_t *cfg);
bool        sfrInboundSyncAccepted(const sfrConfig_t *cfg, uint8_t sync);
bool        sfrRoleIsRepeater(uint8_t role);
bool        sfrRoleIsUser(uint8_t role);
const char *sfrRoleName(uint8_t role);
const char *sfrFramingName(uint8_t framing);

// The user roles' receiver: both slots' copies of a call (direct and repeated) may be heard. A voice burst is played
// only if it's after the last one played (burst A-F, 0-5): a repeat of one already played is dropped
typedef struct
{
	int8_t   lastSeq;			// -1: none yet
	uint32_t lastMs;
} sfrDedup_t;

void sfrDedupReset(sfrDedup_t *d);
bool sfrDedupVoice(sfrDedup_t *d, uint8_t seq, uint32_t nowMs);

// The repeater's receiver: where a burst's report (the HR-C6000's Sys interrupt) falls against its timeslot
// interrupts. In step with the call, it comes a few ms (3-5 on air) after the timeslot interrupt that starts the slot
// after the burst; with the HR-C6000's slot timing off the call's (on-air test 16: locked to noise before the call,
// 22 ms off) it comes later in the slot (26-30 ms on air), and the repeat would go out over the inbound bursts
#define SFR_REPORT_MIN_MS			2
#define SFR_REPORT_MAX_MS			10
#define SFR_REPORT_NO_TIMING_MS		32			// no timeslot interrupt for longer than a slot: no slot timing to be off
typedef enum
{
	SFR_STEP_UNKNOWN = 0,		// no slot timing (or not measured)
	SFR_STEP_IN,
	SFR_STEP_OUT
} sfrStep_t;

uint8_t sfrReportStep(uint32_t afterTimeslotMs);

// A voice call heard without its header (late entry) when idle: a voice A alone (noise, a stray burst) doesn't start
// it; a B in sequence after it (60 ms later) does, from that B
#define SFR_LATE_ENTRY_MIN_MS		50
#define SFR_LATE_ENTRY_MAX_MS		70
typedef struct
{
	bool     armed;
	uint32_t voiceAMs;
} sfrLateEntry_t;

void sfrLateEntryReset(sfrLateEntry_t *l);
bool sfrLateEntryVoice(sfrLateEntry_t *l, uint8_t seq, uint32_t nowMs);// seq 0-5 (A-F): true when the call starts with it

// ERDM user: the colour code the receiver follows for a received one, -1 if it isn't one of the channel's two
int sfrUserRxColourCode(const sfrConfig_t *cfg, uint8_t rxCC);

// The repeat queue

#define SFR_BURST_VOICE				0
#define SFR_BURST_DATA				1

#define SFR_BURST_FLAG_MCU_CRC		0x01		// the payload has its CRC and mask: the HR-C6000 sends it as is
#define SFR_BURST_FLAG_NET			0x02		// from the network (hotspot), not repeated from RF

#define SFR_VOICE_LENGTH			27			// three AMBE frames
#define SFR_LC_LENGTH				9

typedef struct
{
	uint8_t kind;				// SFR_BURST_VOICE or SFR_BURST_DATA
	uint8_t type;				// voice: burst A-F 0-5; data: DT_xxx (voice LC header, terminator, CSBK, data, idle)
	uint8_t length;				// payload bytes: 27 voice, 12 / 18 / 24 data
	uint8_t flags;				// SFR_BURST_FLAG_xxx
	uint8_t payload[SFR_VOICE_LENGTH];
} sfrBurst_t;

typedef enum
{
	SFR_RX_REPEATED = 0,		// queued for the outbound slot
	SFR_RX_IGNORED,				// not for the repeater: other colour code, the outbound / a repeater's sync, idle
	SFR_RX_DUPLICATE,			// a voice burst already queued
	SFR_RX_TOT,					// the inbound call is longer than the TOT: not repeated until it's over
	SFR_RX_FULL
} sfrRxResult_t;

typedef enum
{
	SFR_TX_NONE = 0,			// not transmitting: the outbound slot stays off
	SFR_TX_BURST,				// *out to send
	SFR_TX_IDLE,				// an idle burst (gaps in a call, hang time)
	SFR_TX_END					// the transmission is over (this slot stays off)
} sfrTxAction_t;

// MMDVM: what the host is told
#define SFR_MMDVM_NAK_RF_BUSY		6			// DMR_DATA2 refused: an inbound call is being repeated
#define SFR_MMDVM_NAK_FULL			5
#define SFR_MMDVM_STATUS_INBOUND_BUSY 0x80		// GET_STATUS byte 5 (v1): an inbound call is being repeated

typedef struct
{
	sfrConfig_t cfg;

	sfrBurst_t  queue[SFR_QUEUE_SIZE];
	uint8_t     head;			// next out
	uint8_t     count;

	// The inbound (RF) call
	bool        rfCall;			// heard and not over yet
	bool        rfTot;			// over the TOT: not repeated any more
	bool        rfBusy;			// MMDVM inbound busy: until the call's terminator (or its last burst) has been repeated
	bool        rfTermQueued;	// the call's terminator is queued: network bursts can queue behind it
	uint32_t    rfStartMs;
	uint32_t    rfLastMs;
	uint8_t     rfLc[SFR_LC_LENGTH];// the call's LC: from its header or embedded LC, all 0 until known (cleared per call)
	sfrDedup_t  rfDedup;
	bool        rfVoice;		// the call is a voice call (a voice header, PI header or voice burst taken in it)
	bool        rfHold;			// a data call (CSBK, data header, blocks) being heard: not sent until it's over, so it's
								// heard with the repeater not keyed (its receiver reports bit errors while it sends)
	bool        noDataHold;		// set by the glue: data calls repeated as they're heard, 90 ms later (no rfHold)

	// The network stream (hotspot)
	bool        netStream;		// a stream is coming in
	bool        netDropping;	// an RF call cut into it: the rest of it is refused
	uint32_t    netLastMs;
	uint8_t     netLc[SFR_LC_LENGTH];

	// The outbound slot
	bool        keyed;
	uint32_t    lastTrafficMs;	// last burst sent that wasn't idle
	uint8_t     txLc[SFR_LC_LENGTH];// the LC of the voice burst returned (its embedded LC)
	uint8_t     txLcOrigin;		// SFR_BURST_FLAG_NET or 0, of the LC in txLc
} sfrEngine_t;

void          sfrEngineInit(sfrEngine_t *e, const sfrConfig_t *cfg);
sfrRxResult_t sfrEngineRfBurst(sfrEngine_t *e, const sfrBurst_t *b, uint8_t cc, uint8_t sync, uint32_t nowMs);
void          sfrEngineRfLc(sfrEngine_t *e, const uint8_t *lc);
uint8_t       sfrEngineNetBurst(sfrEngine_t *e, const sfrBurst_t *b, const uint8_t *lc, uint32_t nowMs);
void          sfrEngineNetAbort(sfrEngine_t *e);
void          sfrEngineFlush(sfrEngine_t *e);// drop everything queued, the call state kept
// Keyed only for the hang time (nothing queued, no call, no network stream): unkeyed now, true if it was. A data call
// starting on the inbound slot is heard better with the repeater not sending
bool          sfrEngineEndHang(sfrEngine_t *e);
sfrTxAction_t sfrEngineOutboundSlot(sfrEngine_t *e, uint32_t nowMs, sfrBurst_t *out);
uint8_t       sfrEngineSpace(const sfrEngine_t *e);

static inline bool sfrEngineRfBusy(const sfrEngine_t *e)
{
	return e->rfBusy;
}

static inline bool sfrEngineKeyed(const sfrEngine_t *e)
{
	return e->keyed;
}

// An inbound voice call is on: voice heard outside one (after a data or CSBK call, the HR-C6000's reports in the
// empty slot) starts one only if it's credible (the glue's late entry rule)
static inline bool sfrEngineRfVoiceCall(const sfrEngine_t *e)
{
	return e->rfCall && e->rfVoice;
}

// An LC with a destination and a source (all 0: not known yet)
bool sfrLcKnown(const uint8_t *lc);

// The LC (9 bytes) of the voice burst sfrEngineOutboundSlot() returned, for its embedded signalling
static inline const uint8_t *sfrEngineTxLc(const sfrEngine_t *e)
{
	return e->txLc;
}

// MMDVM uplink (hotspot): every burst heard on the inbound slot goes up, as it was heard. A call heard without its
// header (late entry) gets one made from its LC first, once that's known; one that stops without its terminator gets
// DMR_LOST after SFR_UPLINK_LOST_MS
#define SFR_UPLINK_LOST_MS			300U

#define SFR_UP_SEND					0x01		// send the burst
#define SFR_UP_HEADER_FIRST			0x02		// send a voice LC header (from the call's LC) before it

typedef struct
{
	bool     open;				// a call is going up
	bool     headerSent;		// its voice LC header (heard or made) has gone up
	uint32_t lastMs;
} sfrUplink_t;

void    sfrUplinkReset(sfrUplink_t *u);
uint8_t sfrUplinkBurst(sfrUplink_t *u, const sfrBurst_t *b, bool lcKnown, uint32_t nowMs);
bool    sfrUplinkLost(sfrUplink_t *u, uint32_t nowMs);// true once: send DMR_LOST

// The checksum of a data burst's info bits, as the HR-C6000 gives them (12 bytes): RS(12,9) of the voice LC header
// and terminator, CRC-CCITT of the PI header, CSBK, MBC and data headers, with their masks. Blocks: true (the packet's
// CRC covers them). For when the HR-C6000's own CRC flag can't be trusted (while it transmits)
bool sfrDataBurstCrcOk(uint8_t dataType, const uint8_t *info);

// A burst as the HR-C6000 receives it (264 bits, the MMDVM frame layout: 108 info, 48 sync / EMB / slot type, 108 info),
// from its RX RAM's per slot analysis areas, decoded here: while it transmits, its status registers and decoded info
// bits for the burst heard are wrong at times (on-air tests 3-7)
#define SFR_RAW_LENGTH				33
#define SFR_RAW_MAX_SYNC_ERRORS		4

typedef enum
{
	SFR_RAW_NONE = 0,			// none of these: noise, or not for us
	SFR_RAW_DATA,				// data sync, the slot type's colour code ours, and its checksum good (blocks: none to check)
	SFR_RAW_VOICE_A,			// voice sync
	SFR_RAW_VOICE_EMB			// no sync, the EMB's colour code ours: voice B-F
} sfrRawKind_t;

// dataType, info (12 bytes): for SFR_RAW_DATA. sync: the sync type found (sfrSync_t), for DATA and VOICE_A
uint8_t sfrRawClassify(const uint8_t *frame, uint8_t colourCode, uint8_t *dataType, uint8_t *info, uint8_t *sync);
// The 27 bytes of AMBE of a voice burst (around its sync / EMB), as the HR-C6000's TX RAM takes them
void    sfrRawAmbe(const uint8_t *frame, uint8_t *ambe);

// A voice header or terminator heard while the HR-C6000 sends comes out with its data type wrong at times (0) and info
// bytes 5-11 wrong (on-air tests 5-9): one whose first 5 bytes are the inbound call's LC (FLCO, FID, service options,
// the start of the destination; the LC known from the call's first header, heard before keying) and whose checksum
// fails is that call's: a terminator once voice has been heard since its last header, else a header. Returns the data
// type to repeat it as with the call's LC, or 0xFF (not one)
uint8_t sfrLcBurstGuess(uint8_t reportedType, const uint8_t *info, const uint8_t *callLc, bool voiceSinceHeader);

// The ETSI idle message (TS 102 361-1 D.2), the info bits of an idle burst
extern const uint8_t SFR_IDLE_INFO[12];

// MMDVM hotspot: the host wants SFR when SET_CONFIG has the SIMPLEX flag clear (duplex) and SET_FREQ has RX = TX
bool sfrMmdvmTrigger(bool simplexFlag, uint32_t rxFreq, uint32_t txFreq);
// " SFR TS1>TS2 TS": appended to the GET_VERSION description, parsed by the host. Returns the length written
int  sfrMmdvmVersionSuffix(const sfrConfig_t *cfg, char *buf, size_t size);

#endif
