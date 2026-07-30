// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
/*
 * Host unit test for the DMR data service's replies (functions/dmrDataService.c): the reply queue filled by the
 * HR-C6000 task and emptied by the main task, and a TMS message retried by its sender kept once but ACKed again
 *
 *   make -C tests/host
 *
 * The service is included as source, with the firmware it uses stubbed (stubs/), so that its static state can be
 * looked at. Transmissions are decoded again with functions/dmrData.c
 */

#include <stdio.h>
#include <string.h>
#define DMR_DATA_SERVICE_TEST
#include "../../core/dmr/dmrDataService.c"

static int failures = 0;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

#define OUR_ID		1000
#define RADIO_A		2001
#define RADIO_B		2002

// Stubbed firmware
static uint32_t millis = 100000;
uint32_t trxDMRID = OUR_ID;
uint32_t trxTalkGroupOrPcId = 9;
int trxDMRModeRx = 0;// direct mode
int settingsUsbMode = 0;
settingsStruct_t nonVolatileSettings = { BAND_LIMITS_NONE };
settingsStruct_t originalNonVolatileSettings = { .magicNumber = (int32_t)0xDEADBEEF };// no options menu open
static struct_codeplugChannel_t channel = { 446000000 };
struct_codeplugChannel_t *currentChannelData = &channel;
struct_codeplugRxGroup_t currentRxGroupData;
uiDataGlobal_t uiDataGlobal = { OUR_ID };
const int16_t MELODY_PRIVATE_CALL[] = { -1, -1 };
static int melodies = 0;

uint32_t ticksGetMillis(void) { return millis; }
int trxGetMode(void) { return RADIO_MODE_DIGITAL; }
bool trxCheckFrequencyInAmateurBand(uint32_t freq) { return true; }
bool trxCarrierDetected(void) { return false; }
int trxGetDMRTimeSlot(void) { return 0; }
int trxGetDMRColourCode(void) { return 1; }
int HRC6000DataTxGetStatus(void) { return 0; }
uint32_t HRC6000GetReceivedSrcId(void) { return 0; }
int codeplugChannelGetFlag(struct_codeplugChannel_t *c, int flag) { return 0; }
bool settingsIsOptionBitSet(int bit) { return false; }
void settingsSetOptionBit(int bit, bool set) { }
void settingsSetDirty(void) { }
bool settingsIsOptionBitSetFromSettings(settingsStruct_t *sets, int bit) { return (sets->bitfieldOptions & bit) == (uint32_t)bit; }
void soundSetMelody(const int16_t *melody) { if (melody == MELODY_PRIVATE_CALL) melodies++; }
bool contactIDLookup(uint32_t id, int callType, char *name) { return false; }
bool dmrIDLookup(uint32_t id, dmrIdDataStruct_t *record) { snprintf(record->text, sizeof(record->text), "ID:%u", (unsigned)id); return true; }
void displayLightTrigger(bool on) { }
void uiNotificationShow(int type, int id, uint32_t ms, const char *message, bool immediate) { }
void rxPowerSavingSetState(int state) { }
bool usbNcmIsUp(void) { return false; }
bool ipGatewayDeliverMonitor(const uint8_t *record, int length) { return true; }
bool ipGatewayDeliverIP(bool group, uint32_t dst, uint32_t src, uint8_t protocol, const uint8_t *l4, int length) { return true; }

static bool haveLocation = false;
bool dmrDataServiceGetLocation(dmrDataLocation_t *location)
{
	if (!haveLocation)
	{
		return false;
	}
	memset(location, 0, sizeof(*location));
	location->fixValid = true;
	location->north = true;
	location->latDegrees = 33;
	location->lonDegrees = 84;
	location->speedKnots = DMR_LOCATION_SPEED_UNKNOWN;
	return true;
}

