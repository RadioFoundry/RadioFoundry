// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
/*
 * DMR single frequency repeater (SFR): the HR-C6000 glue for functions/sfr.h, see hrcSfr.h. The radio provides the RF
 * side (radioSfrSwitch() etc., hrcSfr.h); the rest is the same on the STM32 radios and the GD-77 (MK22)
 */

#if defined(HAS_SFR)

#include <string.h>
// TODO(hal): aftermarket firmware headers. Used from them: SPI0/SPI1 page register access (SPI0ReadPageRegByte,
// SPI0WritePageRegByte, SPI0ClearPageRegByteWithMask, SPI0Read/WritePageRegByteArray, SPI1Read/WritePageRegByteArray),
// HRC6000InitDigitalDmrRx, trxGetDMRColorCode, trxSetDMRColorCode, trxGetPowerLevel, trxSetFrequency,
// settingsIsOptionBitSet, settingsUsbMode, codeplugChannelGetFlag, currentChannelData, uiDataGlobal, LedWrite,
// taskENTER/EXIT_CRITICAL, DWT / SystemCoreClock. The radio supplies radioSfrSwitch() etc. (hrcSfr.h).
#include "FreeRTOS.h"
#include "task.h"
#include "hardware/HR-C6000.h"
#include "modem/hrc6000/sfr_glue/hrcSfr.h"
#include "dmr/dmrData.h"
#include "functions/settings.h"
#include "functions/ticks.h"
#include "functions/trx.h"
#include "io/LEDs.h"
#include "user_interface/uiGlobals.h"
#if defined(STM32F405xx)
#include "main.h"// DWT, SystemCoreClock
#else
#include "fsl_device_registers.h"// DWT, SystemCoreClock
#endif

// The HR-C6000's sync pattern register (page 1, 0x04) holds the data sync; it sends the voice sync inverted from it.
// In sfrSync_t order
static const uint8_t SYNC_PATTERNS[4][6] = {
		{ 0xD5, 0xD7, 0xF7, 0x7F, 0xD7, 0x57 },	// MS sourced data sync (voice 7F7D5DD57DFD), the driver's
		{ 0xDF, 0xF5, 0x7D, 0x75, 0xDF, 0x5D },	// BS sourced data sync (voice 755FD7DF75F7)
		{ 0xF7, 0xFD, 0xD5, 0xDD, 0xFD, 0x55 },	// TDMA direct mode TS1 data sync (voice 5D577F7757FF)
		{ 0xD7, 0x55, 0x7F, 0x5F, 0xF7, 0xF5 }	// TDMA direct mode TS2 data sync (voice 7DFFD5F55D5F)
};

// Reg 0x10: DMR, Tier II, timeslot, layer 2, aligned; bit 2 ISRepeater (relay: BS framing and the CACH), bit 0 the slot
#define REG10_RELAY							0x6E

#define BURST_MAX_MS						20	// a burst of ours is over by the next timeslot interrupt after this long
#define BURST_MIN_MS						20	// an RF RX interrupt (GD-77) ends a burst of ours on for this long
#define BURST_STUCK_MS						100	// the timeslot interrupts stopped while a burst was on: back to receive
#define NO_TIMESLOT_MS						100	// no timeslot interrupt for this long: the HR-C6000 has no slot timing

static sfrEngine_t engine;// in the main RAM: the CCM is full on the largest STM32 build

#define UPLINK_SIZE							8	// 480 ms of bursts heard, for the hotspot's task
static sfrBurst_t uplink[UPLINK_SIZE];
static volatile uint8_t uplinkIn = 0;
static volatile uint8_t uplinkOut = 0;

static hrcSfrDebug_t debug = { .reg12 = 0xFF, .rssiMin = HRC_SFR_RSSI_MIN_DEFAULT };
static uint8_t debugFraming = SFR_FRAMING_AUTO;

#define ECHO_WINDOW_MS						15	// a burst heard this soon after one of ours ended is ours (its slot)

static struct
{
	volatile bool enabled;				// repeater role or hotspot SFR: the engine runs
	volatile uint8_t role;
	sfrConfig_t cfg;
	bool hotspotOn;
	uint8_t hotspotCC;
	uint16_t paDac;						// the repeater's PA drive
	bool outboundPhase;					// the slot set up at this timeslot interrupt is the outbound one
	volatile bool keyed;				// our transmission: outbound framing set up in the HR-C6000
	volatile bool burstOn;				// the PA is on for one of our bursts
	volatile bool paArmed;				// HRC_SFR_DEBUG_PA_NEXT_SLOT: the RF TX interrupt came, the PA goes on at the next timeslot one
	volatile uint32_t burstStartMs;
	volatile uint32_t burstEndMs;
	volatile uint32_t txInterruptMs;	// the last RF TX interrupt: the HR-C6000 sends from about 1.2 ms after it, for 30 ms
	uint8_t testStep;					// HRC_SFR_DEBUG_BEACON: the next burst of the test call
	uint32_t lastVoiceMs;				// the last inbound voice burst taken
	uint8_t lastVoiceSeq;				// its burst, 0-5 (A-F)
	uint32_t lastHeaderMs;				// the last inbound voice header and terminator taken
	uint32_t lastHeardMs;				// the last inbound slot's first report (taken or not): one burst per slot
	bool taken;							// capture(): the report was queued
	volatile bool waitPhase;			// resynced: no transmission until a burst of the inbound call has been heard again (in step)
	uint8_t reportStep;					// sfrStep_t of the report being captured (only the Sys interrupt's are timed)
	sfrLateEntry_t lateEntry;			// idle: a voice call without a header starts from a B after its A
	volatile uint32_t tsCount;			// timeslot interrupts
	uint8_t inboundCfgParity;			// the parity of the timeslot interrupts that set up the inbound slot
	volatile bool inboundParityFresh;	// heard since the last resync
	uint32_t rxAmbeHash[6];				// the last inbound voice bursts' AMBE
	uint8_t rxAmbeNext;
	uint32_t lastRxAmbeHash;			// the last inbound voice burst's AMBE
	uint32_t lastTerminatorMs;
	uint32_t lastDataMs;				// the last data burst taken (CSBK, data header, block): a data call's end, for late entry
	int8_t rawArea;						// which of the RX RAM's two raw burst areas has the inbound slot (-1: not known yet)
	uint8_t outKind[2];					// what we set up for the last two outbound slots (debug: 1 voice, 2 data, 3 idle). A
										// burst heard is read (capturePending()) before the next one is set up: [outIdx & 1] is
										// the outbound slot right after it, the one being framed while it was decoded
	uint8_t outIdx;
	uint8_t rssi;						// the inbound slot's RSSI, read at the RF TX interrupt that ends it
	uint32_t rssiMs;
	volatile bool pending;				// a burst heard while we send, read at the next timeslot interrupt
	volatile bool pendingVoiceAOn;		// a voice A heard in the header phase, decided at the next timeslot interrupt
	uint8_t pendingVoiceAWas;
	sfrBurst_t pendingVoiceA;
	uint8_t pendingClass;
	uint8_t pendingType;
	bool pendingCrc;
	uint8_t savedReg10;					// reg 0x10 before our transmission (the driver's HRC6000SetDMR() sets 0x6E)
	volatile uint32_t lastTimeslotMs;
	uint32_t previousTimeslotMs;
	bool activeTiming;					// the HR-C6000's own slot timing, started for traffic with nothing heard
	uint8_t mcuCrc;						// reg 0x40 bit 3 as last written, 0xFF unknown
	bool lcWritten;						// the TX RAM has the LC of the voice superframe being sent
	sfrDedup_t userDedup;
	uint32_t rxFreq;					// the channel's, as last set (trxSetFrequency())
	uint32_t txFreq;
} sfr;

// The SFR's SPI transfers, counted when refused (the bus in use by another context: SPI0inUse)
static inline void spiW(uint8_t page, uint8_t reg, uint8_t val)
{
	if (SPI0WritePageRegByte(page, reg, val) != kStatus_Success)
	{
		debug.spiFail++;
	}
}

static inline void spiWA(uint8_t page, uint8_t reg, const uint8_t *values, uint8_t length)
{
	if (SPI0WritePageRegByteArray(page, reg, values, length) != kStatus_Success)
	{
		debug.spiFail++;
	}
}