static int positionLatMinutes = 0;// the test moves the radio north by this many 1/10000 minutes
bool dmrDataServiceGetPosition(lrrpPosition_t *position)
{
	memset(position, 0, sizeof(*position));
	if (!dmrDataServiceGetLocation(&position->location))
	{
		return false;
	}
	position->location.latMinutes10000 = positionLatMinutes;
	position->utcYear = 2026;
	position->utcMonth = 10;
	position->utcDay = 8;
	return true;
}

// What went on air, decoded: 'R' ETSI response, 'T' TMS ACK, '?' anything else
#define MAX_SENT	16
static struct { char kind; uint32_t dst; uint8_t seq; } sent[MAX_SENT];
static int sentCount = 0;

bool HRC6000DataTxStart(const dmrBurst_t *bursts, int count)
{
	char kind = '?';
	uint32_t dst = 0;
	uint8_t seq = 0;

	int preambles = 0;

	for (int i = 0; i < count; i++)
	{
		if ((bursts[i].dataType == DT_CSBK) && ((bursts[i].payload[0] & 0x3F) == DMR_CSBKO_PREAMBLE))
		{
			preambles++;
		}
		else if ((bursts[i].dataType == DT_DATA_HEADER) && ((bursts[i].payload[0] & 0x0F) == DMR_DPF_UDT) &&
					((bursts[i].payload[1] & 0x0F) == DMR_UDT_FORMAT_NMEA))
		{
			kind = 'G';// seq: the preambles before it
			dst = (bursts[i].payload[2] << 16) | (bursts[i].payload[3] << 8) | bursts[i].payload[4];
			seq = preambles;
		}
	}

	dmrDataRxReset();
	for (int i = 0; (i < count) && (kind != 'G'); i++)
	{
		if (dmrDataRxBurst(&bursts[i]) == DMR_DATA_RX_PACKET)
		{
			dmrDataUDP_t udp;
			dmrDataTMS_t tms;

			dst = dmrDataRxPacket.dst;
			if (dmrDataRxPacket.dpf == DMR_DPF_RESPONSE)
			{
				kind = 'R';
			}
			else if (dmrDataGetUDP(&dmrDataRxPacket, &udp) && (udp.appPort == DMR_UDP_PORT_TMS) &&
						dmrDataDecodeTMS(udp.payload, udp.length, &tms) && tms.isAck)
			{
				kind = 'T';
				seq = tms.seq;
			}
			else if (dmrDataGetUDP(&dmrDataRxPacket, &udp) && (udp.appPort == DMR_UDP_PORT_LRRP))
			{
				kind = 'L';// seq: the LRRP document ID
				seq = udp.payload[0];
			}
			else if (dmrDataGetUDP(&dmrDataRxPacket, &udp) && (udp.appPort == DMR_UDP_PORT_ARS))
			{
				kind = 'A';// seq: the ARS PDU type
				seq = dmrDataDecodeARS(udp.payload, udp.length) & 0xFF;
			}
		}
	}

	if (sentCount < MAX_SENT)
	{
		sent[sentCount].kind = kind;
		sent[sentCount].dst = dst;
		sent[sentCount].seq = seq;
		sentCount++;
	}
	return true;
}

// The main task: ticks every 10 ms for a while, everything queued goes out
static void runMainTask(int ms)
{
	for (int t = 0; t < ms; t += 10)
	{
		millis += 10;
		dmrDataServiceTick();
	}
}

// HR-C6000 task: a TMS message from src to us, burst by burst (seq: dmrDataBuildTMS sends 5 bits)
static void receiveTMS(uint32_t src, const char *text, uint8_t seq)
{
	dmrBurst_t bursts[DMR_DATA_MAX_BURSTS];
	int count = dmrDataBuildTMS(false, OUR_ID, src, text, seq, true, DT_RATE_12_DATA, 1, bursts, DMR_DATA_MAX_BURSTS);

	CHECK(count > 0);
	dmrDataRxReset();
	for (int i = 0; i < count; i++)
	{
		millis += 60;
		dmrDataServiceRxBurst(&bursts[i]);
	}
}

// The same, as confirmed data with a response requested: an ETSI response, then the TMS ACK
static void receiveConfirmedTMS(uint32_t src, const char *text, uint8_t seq, uint8_t sendSeq)
{
	static dmrDataPacket_t packet;
	dmrBurst_t bursts[DMR_DATA_MAX_BURSTS];
	int count = dmrDataBuildTMS(false, OUR_ID, src, text, seq, true, DT_RATE_12_DATA, 0, bursts, DMR_DATA_MAX_BURSTS);
	dmrDataRxResult_t result = DMR_DATA_RX_NONE;

	dmrDataRxReset();
	for (int i = 0; (i < count) && (result != DMR_DATA_RX_PACKET); i++)
	{
		result = dmrDataRxBurst(&bursts[i]);
	}
	CHECK(result == DMR_DATA_RX_PACKET);
	packet = dmrDataRxPacket;
	packet.dpf = DMR_DPF_CONFIRMED;
	packet.responseRequested = true;
	packet.sendSeq = sendSeq;
	millis += 60;
	handlePacket(&packet);
}

static void reset(void)
{
	runMainTask(6000);// anything left over goes, or ages out
	sentCount = 0;
	melodies = 0;
}

#if defined(HAS_DMR_ARS)
#define ARS_SERVER	9990100

// The server's ARS PDU to us, over the air
static void receiveARS(const uint8_t *pdu, int length)
{
	dmrBurst_t bursts[DMR_DATA_MAX_BURSTS];
	int count = dmrDataBuildUDP(false, OUR_ID, ARS_SERVER, DMR_UDP_PORT_ARS, DMR_UDP_PORT_ARS, pdu, length, DT_RATE_12_DATA, 1,
								bursts, DMR_DATA_MAX_BURSTS);

	CHECK(count > 0);
	dmrDataRxReset();
	for (int i = 0; i < count; i++)
	{
		millis += 60;
		dmrDataServiceRxBurst(&bursts[i]);
	}
}

static int countSent(char kind, uint8_t seq)
{
	int n = 0;

	for (int i = 0; i < sentCount; i++)
	{
		n += ((sent[i].kind == kind) && (sent[i].seq == seq) && (sent[i].dst == ARS_SERVER));
	}
	return n;
}