static inline uint8_t slotCtl(uint8_t v)
{
	return (uint8_t)(v | ((sfr.keyed && (debug.flags2 & HRC_SFR_DEBUG2_CC_ANY)) ? 0x08 : 0x00));
}

static inline void ccWrite(uint8_t cc)
{
	if ((debug.flags2 & HRC_SFR_DEBUG2_NO_CC_WRITES) == 0)
	{
		spiW(0x04, 0x1F, (uint8_t)(cc << 4));
	}
}

static void setMcuCrc(uint8_t bit)
{
	if (bit != sfr.mcuCrc)
	{
		SPI0ClearPageRegByteWithMask(0x04, 0x40, 0xF7, bit);// CRC_MCU_Control
		sfr.mcuCrc = bit;
	}
}

// The AT1846S between our burst and the receiver, timed with the cycle counter for the debug counters
static bool rfSwitch(bool tx)
{
	uint32_t start = DWT->CYCCNT;
	bool done = radioSfrSwitch(tx, sfr.paDac, ((debug.flags & HRC_SFR_DEBUG_NO_I2C_CHECK) != 0), ((debug.flags & HRC_SFR_DEBUG_TX_OFFSET) != 0));
	uint32_t t10us = (DWT->CYCCNT - start) / (SystemCoreClock / 100000U);
	uint8_t v = (t10us > 255) ? 255 : (uint8_t)t10us;

	if (tx)
	{
		debug.txSwitch10us = v;
	}
	else
	{
		debug.rxSwitch10us = v;
		sfr.burstEndMs = ticksGetMillis();
	}

	if (!done)
	{
		debug.switchSkipped++;
	}

	return done;
}

// ISR: the first burst of a transmission. The outbound framing is set up
static void sessionStart(void)
{
	SPI0WritePageRegByteArray(0x01, 0x04, SYNC_PATTERNS[sfrOutboundSync(&sfr.cfg)], 6);

	SPI0ReadPageRegByte(0x04, 0x10, &sfr.savedReg10);
	if (debug.reg10 != 0)
	{
		spiW(0x04, 0x10, debug.reg10);
	}

	// Reg 0x11 (local channel mode: bit 7 direct simplex, 6 direct duplex, 3 single frequency simplex), 0x80 as set
	// by the init: the datasheet's passive full duplex may want another (on-air sweep)
	if (debug.reg11 != 0)
	{
		spiW(0x04, 0x11, debug.reg11);
	}

	// The HR-C6000 doesn't transmit when the channel is busy with our color code and its access policy is polite
	// (reg 0x21 0xA2, set by the driver's transmissions): the radios on the inbound slot keep it busy
	if ((debug.flags & HRC_SFR_DEBUG_POLITE) == 0)
	{
		SPI0WritePageRegByte(0x04, 0x20, 0x00);// impolite
		SPI0WritePageRegByte(0x04, 0x21, 0x02);
	}

	if ((debug.flags & HRC_SFR_DEBUG_ACTIVE_TIMING) && !sfr.activeTiming)
	{
		SPI0WritePageRegByte(0x04, 0x40, 0xE3);
		sfr.activeTiming = true;
	}

#if defined(HRC_SFR_BS_CACH)
	// Experimental: the HR-C6000 as a BS (relay mode), which should add the CACH. An ETSI single frequency BS uses the
	// guard time framing, without a CACH (TS 102 361-1 4.6.2), and so most likely does an ERDM repeater
	if (sfr.cfg.framing == SFR_FRAMING_BS)
	{
		SPI0WritePageRegByte(0x04, 0x10, (REG10_RELAY | ((sfr.cfg.inboundSlot == 0) ? 0x01 : 0x00)));
	}
#endif

	sfr.mcuCrc = 0xFF;
	sfr.lcWritten = false;
	sfr.keyed = true;
}

// ISR: the transmission is over (or the radio's own needs the channel): the driver's settings back
static void sessionEnd(void)
{
	sfr.paArmed = false;

	if (sfr.burstOn)
	{
		rfSwitch(false);
		sfr.burstOn = false;
	}

	LedWrite(LED_RED, 0);
	SPI0WritePageRegByteArray(0x01, 0x04, SYNC_PATTERNS[SFR_SYNC_MS], 6);
	SPI0WritePageRegByte(0x04, 0x10, sfr.savedReg10);// as it was (0x6E from HRC6000SetDMR(), not the 0x6B of the init)
	if (debug.reg11 != 0)
	{
		SPI0WritePageRegByte(0x04, 0x11, 0x80);// the init's
	}
	SPI0WritePageRegByte(0x04, 0x1F, (trxGetDMRColorCode() << 4));
	setMcuCrc(0x00);

	if (sfr.activeTiming)
	{
		SPI0WritePageRegByte(0x04, 0x40, 0xC3);// passive timing again: the radios' bursts set the slots
		sfr.activeTiming = false;
	}

	sfr.keyed = false;
}

static void txVoiceRemember(const sfrBurst_t *b);
static void txDataRemember(const uint8_t *info);
static bool txDataIsOurs(const uint8_t *info, uint32_t now);

static void writeBurst(const sfrBurst_t *b)
{
	// Whatever color code the receiver was set to (the CC probe), the outbound one
	ccWrite(sfr.cfg.outboundCC);

	if (b->kind == SFR_BURST_VOICE)
	{
		setMcuCrc(0x00);// the HR-C6000 makes the embedded LC's checksum

		if ((b->type == 0) || !sfr.lcWritten)
		{
			uint8_t lc[LC_DATA_LENGTH] = { 0 };

			memcpy(lc, sfrEngineTxLc(&engine), SFR_LC_LENGTH);
			SPI0WritePageRegByteArray(0x02, 0x00, lc, LC_DATA_LENGTH);
			sfr.lcWritten = true;
		}

		SPI1WritePageRegByteArray(0x03, 0x00, b->payload, SFR_VOICE_LENGTH);
		txVoiceRemember(b);
		sfr.outKind[sfr.outIdx & 1] = 1;
		spiW(0x04, 0x41, slotCtl(0x80));// transmit in the next slot
		spiW(0x04, 0x50, (0x08 | (b->type << 4)));// voice burst A-F
	}
	else
	{
		sfr.outKind[sfr.outIdx & 1] = ((b->type == DT_IDLE) ? 3 : 2);
		// Reg 0x40 bit 3 (CRC_MCU_Control) seems to apply to the deframer too: every inbound header / terminator that
		// came out bad was decoded while our idle burst, the only burst sent with it set, was framed (on-air test 9).
		// The idle message has no checksum: it goes with the bit clear. Other data (CSBK, data) still needs it, verbatim
		bool mcuCrc = (b->flags & SFR_BURST_FLAG_MCU_CRC) && ((b->type != DT_IDLE) || (debug.flags2 & HRC_SFR_DEBUG2_IDLE_MCU_CRC));
		setMcuCrc(mcuCrc ? 0x08 : 0x00);

		if (b->type == DT_IDLE)
		{
			// The TX RAM's idle message has its own place: the LC at 0x00 stays, for the voice bursts after the gap
			SPI0WritePageRegByteArray(0x02, 0x18, b->payload, b->length);
		}
		else
		{
			SPI0WritePageRegByteArray(0x02, 0x00, b->payload, b->length);
			sfr.lcWritten = false;

			if ((b->type != DT_VOICE_LC_HEADER) && (b->type != DT_TERMINATOR_WITH_LC))
			{
				txDataRemember(b->payload);
			}
		}

		spiW(0x04, 0x41, slotCtl(0x80));
		spiW(0x04, 0x50, (b->type << 4));// data burst, LCSS 0
	}
}

// The engine afresh, with the debug knob that turns the data call hold off (HRC_SFR_DEBUG2_NO_DATA_HOLD)
static void engineInit(void)
{
	sfrEngineInit(&engine, &sfr.cfg);
	engine.noDataHold = ((debug.flags2 & HRC_SFR_DEBUG2_NO_DATA_HOLD) != 0);
}

// End our transmission and make the HR-C6000 lock to the inbound call afresh (ISR context)
static void resync(void)
{
	sessionEnd();
	sfrEngineFlush(&engine);
	HRC6000InitDigitalDmrRx();// sync fail and receive: passive timing from the next burst heard
	sfr.waitPhase = true;
	sfr.inboundParityFresh = false;
	sfr.pending = false;
	sfr.pendingVoiceAOn = false;
	debug.resyncs++;
}