static void testARS(void)
{
	static const uint8_t ACK_REFRESH_1[] = { 0x00, 0x02, 0xBF, 0x01 };// hbnet's: accepted, refresh 1 (30 min)
	static const uint8_t ACK_PLAIN[] = { 0x00, 0x01, 0x3F };// accepted, no refresh given
	static const uint8_t ACK_REFRESH_0[] = { 0x00, 0x02, 0xBF, 0x00 };// accepted, refresh not expected
	static const uint8_t ACK_REFRESH_3[] = { 0x00, 0x02, 0xBF, 0x03 };// accepted, refresh 3 (90 min)
	static const uint8_t NAK[] = { 0x00, 0x01, 0x7F };// the ACK/NAK bit: refused
	static const uint8_t QUERY[] = { 0x00, 0x01, 0x34 };
	bool accepted;
	uint16_t refresh;

	CHECK(dmrDataDecodeARSResponse(ACK_REFRESH_1, sizeof(ACK_REFRESH_1), &accepted, &refresh) && accepted && (refresh == 30));
	CHECK(dmrDataDecodeARSResponse(ACK_PLAIN, sizeof(ACK_PLAIN), &accepted, &refresh) && accepted && (refresh == DMR_ARS_REFRESH_NOT_GIVEN));
	CHECK(dmrDataDecodeARSResponse(ACK_REFRESH_0, sizeof(ACK_REFRESH_0), &accepted, &refresh) && accepted && (refresh == 0));
	CHECK(dmrDataDecodeARSResponse(ACK_REFRESH_3, sizeof(ACK_REFRESH_3), &accepted, &refresh) && accepted && (refresh == 90));
	CHECK(dmrDataDecodeARSResponse(NAK, sizeof(NAK), &accepted, &refresh) && !accepted);
	CHECK(!dmrDataDecodeARSResponse(QUERY, sizeof(QUERY), &accepted, &refresh));

	reset();
	CHECK(dmrDataServiceArsState() == DMR_ARS_STATE_OFF);

	// Switched on: registers 5-15 s later
	nonVolatileSettings.bitfieldOptions |= BIT_DMR_ARS;
	nonVolatileSettings.dmrArsId[0] = (ARS_SERVER >> 16) & 0xFF;
	nonVolatileSettings.dmrArsId[1] = (ARS_SERVER >> 8) & 0xFF;
	nonVolatileSettings.dmrArsId[2] = ARS_SERVER & 0xFF;
	runMainTask(4000);
	CHECK(countSent('A', DMR_ARS_DEVICE_REGISTRATION) == 0);
	runMainTask(20000);
	CHECK(countSent('A', DMR_ARS_DEVICE_REGISTRATION) == 1);
	CHECK(dmrDataServiceArsState() == DMR_ARS_STATE_REGISTERING);

	// Accepted with a 30 min refresh: registered, again after 30 min
	receiveARS(ACK_REFRESH_1, sizeof(ACK_REFRESH_1));
	runMainTask(1000);
	CHECK(dmrDataServiceArsState() == DMR_ARS_STATE_REGISTERED);
	sentCount = 0;
	millis += (29 * 60000);
	runMainTask(1000);
	CHECK(countSent('A', DMR_ARS_DEVICE_REGISTRATION) == 0);
	millis += 60000;
	runMainTask(1000);
	CHECK(countSent('A', DMR_ARS_DEVICE_REGISTRATION) == 1);
	CHECK(dmrDataServiceArsState() == DMR_ARS_STATE_REGISTERING);

	// Refused: shown so, and tried again on the retry schedule (30 s)
	receiveARS(NAK, sizeof(NAK));
	runMainTask(1000);
	CHECK(dmrDataServiceArsState() == DMR_ARS_STATE_REFUSED);
	sentCount = 0;
	runMainTask(31000);
	CHECK(countSent('A', DMR_ARS_DEVICE_REGISTRATION) == 1);

	// Accepted without a refresh time: the 30 min default
	receiveARS(ACK_PLAIN, sizeof(ACK_PLAIN));
	runMainTask(1000);
	CHECK(dmrDataServiceArsState() == DMR_ARS_STATE_REGISTERED);
	CHECK(ars.refreshMs == ARS_REFRESH_DEFAULT_MS);

	// Accepted with refresh 0: not expected, so no re-registration however long it stays registered
	receiveARS(ACK_REFRESH_0, sizeof(ACK_REFRESH_0));
	runMainTask(1000);
	CHECK(dmrDataServiceArsState() == DMR_ARS_STATE_REGISTERED);
	sentCount = 0;
	millis += (5 * 60 * 60000);
	runMainTask(1000);
	CHECK(countSent('A', DMR_ARS_DEVICE_REGISTRATION) == 0);
	CHECK(dmrDataServiceArsState() == DMR_ARS_STATE_REGISTERED);

	// The server asks: registers again straight away
	sentCount = 0;
	receiveARS(QUERY, sizeof(QUERY));
	runMainTask(2000);
	CHECK(countSent('A', DMR_ARS_DEVICE_REGISTRATION) == 1);

	// Switched off: deregisters from the server
	receiveARS(ACK_PLAIN, sizeof(ACK_PLAIN));
	runMainTask(1000);
	sentCount = 0;
	nonVolatileSettings.bitfieldOptions &= ~BIT_DMR_ARS;
	runMainTask(8000);
	CHECK(countSent('A', DMR_ARS_DEVICE_DEREGISTRATION) == 1);
	CHECK(dmrDataServiceArsState() == DMR_ARS_STATE_OFF);
}
#endif

#if defined(HAS_DMR_GPS_REPORTS)
#define GPS_SERVER	9990200

static int countGps(uint8_t preambles)
{
	int n = 0;

	for (int i = 0; i < sentCount; i++)
	{
		n += ((sent[i].kind == 'G') && (sent[i].seq == preambles) && (sent[i].dst == GPS_SERVER));
	}
	return n;
}

static void testGpsReports(void)
{
	reset();
	nonVolatileSettings.gpsReportDest[0] = (GPS_SERVER >> 16) & 0xFF;
	nonVolatileSettings.gpsReportDest[1] = (GPS_SERVER >> 8) & 0xFF;
	nonVolatileSettings.gpsReportDest[2] = GPS_SERVER & 0xFF;

	// Periodic, every 60 s, with preambles; nothing without a position
	nonVolatileSettings.gpsReportInterval = 60;
	runMainTask(61000);
	CHECK(countGps(DMR_DATA_SMS_PREAMBLES) == 0);
	haveLocation = true;
	runMainTask(60000);
	CHECK(countGps(DMR_DATA_SMS_PREAMBLES) == 1);
	runMainTask(60000);
	CHECK(countGps(DMR_DATA_SMS_PREAMBLES) == 2);

	// After a call of ours, straight away and without preambles; only with the flag
	sentCount = 0;
	nonVolatileSettings.gpsReportInterval = 0;
	dmrDataServiceVoiceTxEnded();
	runMainTask(2000);
	CHECK(sentCount == 0);
	nonVolatileSettings.gpsReportFlags = GPS_REPORT_FLAG_AFTER_CALL;
	dmrDataServiceVoiceTxEnded();
	runMainTask(2000);
	CHECK(countGps(0) == 1);

	// No destination: nothing
	sentCount = 0;
	memset(nonVolatileSettings.gpsReportDest, 0, 3);
	nonVolatileSettings.gpsReportInterval = 60;
	dmrDataServiceVoiceTxEnded();
	runMainTask(130000);
	CHECK(sentCount == 0);
	nonVolatileSettings.gpsReportInterval = 0;
	nonVolatileSettings.gpsReportFlags = 0;
	haveLocation = false;
}
#endif

#if defined(HAS_DMR_LRRP)
#define LRRP_SERVER	9990300

static void receiveLrrp(const char *hex)
{
	uint8_t doc[64];
	int n = strlen(hex) / 2;
	dmrBurst_t bursts[DMR_DATA_MAX_BURSTS];

	for (int i = 0; i < n; i++)
	{
		sscanf(&hex[i * 2], "%2hhx", &doc[i]);
	}

	int count = dmrDataBuildUDP(false, OUR_ID, LRRP_SERVER, DMR_UDP_PORT_LRRP, DMR_UDP_PORT_LRRP, doc, n, DT_RATE_12_DATA, 1, bursts, DMR_DATA_MAX_BURSTS);

	dmrDataRxReset();
	for (int i = 0; i < count; i++)
	{
		millis += 60;
		dmrDataServiceRxBurst(&bursts[i]);
	}
}

static int countLrrp(uint8_t docId)
{
	int n = 0;

	for (int i = 0; i < sentCount; i++)
	{
		n += ((sent[i].kind == 'L') && (sent[i].seq == docId) && (sent[i].dst == LRRP_SERVER));
	}
	return n;
}