// Not sending: a burst heard out of step with the HR-C6000's slot timing (locked to noise before the call: on-air
// test 16, z_g, the HAT's preamble). Repeated on that timing it would go out 68 ms after the inbound burst, over the
// next one: the HR-C6000 is made to lock to the call afresh, and nothing goes out until a burst has been heard in step
static void relock(void)
{
	// (A data call being held keeps what was heard of it: heard unkeyed, it's good, and it goes out once the call is
	// over, with the timing of the bursts heard in step)
	if (!engine.rfHold)
	{
		sfrEngineFlush(&engine);
	}
	HRC6000InitDigitalDmrRx();
	sfr.waitPhase = true;
	sfr.inboundParityFresh = false;
	debug.relocks++;
}

static void capturePending(void);
static void capturePendingStatus(void);

static void writeIdle(void)
{
	sfrBurst_t b;

	b.kind = SFR_BURST_DATA;
	b.type = DT_IDLE;
	b.length = sizeof(SFR_IDLE_INFO);
	b.flags = SFR_BURST_FLAG_MCU_CRC;
	memcpy(b.payload, SFR_IDLE_INFO, sizeof(SFR_IDLE_INFO));
	writeBurst(&b);
}

// Task: switch the configuration, with the ISRs out of the way
static void apply(uint8_t role, const sfrConfig_t *cfg)
{
	bool enable = sfrRoleIsRepeater(role);

	if ((role == sfr.role) && (enable == sfr.enabled) && ((role == SFR_ROLE_OFF) || (memcmp(cfg, &sfr.cfg, sizeof(sfrConfig_t)) == 0)))
	{
		return;
	}

	taskENTER_CRITICAL();

	if (sfr.keyed || sfr.activeTiming)
	{
		sessionEnd();
	}

	sfr.enabled = false;
	sfr.role = role;

	if (role != SFR_ROLE_OFF)
	{
		sfr.cfg = *cfg;
	}

	sfrDedupReset(&sfr.userDedup);

	uplinkIn = uplinkOut = 0;
	sfr.rawArea = -1;
	sfr.pending = false;

	if (enable)
	{
		// The cycle counter, for the RF switch timings in the debug counters
		CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
		DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

		engineInit();
		sfr.outboundPhase = false;
		sfr.enabled = true;
	}

	taskEXIT_CRITICAL();
}

void hrcSfrChannelChanged(CodeplugChannel_t *channel, uint32_t rxFreq, uint32_t txFreq)
{
	sfrChannelSettings_t settings;
	sfrConfig_t cfg;
	uint8_t role = SFR_ROLE_OFF;

	memset(&cfg, 0, sizeof(cfg));
	sfr.rxFreq = rxFreq;
	sfr.txFreq = txFreq;

	if (settingsUsbMode == USB_MODE_HOTSPOT)
	{
		// The host decides (hrcSfrHotspotSet), whatever the channel the radio was on has
		if (sfr.hotspotOn)
		{
			hrcSfrHotspotConfig(sfr.hotspotCC, &cfg);
			role = SFR_ROLE_HOTSPOT;
			sfr.paDac = radioSfrPaDrive(trxGetPowerLevel());
		}

		apply(role, &cfg);
		return;
	}

	// One frequency, DMR. A repeater that can't transmit isn't one
	if ((channel->chMode == RADIO_MODE_DIGITAL) && (rxFreq == txFreq) &&
			sfrChannelDecode(channel->LibreDMR_flag1, channel->sfrSettings, channel->sfrOutboundCC, &settings))
	{
		role = settings.role;
		sfrConfigBuild(&settings, channel->txColor, channel->tot, &cfg);

		if (sfrRoleIsRepeater(role))
		{
			if ((codeplugChannelGetFlag(channel, CHANNEL_FLAG_RX_ONLY) != 0) || settingsIsOptionBitSet(BIT_TX_INHIBIT))
			{
				role = SFR_ROLE_OFF;
			}
			else
			{
				sfr.paDac = radioSfrPaDrive((channel->libreDMR_Power != 0) ? (channel->libreDMR_Power - 1) : SFR_REPEATER_DEFAULT_POWER_LEVEL);
			}
		}
	}

	apply(role, &cfg);
}

// Task: the safety nets, if the timeslot interrupts stop
// The TX test call's bursts, from 9999 to TG 9: 2 headers, 4 superframes of silence, a terminator
static const uint8_t TEST_LC[SFR_LC_LENGTH] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x09, 0x00, 0x27, 0x0F };
static const uint8_t SILENCE_AMBE[SFR_VOICE_LENGTH] = {
		0xB9, 0xE8, 0x81, 0x52, 0x61, 0x73, 0x00, 0x2A, 0x6B, 0xB9, 0xE8, 0x81, 0x52, 0x61, 0x73, 0x00, 0x2A, 0x6B,
		0xB9, 0xE8, 0x81, 0x52, 0x61, 0x73, 0x00, 0x2A, 0x6B };
#define TEST_CALL_BURSTS					(2 + (4 * 6) + 1)

static void testCallTick(void)
{
	sfrBurst_t b;

	if (((debug.flags & HRC_SFR_DEBUG_BEACON) == 0) || !sfr.enabled || (engine.count >= 4))
	{
		return;
	}

	memset(&b, 0, sizeof(b));

	if ((sfr.testStep < 2) || (sfr.testStep == (TEST_CALL_BURSTS - 1)))
	{
		b.kind = SFR_BURST_DATA;
		b.type = ((sfr.testStep < 2) ? DT_VOICE_LC_HEADER : DT_TERMINATOR_WITH_LC);
		b.length = LC_DATA_LENGTH;
		memcpy(b.payload, TEST_LC, SFR_LC_LENGTH);
	}
	else
	{
		b.kind = SFR_BURST_VOICE;
		b.type = (sfr.testStep - 2) % 6;
		b.length = SFR_VOICE_LENGTH;
		memcpy(b.payload, SILENCE_AMBE, SFR_VOICE_LENGTH);
	}

	taskENTER_CRITICAL();
	uint8_t err = sfrEngineNetBurst(&engine, &b, TEST_LC, ticksGetMillis());
	taskEXIT_CRITICAL();

	if (err == 0)
	{
		sfr.testStep = (sfr.testStep + 1) % TEST_CALL_BURSTS;
	}
}

void hrcSfrTick(void)
{
	testCallTick();

	// Traffic to send (the network's, in hotspot mode) with no slot timing, as nothing was heard since the HR-C6000
	// was set up: its own timing is started, as for a direct mode transmission
	if (sfr.enabled && !sfr.keyed && !sfr.activeTiming && ((engine.count > 0) || (debug.flags & HRC_SFR_DEBUG_BEACON)) &&
			((ticksGetMillis() - sfr.lastTimeslotMs) > NO_TIMESLOT_MS) && (slotState == DMR_STATE_IDLE))
	{
		taskENTER_CRITICAL();
		sfr.activeTiming = true;
		SPI0WritePageRegByte(0x04, 0x40, 0xE3);// TX and RX enabled, active timing
		taskEXIT_CRITICAL();
	}

	if (sfr.burstOn && ((ticksGetMillis() - sfr.burstStartMs) > BURST_STUCK_MS))
	{
		taskENTER_CRITICAL();
		if (sfr.burstOn)
		{
			rfSwitch(false);
			sfr.burstOn = false;
			LedWrite(LED_RED, 0);
		}
		taskEXIT_CRITICAL();
	}
}

uint8_t hrcSfrRole(void)
{
	return sfr.role;
}

bool hrcSfrIsKeyed(void)
{
	return sfr.keyed;
}

bool hrcSfrRepeaterActive(void)
{
	return sfr.enabled;
}

bool hrcSfrTimeslotStart(void)
{
	sfr.previousTimeslotMs = sfr.lastTimeslotMs;
	sfr.lastTimeslotMs = ticksGetMillis();
	sfr.tsCount++;
	debug.tsPeriodMs = (uint8_t)(sfr.lastTimeslotMs - sfr.previousTimeslotMs);

	if (sfr.paArmed)
	{
		sfr.paArmed = false;
		rfSwitch(true);
		sfr.burstStartMs = sfr.lastTimeslotMs;
		sfr.burstOn = true;
		LedWrite(LED_RED, 1);
		return true;
	}

	if (sfr.burstOn && ((ticksGetMillis() - sfr.burstStartMs) > BURST_MAX_MS))
	{
		rfSwitch(false);
		sfr.burstOn = false;
		debug.burstOnMs = (uint8_t)(ticksGetMillis() - sfr.burstStartMs);
		LedWrite(LED_RED, 0);
		return true;
	}

	return false;
}

void hrcSfrTimeslot(int stateAtEntry, bool localTx)
{
	sfrBurst_t b;

	if (!sfr.enabled)
	{
		return;
	}

	// The radio's own transmission (PTT, data service) has the channel, or it's scanning: what was queued is dropped
	if (localTx || (stateAtEntry >= DMR_STATE_TX_START_1) || uiDataGlobal.Scan.active)
	{
		if (sfr.keyed || (engine.count > 0))
		{
			if (sfr.keyed)
			{
				sessionEnd();
			}
			engineInit();
		}
		return;
	}

	// Not sending, the receiver listens to every slot: the driver's DMO receiver locks to one slot (writes 0x00 for the
	// other), and locked to the wrong one (noise, a preamble) it missed whole header runs and data calls (on-air tests
	// 14-15). Overwritten below in a slot we send in
	if (!sfr.keyed)
	{
		spiW(0x04, 0x41, 0x50);
	}

	// After a resync (resync()), nothing goes out until a burst of the inbound call has been heard again
	if (sfr.waitPhase)
	{
		// (an inbound call lost without a burst heard in step: the engine ends it, its busy flag and hang)
		if (!sfr.inboundParityFresh && (!engine.rfCall || ((ticksGetMillis() - engine.rfLastMs) <= SFR_RF_LOSS_MS)))
		{
			return;
		}

		sfr.waitPhase = false;
	}

	// The slots: a burst heard is reported a few ms after the timeslot interrupt that starts the slot after it, so the
	// interrupts with that one's parity set up the inbound slots, the others the outbound ones (the driver's DMO
	// receiver's DMR_STATE_RX_1 / _2 lock was used before, and could be on the wrong slot)
	sfr.outboundPhase = ((((sfr.tsCount & 1) != sfr.inboundCfgParity) ? 1 : 0) != ((debug.flags & HRC_SFR_DEBUG_PHASE_FLIP) ? 1 : 0));

	if (!sfr.outboundPhase)
	{
		if (sfr.keyed)
		{
			SPI0ReadPageRegByte(0x04, 0x42, &debug.reg42Tx);// the slot starting: ours
			ccWrite(sfr.cfg.inboundCC);
			spiW(0x04, 0x41, slotCtl(0x50));// receive the inbound slot
		}
		return;
	}

	SPI0ReadPageRegByte(0x04, 0x42, &debug.reg42Rx);// the slot starting: the inbound one

	capturePending();

	sfr.outIdx++;
	sfr.outKind[sfr.outIdx & 1] = 0;

	sfrTxAction_t action = sfrEngineOutboundSlot(&engine, ticksGetMillis(), &b);


	switch (action)
	{
		case SFR_TX_BURST:
		case SFR_TX_IDLE:
			if (!sfr.keyed)
			{
				sessionStart();
			}
			else if (debug.flags & HRC_SFR_DEBUG_SYNC_PER_BURST)
			{
				SPI0WritePageRegByteArray(0x01, 0x04, SYNC_PATTERNS[sfrOutboundSync(&sfr.cfg)], 6);
			}

			if (action == SFR_TX_BURST)
			{
				writeBurst(&b);
			}
			else if (((debug.flags2 & HRC_SFR_DEBUG2_IDLE_IN_CALL) == 0) && engine.rfCall)
			{
				// No idle burst in a gap of an inbound call: the outbound slot is left empty (on-air test 10: the inbound
				// bursts heard while our idle bursts were framed came out bad). The hang time still has its idle bursts
				break;
			}
			else
			{
				writeIdle();
			}
			debug.burstsWritten++;
			break;

		case SFR_TX_END:
			sessionEnd();
			SPI0WritePageRegByte(0x04, 0x41, 0x50);// back to receiving every slot
			break;

		default:
			break;
	}
}

bool hrcSfrTxInterrupt(void)
{
	if (!sfr.keyed)
	{
		return false;
	}

	debug.txInterrupts++;

	// The inbound slot ends here (its burst 1.25 ms before our slot): its RSSI, before the AT1846S goes to TX
	if (radioSfrReadRssi(&sfr.rssi))
	{
		sfr.rssiMs = ticksGetMillis();
		debug.lastRssi = sfr.rssi;
	}

	sfr.txInterruptMs = ticksGetMillis();
	debug.txAfterTsMs = (uint8_t)(sfr.txInterruptMs - sfr.lastTimeslotMs);

	if (debug.flags & HRC_SFR_DEBUG_PA_NEXT_SLOT)
	{
		sfr.paArmed = true;
	}
	else
	{
		rfSwitch(true);
		sfr.burstStartMs = ticksGetMillis();
		sfr.burstOn = true;
		LedWrite(LED_RED, 1);
	}

	if (debug.flags & HRC_SFR_DEBUG_SYNC_PER_BURST)
	{
		SPI0WritePageRegByteArray(0x01, 0x04, SYNC_PATTERNS[SFR_SYNC_MS], 6);// the receiver's again, the burst is framed
	}

	return true;
}

bool hrcSfrRxInterrupt(void)
{
	if (!sfr.keyed)
	{
		return false;
	}

	if (sfr.burstOn && ((ticksGetMillis() - sfr.burstStartMs) >= BURST_MIN_MS))
	{
		rfSwitch(false);
		sfr.burstOn = false;
		debug.burstOnMs = (uint8_t)(ticksGetMillis() - sfr.burstStartMs);
		LedWrite(LED_RED, 0);
	}

	return true;
}

// Sys ISR: a burst received (the driver read reg 0x51 and 0x5F, and the color code from reg 0x52)
static uint32_t ambeHash(const uint8_t *ambe);
// The AMBE of one of the last 6 inbound voice bursts (the HR-C6000's stale voice data)
static bool rxAmbeRecent(uint32_t hash);
static void queueHeard(const sfrBurst_t *heard, uint8_t colorCode, uint8_t sync, uint32_t now);

// A burst heard, queued for the repeat (and the uplink). keyed: heard while we send (its status isn't reliable)
static bool txVoiceIsOurs(const uint8_t *ambe, uint8_t seq, uint32_t now);

static void captureBurst(int syncClass, int dataType, int syncType, bool crcValid, uint8_t colorCode, uint32_t now, bool keyed);

// A slot's report, and a header's place kept: in a call's header phase (headers heard, no voice yet), a slot whose
// report couldn't be used (on-air test 13: every third header heard while keyed came out unusable) gets a header with
// the call's LC, so the header run is repeated whole
static void capture(int syncClass, int dataType, int syncType, bool crcValid, uint8_t colorCode, uint32_t now, bool keyed)
{
	sfr.taken = false;
	captureBurst(syncClass, dataType, syncType, crcValid, colorCode, now, keyed);

	if (sfr.taken && !keyed && (sfr.reportStep != SFR_STEP_OUT))
	{
		sfr.inboundCfgParity = (uint8_t)(sfr.tsCount & 1);
		sfr.inboundParityFresh = true;
	}

	if (!sfr.taken && keyed && (sfr.lastHeardMs == now) && sfrEngineRfVoiceCall(&engine) && sfrLcKnown(engine.rfLc) &&
			((int32_t)(sfr.lastHeaderMs - sfr.lastVoiceMs) > 0) &&
			((now - sfr.lastHeaderMs) >= 50) && ((now - sfr.lastHeaderMs) <= 75))
	{
		sfrBurst_t h;

		h.kind = SFR_BURST_DATA;
		h.type = DT_VOICE_LC_HEADER;
		h.length = 12;
		h.flags = 0;
		memset(h.payload, 0, sizeof(h.payload));
		memcpy(h.payload, engine.rfLc, SFR_LC_LENGTH);
		debug.headerFilled++;
		queueHeard(&h, engine.cfg.inboundCC, SFR_SYNC_MS, now);
	}
}