// Requests made by RadioDesk's encoder (radiodesk.dmr.location.encode_lrrp_*)
static void testLrrp(void)
{
	reset();
	haveLocation = true;
	positionLatMinutes = 0;

	// Immediate request: one immediate response
	receiveLrrp("050922040a0b0c0d516257");
	runMainTask(3000);
	CHECK(countLrrp(0x07) == 1);

	// Periodic, every 60 s: the start response, a first report after 5 s, then one every 60 s
	sentCount = 0;
	receiveLrrp("090922041122334434313c");
	runMainTask(8000);
	CHECK(countLrrp(0x0B) == 1);
	CHECK(countLrrp(0x0D) == 1);
	runMainTask(121000);
	CHECK(countLrrp(0x0D) == 3);
	CHECK(dmrDataServiceLrrpTriggerCount() == 1);

	// Stopped: a stop response, no more reports
	sentCount = 0;
	receiveLrrp("0f06220411223344");
	runMainTask(130000);
	CHECK(countLrrp(0x11) == 1);
	CHECK(countLrrp(0x0D) == 0);
	CHECK(dmrDataServiceLrrpTriggerCount() == 0);

	// Distance 500 m checked every 30 s: the first report, then only once it has moved 500 m
	sentCount = 0;
	receiveLrrp("090922015534311e788374");
	runMainTask(8000);
	CHECK(countLrrp(0x0D) == 1);
	runMainTask(95000);
	CHECK(countLrrp(0x0D) == 1);// not moved
	positionLatMinutes = 2000;// about 370 m
	runMainTask(31000);
	CHECK(countLrrp(0x0D) == 1);
	positionLatMinutes = 3000;// about 556 m from the last report
	runMainTask(31000);
	CHECK(countLrrp(0x0D) == 2);
	CHECK(dmrDataServiceLocationNeededInMs() <= 30000);

	// An emergency trigger: refused (no emergency on this radio), nothing more running
	sentCount = 0;
	receiveLrrp("0906220166344a02");
	runMainTask(3000);
	CHECK(countLrrp(0x0B) == 1);
	CHECK(dmrDataServiceLrrpTriggerCount() == 1);// the distance one

	receiveLrrp("0f06220155");
	runMainTask(3000);
	CHECK(dmrDataServiceLrrpTriggerCount() == 0);
	CHECK(dmrDataServiceLocationNeededInMs() == UINT32_MAX);
	haveLocation = false;
}
#endif