static void captureBurst(int syncClass, int dataType, int syncType, bool crcValid, uint8_t colorCode, uint32_t now, bool keyed)
{
	sfrBurst_t b;

	// One burst per inbound slot: the HR-C6000 raises more than one Sys interrupt for a slot (the EMB parsing one), and
	// the extra one comes with a wrong type at times (on-air test 12: a voice B 12 ms after a header, a header twice
	// 26 ms apart, a terminator right after a voice burst that ended the call). While keyed only the inbound slot is
	// heard, 60 ms apart: anything within 45 ms of the slot's first report is dropped, whatever it says it is. Not keyed
	// both slots are (30 ms apart): within 28 ms
	if ((now - sfr.lastHeardMs) < (keyed ? 45U : 28U))
	{
		debug.rxSameSlot++;
		return;
	}

	// The slot's first report, taken or not. Not keyed, only one with our color code: the noise the HR-C6000 reports
	// in the other slot (random color codes) mustn't shadow the real bursts 30 ms later (on-air test 14: a whole
	// header run lost)
	if (keyed || (colorCode == engine.cfg.inboundCC))
	{
		sfr.lastHeardMs = now;
	}

	// While we send, the HR-C6000's status for the burst heard (reg 0x51 CRC, 0x52 color code, 0x5F sync type) is
	// wrong (on-air tests 3 and 4: inbound headers and terminators dropped as the wrong sync, then the wrong color
	// code, CRC bad). The info bits read out are right: their own checksum is checked instead, and the color code taken
	// as the inbound one. Our own bursts are told apart by their slot (above)
	if (keyed)
	{
		colorCode = engine.cfg.inboundCC;
	}

	if (syncClass == 2)// data
	{
		bool block = (dataType == DT_RATE_12_DATA) || (dataType == DT_RATE_34_DATA) || (dataType == DT_RATE_1_DATA);

		if ((dataType == DT_IDLE) || (dataType > DT_RATE_1_DATA))
		{
			debug.rxIgnored++;
			return;
		}

		b.kind = SFR_BURST_DATA;
		b.type = dataType;
		b.length = (dataType == DT_RATE_1_DATA) ? 24 : ((dataType == DT_RATE_34_DATA) ? 18 : 12);
		// The voice header and terminator: the HR-C6000 adds their checksum, as for our own calls. Everything else is
		// sent as received, CRC and mask included
		b.flags = ((dataType == DT_VOICE_LC_HEADER) || (dataType == DT_TERMINATOR_WITH_LC)) ? 0 : SFR_BURST_FLAG_MCU_CRC;
		SPI0ReadPageRegByteArray(0x02, 0x00, b.payload, b.length);

		if (keyed && (dataType != DT_VOICE_LC_HEADER) && (dataType != DT_TERMINATOR_WITH_LC))
		{
			// The HR-C6000's report of the empty inbound slot with what we sent in it (as it does for voice): never
			// repeated again
			if (txDataIsOurs(b.payload, now))
			{
				debug.rxDataEcho++;
				return;
			}

			// A data call starting in our hang time: heard while we send, its bursts come out with bit errors (on-air
			// test 17: every CSBK and data header heard keyed failed its CRC, by 11-23 bits). The hang ends now, so the
			// rest of the call is heard unkeyed (this burst only counts if its CRC checks)
			if (((dataType == DT_CSBK) || (dataType == DT_MBC_HEADER) || (dataType == DT_DATA_HEADER)) && !engine.noDataHold &&
					sfrEngineEndHang(&engine))
			{
				sessionEnd();
				spiW(0x04, 0x41, 0x50);// back to receiving every slot
				debug.hangCut++;
			}
		}

		// While keyed, a voice header or terminator's info bits come out with bytes 5-11 wrong (on-air tests 5-8, the same
		// pattern every time, whenever read): within the call, whose LC is known from its first header (heard before the
		// repeater keyed), one whose first 5 bytes are that LC's (FLCO, FID, service options, the start of the
		// destination) is that call's: its LC is used, the HR-C6000 makes the checksum
		uint8_t guess = (keyed && sfrEngineRfVoiceCall(&engine)) ?
				sfrLcBurstGuess((uint8_t)dataType, b.payload, engine.rfLc, ((int32_t)(sfr.lastVoiceMs - sfr.lastHeaderMs) > 0)) : 0xFF;

		if (guess != 0xFF)
		{
			b.type = guess;
			b.length = 12;
			b.flags = 0;
			memset(b.payload, 0, b.length);
			memcpy(b.payload, engine.rfLc, SFR_LC_LENGTH);
			debug.rxLcRebuilt++;
			debug.outAtRebuild = sfr.outKind[sfr.outIdx & 1];
		}
		// Data blocks have no CRC of their own (the packet's covers them): only within a call, noise decodes as them
		// (Not keyed, our own check too: a first header the HR-C6000 flagged bad cost a whole call its headers, on air)
		else if (block ? !engine.rfCall : !(keyed ? sfrDataBurstCrcOk(dataType, b.payload) : (crcValid || sfrDataBurstCrcOk(dataType, b.payload))))
		{
			debug.rxIgnored++;
			debug.rxIgnoredNoCrc++;
			debug.rejectedType = (uint8_t)(dataType | (keyed ? 0x80 : 0));
			debug.outAtReject = sfr.outKind[sfr.outIdx & 1];
			memcpy(debug.rejected, b.payload, sizeof(debug.rejected));
			return;
		}
	}
	else if (syncClass != 3)// voice (3: reverse channel)
	{
		// Burst A-F is 1-6 in the low bits (as the driver reads it: the other bits aren't reliable while we send, on-air
		// test 5). Two voice bursts of one call are 60 ms apart: a second interrupt for the same slot (the EMB parsing
		// one) is dropped by its timing, whatever its data type says
		int seq = (dataType & 0x07);

		if ((seq < 1) || (seq > 6) || (!crcValid && !sfrEngineRfVoiceCall(&engine)))
		{
			debug.rxIgnored++;
			return;
		}

		b.kind = SFR_BURST_VOICE;
		b.type = seq - 1;
		b.length = SFR_VOICE_LENGTH;
		b.flags = 0;
		SPI1ReadPageRegByteArray(0x03, 0x00, b.payload, SFR_VOICE_LENGTH);

		// The HR-C6000 reports voice bursts in the empty inbound slot after a call (on-air tests 7-10: after the call the
		// repeater went on repeating them, with what we sent in them). Voice counts only within an inbound voice call:
		// until its terminator, or after a voice header heard since. A data or CSBK call isn't one (on-air test 17: the
		// voice reports after a data call, while keyed, were repeated and sent up as a voice call). Not keyed, a call
		// without a header (late entry) is taken again 10 s after the last call (its terminator, or a data call's last
		// burst), and with a good CRC (on air the post-call reports went on for seconds, and went up to the host once
		// the repeater had stopped sending)
		bool inCall = sfrEngineRfVoiceCall(&engine) ||
				(((int32_t)(sfr.lastHeaderMs - sfr.lastTerminatorMs) > 0) && ((now - sfr.lastHeaderMs) < 1000) && !engine.rfCall);

		if (!inCall)
		{
			// Late entry: from a B 60 ms after its A (a lone voice A, noise or a stray burst, doesn't key the repeater:
			// on-air tests 14-16), both with a good CRC, in step, 10 s after a call's end at the earliest
			bool eligible = !keyed && crcValid && (sfr.reportStep != SFR_STEP_OUT) && ((now - sfr.lastTerminatorMs) >= 10000) &&
					((now - sfr.lastDataMs) >= 10000);
			bool start = eligible && sfrLateEntryVoice(&sfr.lateEntry, b.type, now);

			if (!start)
			{
				if (!eligible)
				{
					sfrLateEntryReset(&sfr.lateEntry);
				}
				else if (b.type == 0)
				{
					debug.lateEntryHeld++;
				}

				debug.rxNoCall++;
				return;
			}
		}

		// A header heard while keyed is now and then reported as voice A (on-air test 11: one header per call repeated
		// as voice A). In the header phase of a call (headers heard, no voice since), a voice A waits for the next
		// timeslot interrupt (still before its repeat is set up): if reg 0x51 then says data (it keeps a data burst's sync
		// class and clears its type), it was a header
		if (keyed && (b.type == 0) && sfrEngineRfVoiceCall(&engine) && sfrLcKnown(engine.rfLc) &&
				((int32_t)(sfr.lastHeaderMs - sfr.lastVoiceMs) > 0) && ((now - sfr.lastHeaderMs) < 100))
		{
			memcpy(&sfr.pendingVoiceA, &b, sizeof(sfrBurst_t));
			sfr.pendingVoiceAWas = (uint8_t)((syncClass << 4) | (dataType & 0x0F));
			sfr.pendingVoiceAOn = true;
			sfr.taken = true;
			return;
		}

		// A voice A right after F with F's AMBE is the HR-C6000's stale voice data, not a new superframe (on-air test 13:
		// a terminator reported as voice A went out after the call's F). A real silence frame there is lost, inaudibly
		if (keyed && (b.type == 0) && (sfr.lastVoiceSeq == 5) && ((now - sfr.lastVoiceMs) < 80) && rxAmbeRecent(ambeHash(b.payload)))
		{
			debug.staleVoice++;
			return;
		}

		if (keyed)
		{
			// And never with the AMBE of one we sent shortly before (at its position within a call, as the same AMBE can
			// come again: silence)

			// Within a call, a burst out of sequence with the AMBE of one we sent, at any position, is ours too
			bool inSequence = (b.type == ((sfr.lastVoiceSeq + 1) % 6)) && ((now - sfr.lastVoiceMs) < 80);

			if (txVoiceIsOurs(b.payload, ((engine.rfCall && inSequence) ? b.type : 0xFF), now))
			{
				debug.rxEchoContent++;
				return;
			}
		}
	}
	else
	{
		return;
	}

	queueHeard(&b, colorCode, (keyed ? SFR_SYNC_MS : (uint8_t)syncType), now);
}

static bool rxAmbeRecent(uint32_t hash)
{
	for (int i = 0; i < 6; i++)
	{
		if (sfr.rxAmbeHash[i] == hash)
		{
			return true;
		}
	}

	return false;
}

// A burst heard, for the repeat and the uplink
static void queueHeard(const sfrBurst_t *heard, uint8_t colorCode, uint8_t sync, uint32_t now)
{
	sfrBurst_t b = *heard;

	sfr.taken = true;

	if (b.kind == SFR_BURST_VOICE)
	{
		sfr.lastVoiceMs = now;
		sfr.lastVoiceSeq = b.type;
		sfr.lastRxAmbeHash = ambeHash(b.payload);
		sfr.rxAmbeHash[sfr.rxAmbeNext] = sfr.lastRxAmbeHash;
		sfr.rxAmbeNext = (sfr.rxAmbeNext + 1) % 6;
	}
	else if (b.type == DT_VOICE_LC_HEADER)
	{
		sfr.lastHeaderMs = now;
	}
	else if (b.type == DT_TERMINATOR_WITH_LC)
	{
		sfr.lastTerminatorMs = now;
	}
	else if (b.type != DT_VOICE_PI_HEADER)
	{
		sfr.lastDataMs = now;
	}

	if (colorCode != engine.cfg.inboundCC)
	{
		debug.rxIgnoredCC++;
	}
	else if ((b.kind == SFR_BURST_DATA) && !sfrInboundSyncAccepted(&engine.cfg, sync))
	{
		debug.rxIgnoredSync++;
	}

	bool wasHolding = engine.rfHold;
	sfrRxResult_t result = sfrEngineRfBurst(&engine, &b, colorCode, sync, now);

	if (engine.rfHold && !wasHolding)
	{
		debug.dataHeld++;
	}

	// Only a call in step with the HR-C6000's slots is repeated (its report 2-10 ms after a timeslot interrupt; 3-5 on
	// air). Sending, a burst out of step is a call with slot timing of its own (a reply in the hang, an RF call over the
	// host's traffic), which the HR-C6000 doesn't follow while it sends (on-air test 14: repeated 69 ms later in its own
	// slot, our bursts heard back as inbound); mid-call it's the HR-C6000's timing off the call's from the start (on-air
	// test 16, z_g: locked to noise before the call, every repeat 68 ms after its inbound burst, over the next one, and
	// garbage heard back). Not sending, the same off timing before the first repeat. Either way what was queued is
	// dropped and the HR-C6000 made to lock to the call afresh: its next bursts heard in step go out 90 ms after them.
	// After the engine has the call (its LC, busy): the burst isn't repeated, it's still sent up. (Only the Sys
	// interrupt's reports are timed: one taken at a timeslot interrupt, a voice A held over, isn't)
	if ((sfr.reportStep == SFR_STEP_OUT) && ((result == SFR_RX_REPEATED) || (result == SFR_RX_FULL)))
	{
		debug.outOfStepMs = (uint8_t)(now - sfr.lastTimeslotMs);

		if (sfr.keyed)
		{
			resync();
		}
		else
		{
			relock();
		}
	}

	debug.rxBursts++;
	debug.sysAfterTsMs = (uint8_t)(now - sfr.lastTimeslotMs);

	switch (result)
	{
		case SFR_RX_REPEATED:
			debug.rxRepeated++;
			break;
		case SFR_RX_IGNORED:
			debug.rxIgnored++;
			break;
		case SFR_RX_DUPLICATE:
			debug.rxDuplicate++;
			break;
		default:
			break;
	}

	// The hotspot sends up what it heard on the inbound slot (not the duplicates, nor what isn't for the repeater)
	if ((sfr.role == SFR_ROLE_HOTSPOT) && ((result == SFR_RX_REPEATED) || (result == SFR_RX_TOT) || (result == SFR_RX_FULL)))
	{
		uint8_t next = (uplinkIn + 1) % UPLINK_SIZE;

		if (next == uplinkOut)
		{
			debug.uplinkOverflow++;
		}
		else
		{
			memcpy(&uplink[uplinkIn], &b, sizeof(sfrBurst_t));
			uplinkIn = next;
		}
	}
}

void hrcSfrRxBurst(int syncClass, int dataType, int syncType, bool crcValid, uint8_t colorCode)
{
	uint32_t now = ticksGetMillis();

	if (!sfr.enabled || uiDataGlobal.Scan.active)
	{
		return;
	}

	debug.lastRx = (uint8_t)(((syncType & 0x03) << 6) | ((syncClass & 0x03) << 4) | (dataType & 0x0F));
	debug.lastRxCC = (colorCode & 0x0F) | (crcValid ? 0x80 : 0);

	// The Sys interrupt comes a few ms into the next slot, for the slot that just ended (debug byte 19). If that was one
	// of ours (our PA went off at its end, at the last timeslot interrupt), what's heard is our own burst: never repeated
	if (((now - sfr.burstEndMs) < ECHO_WINDOW_MS) ||
			(((now - sfr.txInterruptMs) >= 25) && ((now - sfr.txInterruptMs) < (31 + ECHO_WINDOW_MS))))// the HR-C6000's slot, whatever the PA did
	{
		debug.rxEcho++;
		return;
	}

	if (sfr.keyed)
	{
		// While we send, the HR-C6000 reports bursts in the inbound slot with nothing on the air there (on-air test 6:
		// after the call, voice A-D "heard" out of nothing, repeated, and heard again, for seconds). The inbound slot's
		// RSSI, read at the RF TX interrupt that ends it (hrcSfrTxInterrupt()), must show a signal
		if ((debug.rssiMin != 0) && (((now - sfr.rssiMs) > 15) || (sfr.rssi < debug.rssiMin)))
		{
			debug.rxWeak++;
			return;
		}

		SPI0ReadPageRegByte(0x04, 0x56, &debug.reg56);
		SPI0ReadPageRegByte(0x04, 0x57, &debug.reg57);

		// Read now, as when not keyed: the info bits came out half wrong while our idle bursts were sent with
		// CRC_MCU_Control set (fixed), and read at the next timeslot interrupt (0xD5 flags2 0x10, the old way) the
		// status has been cleared by then (data type 0) and the info bits are those half wrong ones (on-air test 10)
		if ((debug.flags2 & HRC_SFR_DEBUG2_DEFER_READ) == 0)
		{
			sfr.reportStep = sfrReportStep(now - sfr.lastTimeslotMs);
			capture(syncClass, dataType, syncType, crcValid, engine.cfg.inboundCC, now, true);
			sfr.reportStep = SFR_STEP_UNKNOWN;
			return;
		}

		sfr.pending = true;
		sfr.pendingClass = (uint8_t)syncClass;
		sfr.pendingType = (uint8_t)dataType;
		sfr.pendingCrc = crcValid;
		return;
	}

	sfr.reportStep = sfrReportStep(now - sfr.lastTimeslotMs);
	capture(syncClass, dataType, syncType, crcValid, colorCode, now, false);
	sfr.reportStep = SFR_STEP_UNKNOWN;
}