int main(void)
{
	// Two messages from different radios before the main task runs: each gets its ACK, in order
	reset();
	receiveTMS(RADIO_A, "hello from A", 3);
	receiveTMS(RADIO_B, "hello from B", 7);
	CHECK(dmrDataServiceMessageCount() == 2);
	runMainTask(2000);
	CHECK(sentCount == 2);
	CHECK((sent[0].kind == 'T') && (sent[0].dst == RADIO_A) && (sent[0].seq == 3));
	CHECK((sent[1].kind == 'T') && (sent[1].dst == RADIO_B) && (sent[1].seq == 7));
	CHECK(melodies == 1);// one newMessage flag: rung once for both (dmrDataServiceMessageReceived gets the newest)
	CHECK(replyReadIdx == replyWriteIdx);

	// The same message again (the sender missed our ACK): ACKed again, kept once, rung once
	int before = dmrDataServiceMessageCount();
	reset();
	receiveTMS(RADIO_A, "retried", 9);
	runMainTask(2000);
	receiveTMS(RADIO_A, "retried", 9);
	runMainTask(2000);
	CHECK(dmrDataServiceMessageCount() == before + 1);
	CHECK(strcmp(dmrDataServiceMessage(0)->text, "retried") == 0);
	CHECK(sentCount == 2);
	CHECK((sent[0].kind == 'T') && (sent[0].dst == RADIO_A) && (sent[0].seq == 9));
	CHECK((sent[1].kind == 'T') && (sent[1].dst == RADIO_A) && (sent[1].seq == 9));
	CHECK(melodies == 1);

	// The retry arriving before the first ACK has even gone: two ACKs, one message
	before = dmrDataServiceMessageCount();
	reset();
	receiveTMS(RADIO_B, "quick retry", 11);
	receiveTMS(RADIO_B, "quick retry", 11);
	runMainTask(2000);
	CHECK(dmrDataServiceMessageCount() == before + 1);
	CHECK(sentCount == 2);
	CHECK((sent[0].kind == 'T') && (sent[0].dst == RADIO_B) && (sent[1].kind == 'T') && (sent[1].dst == RADIO_B));

	// Same sequence number, other text, or other sender, or after DUPLICATE_MESSAGE_MS: a new message
	before = dmrDataServiceMessageCount();
	reset();
	receiveTMS(RADIO_A, "seq 20 first", 20);
	receiveTMS(RADIO_A, "seq 20 second", 20);
	receiveTMS(RADIO_B, "seq 20 first", 20);
	runMainTask(2000);
	millis += DUPLICATE_MESSAGE_MS;
	receiveTMS(RADIO_A, "seq 20 first", 20);
	runMainTask(2000);
	CHECK(dmrDataServiceMessageCount() == DMR_DATA_INBOX_SIZE);
	CHECK(before + 4 >= DMR_DATA_INBOX_SIZE);
	CHECK(sentCount == 4);
	CHECK(melodies == 2);// once for the three before the main task ran, once for the last

	// Confirmed data from two radios: response then TMS ACK for the first, then the same for the second
	reset();
	receiveConfirmedTMS(RADIO_A, "confirmed A", 21, 2);
	receiveConfirmedTMS(RADIO_B, "confirmed B", 22, 5);
	runMainTask(3000);
	CHECK(sentCount == 4);
	CHECK((sent[0].kind == 'R') && (sent[0].dst == RADIO_A));
	CHECK((sent[1].kind == 'T') && (sent[1].dst == RADIO_A) && (sent[1].seq == 21));
	CHECK((sent[2].kind == 'R') && (sent[2].dst == RADIO_B));
	CHECK((sent[3].kind == 'T') && (sent[3].dst == RADIO_B) && (sent[3].seq == 22));

	// More than the queue holds before the main task runs: REPLY_QUEUE_SIZE - 1 are ACKed, the rest retry
	reset();
	for (int i = 0; i < REPLY_QUEUE_SIZE + 1; i++)
	{
		char text[16];

		snprintf(text, sizeof(text), "burst %d", i);
		receiveTMS(RADIO_A + i, text, 10 + i);
	}
	runMainTask(3000);
	CHECK(sentCount == REPLY_QUEUE_SIZE - 1);
	for (int i = 0; (i < sentCount) && (i < REPLY_QUEUE_SIZE - 1); i++)
	{
		CHECK((sent[i].kind == 'T') && (sent[i].dst == (uint32_t)(RADIO_A + i)) && (sent[i].seq == 10 + i));
	}

	// A reply the main task couldn't send for REPLY_QUEUE_MAX_AGE_MS is dropped, the next one still goes
	reset();
	receiveTMS(RADIO_A, "stale", 24);
	millis += REPLY_QUEUE_MAX_AGE_MS + 100;
	receiveTMS(RADIO_B, "fresh", 25);
	runMainTask(2000);
	CHECK(sentCount == 1);
	CHECK((sent[0].kind == 'T') && (sent[0].dst == RADIO_B) && (sent[0].seq == 25));

	// Across the 32 bit millisecond wraparound: still a duplicate, and the ACK isn't taken for stale
	reset();
	millis = 0xFFFFFF00u;
	receiveTMS(RADIO_B, "wrap", 26);
	runMainTask(2000);
	receiveTMS(RADIO_B, "wrap", 26);
	runMainTask(2000);
	CHECK(millis < 0x10000000u);
	CHECK(sentCount == 2);
	CHECK(melodies == 1);

#if defined(HAS_DMR_ARS)
	testARS();
#endif
#if defined(HAS_DMR_GPS_REPORTS)
	testGpsReports();
#endif
#if defined(HAS_DMR_LRRP)
	testLrrp();
#endif

	printf(failures ? "# FAILED (%d failures)\n" : "# PASSED (%d failures)\n", failures);
	return failures ? 1 : 0;
}