// Our own voice bursts, for the echo check: a voice burst "heard" with the same AMBE and position as one we sent
// shortly before is ours (on-air test 7: after the call, the repeater kept repeating its own superframes)
#define TX_VOICE_HISTORY					6
#define TX_VOICE_ECHO_MS					250	// less than a superframe (360 ms): the same burst of the next one is new

static struct
{
	uint32_t hash;
	uint32_t ms;
	uint8_t seq;
} txVoice[TX_VOICE_HISTORY];
static uint8_t txVoiceNext = 0;

static uint32_t ambeHash(const uint8_t *ambe)
{
	uint32_t h = 2166136261U;

	for (int i = 0; i < SFR_VOICE_LENGTH; i++)
	{
		h = (h ^ ambe[i]) * 16777619U;
	}

	return h;
}

static void txVoiceRemember(const sfrBurst_t *b)
{
	txVoice[txVoiceNext].hash = ambeHash(b->payload);
	txVoice[txVoiceNext].ms = ticksGetMillis();
	txVoice[txVoiceNext].seq = b->type;
	txVoiceNext = (txVoiceNext + 1) % TX_VOICE_HISTORY;
}

static bool txVoiceIsOurs(const uint8_t *ambe, uint8_t seq, uint32_t now)
{
	uint32_t h = ambeHash(ambe);

	for (int i = 0; i < TX_VOICE_HISTORY; i++)
	{
		if ((txVoice[i].hash == h) && ((seq == 0xFF) || (txVoice[i].seq == seq)) && ((now - txVoice[i].ms) < TX_VOICE_ECHO_MS))// 0xFF: any position
		{
			return true;
		}
	}

	return false;
}

// Our own data bursts (CSBK, data headers and blocks), for the echo check while keyed: the HR-C6000 reports the empty
// inbound slot with what we sent in it (voice, on-air test 7). A data call goes out after it's over (sfrEngine_t
// rfHold), so nothing of it is still being heard then: the same 12 bytes less than 250 ms after we sent them are ours
#define TX_DATA_HISTORY						4

static struct
{
	uint32_t hash;
	uint32_t ms;
} txData[TX_DATA_HISTORY];
static uint8_t txDataNext = 0;

static uint32_t infoHash(const uint8_t *info)
{
	uint32_t h = 2166136261U;

	for (int i = 0; i < 12; i++)
	{
		h = (h ^ info[i]) * 16777619U;
	}

	return h;
}

static void txDataRemember(const uint8_t *info)
{
	txData[txDataNext].hash = infoHash(info);
	txData[txDataNext].ms = ticksGetMillis();
	txDataNext = (txDataNext + 1) % TX_DATA_HISTORY;
}

static bool txDataIsOurs(const uint8_t *info, uint32_t now)
{
	uint32_t h = infoHash(info);

	for (int i = 0; i < TX_DATA_HISTORY; i++)
	{
		if ((txData[i].ms != 0) && (txData[i].hash == h) && ((now - txData[i].ms) < TX_VOICE_ECHO_MS))
		{
			return true;
		}
	}

	return false;
}

// The RX RAM's raw burst areas (264 bits as received, one per HR-C6000 slot: "264bit_info_0 / _1")
static const uint8_t RAW_AREAS[2] = { 0x30, 0x60 };

// The timeslot ISR, before the outbound slot is set up: a burst heard while we send, read now. While the HR-C6000
// sends, its status registers and decoded info bits for the burst heard are wrong at times (on-air tests 3-7: wrong
// color code, sync type and CRC flag, a header taken for voice A, a terminator's info bits half wrong). The burst is
// decoded here from the raw bits instead: data by its sync, slot type and own checksum, voice A by its sync, B-F by the
// EMB's color code, following the burst before. Until a raw area has given a burst, the HR-C6000's status is used
static void capturePending(void)
{
	uint8_t raw[2][SFR_RAW_LENGTH];
	uint8_t kind[2], dt[2] = { 0, 0 }, info[2][12], sync[2] = { 0, 0 };
	uint32_t now = ticksGetMillis();
	int use = -1;

	sfr.reportStep = SFR_STEP_UNKNOWN;// (at the timeslot interrupt: not timed)

	if (sfr.pendingVoiceAOn)
	{
		uint8_t r51 = 0;

		sfr.pendingVoiceAOn = false;
		SPI0ReadPageRegByte(0x04, 0x51, &r51);
		debug.voiceAWas = sfr.pendingVoiceAWas;
		debug.voiceAIs = r51;

		if (((r51 & 0x03) == 2) || (debug.flags2 & HRC_SFR_DEBUG2_VOICE_A_IN_HEADERS_IS_HEADER))
		{
			// A header: the call's LC
			sfrBurst_t h;

			h.kind = SFR_BURST_DATA;
			h.type = DT_VOICE_LC_HEADER;
			h.length = 12;
			h.flags = 0;
			memset(h.payload, 0, sizeof(h.payload));
			memcpy(h.payload, engine.rfLc, SFR_LC_LENGTH);
			debug.headerAsVoiceA++;
			queueHeard(&h, engine.cfg.inboundCC, SFR_SYNC_MS, now);
		}
		else
		{
			queueHeard(&sfr.pendingVoiceA, engine.cfg.inboundCC, SFR_SYNC_MS, now);
		}
	}

	if (!sfr.pending)
	{
		return;
	}

	sfr.pending = false;

	for (int a = 0; a < 2; a++)
	{
		SPI0ReadPageRegByteArray(0x02, RAW_AREAS[a], raw[a], SFR_RAW_LENGTH);
		kind[a] = sfrRawClassify(raw[a], engine.cfg.inboundCC, &dt[a], info[a], &sync[a]);
	}

	debug.rawKinds = (uint8_t)(kind[0] | (kind[1] << 4));

	// A data burst or a voice sync is proof: it also tells which area the inbound slot uses
	for (int n = 0; n < 2; n++)
	{
		int a = ((sfr.rawArea == 1) ? (1 - n) : n);// the known area first

		if ((kind[a] == SFR_RAW_DATA) || (kind[a] == SFR_RAW_VOICE_A))
		{
			use = a;
			sfr.rawArea = (int8_t)a;
			break;
		}
	}

	if ((use < 0) && (sfr.rawArea >= 0) && (kind[sfr.rawArea] == SFR_RAW_VOICE_EMB))
	{
		use = sfr.rawArea;
	}

	if (use < 0)
	{
		memcpy(debug.raw, raw[(sfr.rawArea >= 0) ? sfr.rawArea : 0], sizeof(debug.raw));

		if (sfr.rawArea < 0)
		{
			capturePendingStatus();// raw areas not seen working yet
		}
		else
		{
			debug.rawNone++;
		}
		return;
	}

	sfrBurst_t b;

	if (kind[use] == SFR_RAW_DATA)
	{
		debug.rawData++;
		b.kind = SFR_BURST_DATA;
		b.type = dt[use];
		b.length = 12;
		b.flags = ((b.type == DT_VOICE_LC_HEADER) || (b.type == DT_TERMINATOR_WITH_LC)) ? 0 : SFR_BURST_FLAG_MCU_CRC;
		memcpy(b.payload, info[use], 12);

		if (((b.type == DT_RATE_12_DATA) || (b.type == DT_RATE_34_DATA) || (b.type == DT_RATE_1_DATA)) && !engine.rfCall)
		{
			return;// blocks only within a call
		}
	}
	else
	{
		uint8_t seq;

		if (kind[use] == SFR_RAW_VOICE_A)
		{
			debug.rawVoiceA++;
			seq = 0;
		}
		else
		{
			// B-F: the one after the last voice burst heard, a superframe position on
			if (((now - sfr.lastVoiceMs) > 80) || (sfr.lastVoiceSeq >= 5))
			{
				debug.rawNone++;
				return;
			}

			debug.rawEmb++;
			seq = sfr.lastVoiceSeq + 1;
		}

		b.kind = SFR_BURST_VOICE;
		b.type = seq;
		b.length = SFR_VOICE_LENGTH;
		b.flags = 0;
		sfrRawAmbe(raw[use], b.payload);

		if (txVoiceIsOurs(b.payload, seq, now))
		{
			debug.rxEchoContent++;
			return;
		}
	}

	queueHeard(&b, engine.cfg.inboundCC, SFR_SYNC_MS, now);
}

// Without the raw areas: the status read at the interrupt and the info bits now
static void capturePendingStatus(void)
{
	uint8_t r51 = 0, r5F = 0;

	if ((SPI0ReadPageRegByte(0x04, 0x51, &r51) != kStatus_Success) || (SPI0ReadPageRegByte(0x04, 0x5F, &r5F) != kStatus_Success))
	{
		return;
	}

	int syncClass = r51 & 0x03;
	int dataType = (r51 >> 4) & 0x0F;
	bool crcValid = (((r51 >> 2) & 0x01) == 0);

	if ((syncClass != sfr.pendingClass) || (dataType != sfr.pendingType))
	{
		debug.rereadDiffers++;
		debug.pendingWas = (uint8_t)((sfr.pendingClass << 4) | (sfr.pendingType & 0x0F));
		debug.pendingIs = (uint8_t)((syncClass << 4) | (dataType & 0x0F));
	}

	// By the next timeslot interrupt the HR-C6000 has often cleared reg 0x51 (on air with eb78cd3: re-read 0x00 for
	// interrupt time 0x0E). A re-read that is neither data nor voice A-F keeps the status read at the interrupt
	if ((syncClass != 2) && (((dataType & 0x07) < 1) || ((dataType & 0x07) > 6)))
	{
		syncClass = sfr.pendingClass;
		dataType = sfr.pendingType;
		crcValid = sfr.pendingCrc;
	}

	capture(syncClass, dataType, (r5F & 0x03), crcValid, engine.cfg.inboundCC, ticksGetMillis(), true);
}

bool hrcSfrUplinkPop(sfrBurst_t *burst)
{
	if (uplinkOut == uplinkIn)
	{
		return false;
	}

	memcpy(burst, &uplink[uplinkOut], sizeof(sfrBurst_t));
	uplinkOut = (uplinkOut + 1) % UPLINK_SIZE;

	return true;
}

const uint8_t *hrcSfrRfLc(void)
{
	return engine.rfLc;
}

void hrcSfrSendRejected(void)
{
	debug.sendRejected++;
}

const hrcSfrDebug_t *hrcSfrDebug(void)
{
	debug.rawArea = sfr.rawArea;
	debug.state = (sfr.keyed ? 0x01 : 0) | (sfr.activeTiming ? 0x02 : 0) | (sfr.outboundPhase ? 0x04 : 0) | (sfr.burstOn ? 0x08 : 0) |
			(engine.rfCall ? 0x10 : 0) | (engine.rfBusy ? 0x20 : 0);
	return &debug;
}

// Task context
void hrcSfrDebugSet(uint8_t flags, uint8_t framing, uint8_t reg10, uint8_t reg12, uint8_t rssiMin, uint8_t reg11, uint8_t flags2)
{
	debug.reg11 = reg11;
	debug.flags2 = flags2;
	engine.noDataHold = ((flags2 & HRC_SFR_DEBUG2_NO_DATA_HOLD) != 0);
	debug.flags = flags;
	debug.reg10 = reg10;
	debug.rssiMin = ((rssiMin != 0) ? rssiMin : HRC_SFR_RSSI_MIN_DEFAULT);
	sfr.testStep = 0;

	if ((reg12 != 0xFF) && (reg12 <= 0x3F))
	{
		taskENTER_CRITICAL();
		SPI0WritePageRegByte(0x04, 0x12, reg12);// RF TX advance (rf_pre_on), 100 us steps; the driver's is 0x0C
		taskEXIT_CRITICAL();
		debug.reg12 = reg12;
	}
	debugFraming = (framing < SFR_FRAMING_NUM) ? framing : SFR_FRAMING_AUTO;

	if (sfr.hotspotOn)
	{
		hrcSfrHotspotSet(true, sfr.hotspotCC);// the new framing
	}
}

void hrcSfrRxLc(const uint8_t *lc)
{
	// Only a voice call's embedded LC (not the HR-C6000's LC registers during a data call)
	if (sfr.enabled && sfrEngineRfVoiceCall(&engine))
	{
		sfrEngineRfLc(&engine, lc);
	}
}

bool hrcSfrUserVoiceAccept(int seq)
{
	if (!sfrRoleIsUser(sfr.role))
	{
		return true;
	}

	return sfrDedupVoice(&sfr.userDedup, (uint8_t)seq, ticksGetMillis());
}

// ERDM user: the receiver follows the color code heard, the channel's (direct) or the outbound one (the repeat)
void hrcSfrUserRxColorCode(uint8_t rxCC, bool idle)
{
	if ((sfr.role == SFR_ROLE_USER_ERDM) && idle && (sfr.cfg.outboundCC != sfr.cfg.inboundCC) &&
			(rxCC != trxGetDMRColorCode()) && (sfrUserRxColorCode(&sfr.cfg, rxCC) >= 0))
	{
		trxSetDMRColorCode(rxCC);
	}
}

// ERDM user: it transmits with the channel's (inbound) color code
void hrcSfrUserTxColorCode(void)
{
	if ((sfr.role == SFR_ROLE_USER_ERDM) && (trxGetDMRColorCode() != sfr.cfg.inboundCC))
	{
		trxSetDMRColorCode(sfr.cfg.inboundCC);
	}
}

// The hotspot SFR's settings: the radio's channel's when it's an SFR repeater channel (its inbound slot, framing and
// hang), else TS1 in, TS2 out, TS framing, 1 s hang. The color code is the host's
void hrcSfrHotspotConfig(uint8_t colorCode, sfrConfig_t *cfg)
{
	sfrChannelSettings_t settings;

	if (!(sfrChannelDecode(currentChannelData->LibreDMR_flag1, currentChannelData->sfrSettings, currentChannelData->sfrOutboundCC, &settings) &&
			sfrRoleIsRepeater(settings.role)))
	{
		memset(&settings, 0, sizeof(settings));
	}

	settings.role = SFR_ROLE_HOTSPOT;
	settings.outboundCCSet = false;

	if (debugFraming != SFR_FRAMING_AUTO)
	{
		settings.framing = debugFraming;
	}

	sfrConfigBuild(&settings, colorCode, currentChannelData->tot, cfg);
}

void hrcSfrHotspotSet(bool on, uint8_t colorCode)
{
	sfr.hotspotOn = on;
	sfr.hotspotCC = colorCode;
	hrcSfrChannelChanged(currentChannelData, sfr.rxFreq, sfr.txFreq);
}

bool hrcSfrHotspotActive(void)
{
	return sfr.enabled && (sfr.role == SFR_ROLE_HOTSPOT);
}

uint8_t hrcSfrHotspotNetBurst(const sfrBurst_t *burst, const uint8_t *lc)
{
	uint8_t err;

	if (!hrcSfrHotspotActive())
	{
		return 2;// wrong mode
	}

	taskENTER_CRITICAL();
	err = sfrEngineNetBurst(&engine, burst, lc, ticksGetMillis());
	taskEXIT_CRITICAL();

	return err;
}

void hrcSfrHotspotNetAbort(void)
{
	if (hrcSfrHotspotActive())
	{
		taskENTER_CRITICAL();
		sfrEngineNetAbort(&engine);
		taskEXIT_CRITICAL();
	}
}

bool hrcSfrRfBusy(void)
{
	return sfr.enabled && sfrEngineRfBusy(&engine);
}

uint8_t hrcSfrSpace(void)
{
	return sfr.enabled ? sfrEngineSpace(&engine) : 0;
}

#endif // HAS_SFR
