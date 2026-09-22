// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
/*
 * Host unit test for the DMR data service (core/dmr/dmrDataService.c): the reply queue and retried message handling,
 * CSBK signaling both ways, our SMS with its ACK, ARS registration, LRRP answers with and without a position, the
 * 400 ms block window, and the dmr_data_api.h mailbox; ETSI IP text, the control station passthrough. Built twice: as
 * the firmware has it, and with CONFIG_DMR_TMS=0 (dmrDataServiceTestNoTms: TMS heard but neither kept nor ACKed).
 *
 *   make -C tests/host
 *
 * The service is included as source, so that its static state can be looked at, on a fake host (dmrDataEnv_t).
 * Transmissions are decoded again with core/dmr/dmrData.c
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

static uint32_t millis = 100000;
static dmrDataChannel_t ch = { .ownId = OUR_ID, .talkGroup = 9, .rmo = false, .canTransmit = true, .channelKey = 1 };
static bool channelClear = true;
static bool txAccept = true;
static bool txAutoDone = true;		// the fake stack finishes each list at once
static int events[16];

static bool haveLocation = false;
static int positionLatMinutes = 0;// the test moves the radio north by this many 1/10000 minutes
bool dmrDataServiceGetPosition(lrrpPosition_t *position)
{
	memset(position, 0, sizeof(*position));
	if (!haveLocation)
	{
		return false;
	}
	position->location.fixValid = true;
	position->location.north = true;
	position->location.latDegrees = 33;
	position->location.lonDegrees = 84;
	position->location.speedKnots = DMR_LOCATION_SPEED_UNKNOWN;
	position->location.latMinutes10000 = positionLatMinutes;
	position->utcYear = 2026;
	position->utcMonth = 10;
	position->utcDay = 8;
	return true;
}

// What went on air, decoded: 'R' ETSI response, 'T' TMS ACK, 'S' TMS message, 'L' LRRP, 'A' ARS, 'C' CSBK (seq: the
// opcode), 'I' ETSI IP text (text), 'D' ETSI defined short data text, '?' anything else
#define MAX_SENT	16
static struct { char kind; uint32_t dst; uint8_t seq; uint8_t extra; bool group; char text[64]; } sent[MAX_SENT];
static char sentText[64];
static int sentCount = 0;
static bool txOpen = false;

static bool fakeTxStart(const dmrBurst_t *bursts, int count, void *ctx)
{
	char kind = '?';
	uint32_t dst = 0;
	uint8_t seq = 0, extra = 0;

	if (!txAccept)
	{
		return false;
	}
	if ((count == 1) && (bursts[0].dataType == DT_CSBK))
	{
		kind = 'C';
		seq = bursts[0].payload[0] & 0x3F;
		extra = bursts[0].payload[3];
		dst = (bursts[0].payload[4] << 16) | (bursts[0].payload[5] << 8) | bursts[0].payload[6];
	}

	dmrDataRxReset();
	sentText[0] = '\0';
	bool group = false;
	for (int i = 0; (i < count) && (kind == '?'); i++)
	{
		if (dmrDataRxBurst(&bursts[i]) == DMR_DATA_RX_PACKET)
		{
			dmrDataUDP_t udp;
#if CONFIG_DMR_TMS
			dmrDataTMS_t tms;
#endif

			dst = dmrDataRxPacket.dst;
			group = dmrDataRxPacket.group;
			if (dmrDataRxPacket.dpf == DMR_DPF_RESPONSE)
			{
				kind = 'R';
			}
			else if (dmrDataRxPacket.dpf == DMR_DPF_DEFINED_SHORT)
			{
				kind = 'D';
				dmrDataDecodeEtsiText(&dmrDataRxPacket, sentText, sizeof(sentText));
			}
			else if (dmrDataGetUDP(&dmrDataRxPacket, &udp) && (udp.dstPort == DMR_UDP_PORT_ETSI_TEXT))
			{
				kind = 'I';
				extra = (udp.srcPort == DMR_UDP_PORT_ETSI_TEXT);
				dmrDataDecodeIPText(udp.payload, udp.length, sentText, sizeof(sentText));
			}
#if CONFIG_DMR_TMS
			else if (dmrDataGetUDP(&dmrDataRxPacket, &udp) && (udp.appPort == DMR_UDP_PORT_TMS) &&
						dmrDataDecodeTMS(udp.payload, udp.length, &tms))
			{
				kind = tms.isAck ? 'T' : 'S';
				seq = tms.seq;
			}
#endif
			else if (dmrDataGetUDP(&dmrDataRxPacket, &udp) && (udp.appPort == DMR_UDP_PORT_LRRP))
			{
				kind = 'L';// seq: the LRRP document ID, extra: the result token when there is one
				seq = udp.payload[0];
				for (int k = 2; k + 1 < udp.length; k++)
				{
					if (udp.payload[k] == 0x37)
					{
						extra = udp.payload[k + 1];
					}
				}
			}
			else if (dmrDataGetUDP(&dmrDataRxPacket, &udp) && (udp.appPort == DMR_UDP_PORT_ARS))
			{
				kind = 'A';// seq: the ARS PDU type
				seq = dmrDataDecodeARS(udp.payload, udp.length) & 0xFF;
			}
		}
	}
	dmrDataRxReset();

	if (sentCount < MAX_SENT)
	{
		sent[sentCount].kind = kind;
		sent[sentCount].dst = dst;
		sent[sentCount].seq = seq;
		sent[sentCount].extra = extra;
		sent[sentCount].group = group;
		snprintf(sent[sentCount].text, sizeof(sent[sentCount].text), "%s", sentText);
		sentCount++;
	}
	txOpen = true;
	return true;
}

static bool fakeClear(uint32_t nowMs, void *ctx) { return channelClear; }
static bool fakeInRxGroup(uint32_t tg, void *ctx) { return tg == 91; }
static void fakeNotify(const dmrDataEvent_t *e, void *ctx) { if (e->kind < 16) events[e->kind]++; }

static const dmrDataEnv_t fakeEnv = { .txStart = fakeTxStart, .channelClear = fakeClear, .inRxGroup = fakeInRxGroup,
									  .notify = fakeNotify };

// The radio task: ticks every 10 ms for a while, everything queued goes out
static void runMainTask(int ms)
{
	for (int t = 0; t < ms; t += 10)
	{
		millis += 10;
		dmrDataServiceTick(millis, &ch);
		if (txOpen && txAutoDone)
		{
			txOpen = false;
			dmrDataServiceTxDone(0);
		}
	}
}

static void rxBursts(const dmrBurst_t *bursts, int count)
{
	for (int i = 0; i < count; i++)
	{
		millis += 60;
		dmrDataServiceRxBurst(&bursts[i], millis);
	}
}

#if CONFIG_DMR_TMS
// A TMS message from src to us, burst by burst (seq: dmrDataBuildTMS sends 5 bits)
static void receiveTMS(uint32_t src, const char *text, uint8_t seq)
{
	dmrBurst_t bursts[DMR_DATA_MAX_BURSTS];
	int count = dmrDataBuildTMS(false, OUR_ID, src, text, seq, true, DT_RATE_12_DATA, 1, bursts, DMR_DATA_MAX_BURSTS);

	CHECK(count > 0);
	rxBursts(bursts, count);
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
	now = millis;
	handlePacket(&packet);
	dmrDataRxReset();
}
#endif

static void receiveCSBK(uint8_t opcode, uint8_t fid, uint8_t octet3, uint32_t target, uint32_t source)
{
	uint8_t c[10] = { (uint8_t)(0x80 | opcode), fid, 0, octet3, (uint8_t)(target >> 16), (uint8_t)(target >> 8),
					  (uint8_t)target, (uint8_t)(source >> 16), (uint8_t)(source >> 8), (uint8_t)source };
	dmrBurst_t b;

	dmrDataBuildCSBK(c, &b);
	b.flags = 0;
	millis += 60;
	dmrDataServiceRxBurst(&b, millis);
}

static void reset(void)
{
	runMainTask(6000);// anything left over goes, or ages out
	sentCount = 0;
	memset(events, 0, sizeof(events));
}

#define ARS_SERVER	9990100

// The server's ARS PDU to us, over the air
static void receiveARS(const uint8_t *pdu, int length)
{
	dmrBurst_t bursts[DMR_DATA_MAX_BURSTS];
	int count = dmrDataBuildUDP(false, OUR_ID, ARS_SERVER, DMR_UDP_PORT_ARS, DMR_UDP_PORT_ARS, pdu, length, DT_RATE_12_DATA, 1,
								bursts, DMR_DATA_MAX_BURSTS);

	CHECK(count > 0);
	rxBursts(bursts, count);
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
	settings.arsServer = ARS_SERVER;
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
	settings.arsServer = 0;
	runMainTask(8000);
	CHECK(countSent('A', DMR_ARS_DEVICE_DEREGISTRATION) == 1);
	CHECK(dmrDataServiceArsState() == DMR_ARS_STATE_OFF);
}


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

	rxBursts(bursts, count);
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
	settings.hasGps = true;
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

	// No position source (the DM-1701): an immediate request says "no fix", a triggered start is refused with it
	settings.hasGps = false;
	sentCount = 0;
	receiveLrrp("050922040a0b0c0d516257");
	runMainTask(3000);
	CHECK(countLrrp(0x07) == 1);
	CHECK(sent[0].extra == LRRP_RESULT_NO_FIX);
	receiveLrrp("090922041122334434313c");
	runMainTask(8000);
	CHECK(countLrrp(0x0B) == 1);
	CHECK(sent[1].extra == LRRP_RESULT_NO_FIX);
	CHECK(dmrDataServiceLrrpTriggerCount() == 0);
	CHECK(stats.lrrpNoFix == 2);

	// LRRP answers switched off: nothing
	settings.lrrpReply = false;
	sentCount = 0;
	receiveLrrp("050922040a0b0c0d516257");
	runMainTask(3000);
	CHECK(sentCount == 0);
	settings.lrrpReply = true;
}


static int countCsbk(uint8_t opcode, uint32_t dst)
{
	int n = 0;

	for (int i = 0; i < sentCount; i++)
	{
		n += ((sent[i].kind == 'C') && (sent[i].seq == opcode) && (sent[i].dst == dst));
	}
	return n;
}

// Call alert and radio check, received and sent
static void testSignaling(void)
{
	reset();
	settings.callAlertAck = true;
	settings.radioCheckReply = false;

	// A call alert to us: rings (event) and is acked to the caller; the caller's retry rings no more
	receiveCSBK(DMR_CSBKO_CALL_ALERT, DMR_FID_ETSI, 0, OUR_ID, RADIO_A);
	runMainTask(500);
	CHECK(events[DMR_DATA_EV_CALL_ALERT] == 1);
	CHECK(sentCount == 1);
	CHECK((sent[0].kind == 'C') && (sent[0].seq == DMR_CSBKO_CALL_ALERT_ACK) && (sent[0].dst == RADIO_A));
	receiveCSBK(DMR_CSBKO_CALL_ALERT, DMR_FID_ETSI, 0, OUR_ID, RADIO_A);
	runMainTask(500);
	CHECK(events[DMR_DATA_EV_CALL_ALERT] == 1);
	CHECK(sentCount == 2);
	CHECK(stats.callAlertsReceived == 2);

	// To someone else: nothing
	receiveCSBK(DMR_CSBKO_CALL_ALERT, DMR_FID_ETSI, 0, RADIO_B, RADIO_A);
	runMainTask(500);
	CHECK(sentCount == 2);

	// A radio check with the reply off: counted, not answered; on: answered (the request echoed, octet 3 cleared)
	receiveCSBK(DMR_CSBKO_RADIO_CHECK, DMR_FID_MOTOROLA, 0x80, OUR_ID, RADIO_B);
	runMainTask(500);
	CHECK(sentCount == 2);
	CHECK(stats.radioChecksReceived == 1);
	settings.radioCheckReply = true;
	receiveCSBK(DMR_CSBKO_RADIO_CHECK, DMR_FID_MOTOROLA, 0x80, OUR_ID, RADIO_B);
	runMainTask(500);
	CHECK(sentCount == 3);
	CHECK((sent[2].kind == 'C') && (sent[2].seq == DMR_CSBKO_RADIO_CHECK) && (sent[2].extra == 0x00) &&
		  (sent[2].dst == OUR_ID));
	CHECK(events[DMR_DATA_EV_RADIO_CHECK_ANSWERED] == 1);
	// A bad CRC: dropped
	{
		uint8_t c[10] = { 0x80 | DMR_CSBKO_RADIO_CHECK, DMR_FID_MOTOROLA, 0, 0x80, 0, 0x03, 0xE8, 0, 0x07, 0xD2 };
		dmrBurst_t b;
		uint32_t errors = stats.rxCrcErrors;

		dmrDataBuildCSBK(c, &b);
		b.payload[11] ^= 1;
		dmrDataServiceRxBurst(&b, millis);
		runMainTask(500);
		CHECK(sentCount == 3);
		CHECK(stats.rxCrcErrors == errors + 1);
		CHECK(stats.rxCrcFlagMismatch >= 1);// the modem said it was fine
	}

	// Our radio check: one CSBK, answered by the radio checked
	reset();
	CHECK(dmr_signal_radio_check(RADIO_A));
	CHECK(!dmr_signal_call_alert(RADIO_B));// one at a time
	runMainTask(100);
	CHECK(countCsbk(DMR_CSBKO_RADIO_CHECK, RADIO_A) == 1);
	CHECK(sent[0].extra == 0x80);
	CHECK(dmr_signal_state(NULL, NULL) == DMR_REQ_SENDING);
	receiveCSBK(DMR_CSBKO_RADIO_CHECK, DMR_FID_MOTOROLA, 0x00, RADIO_A, OUR_ID);
	runMainTask(100);
	uint8_t kind;
	uint32_t dst;
	CHECK(dmr_signal_state(&kind, &dst) == DMR_REQ_OK);
	CHECK((kind == DMR_DATA_EV_RADIO_CHECK_OK) && (dst == RADIO_A));
	CHECK(events[DMR_DATA_EV_RADIO_CHECK_OK] == 1);
	CHECK(dmr_data_last_event().kind == DMR_DATA_EV_RADIO_CHECK_OK);

	// Our call alert with no answer: three tries, then failed
	reset();
	CHECK(dmr_signal_call_alert(RADIO_B));
	runMainTask(10000);
	CHECK(countCsbk(DMR_CSBKO_CALL_ALERT, RADIO_B) == SIGNAL_TRIES);
	CHECK(dmr_signal_state(NULL, NULL) == DMR_REQ_FAILED);
	CHECK(events[DMR_DATA_EV_CALL_ALERT_FAILED] == 1);

	// ... and acked on the second try (the ack: to us from the radio alerted)
	reset();
	CHECK(dmr_signal_call_alert(RADIO_B));
	runMainTask(SIGNAL_ANSWER_MS + 200);
	CHECK(countCsbk(DMR_CSBKO_CALL_ALERT, RADIO_B) == 2);
	receiveCSBK(DMR_CSBKO_CALL_ALERT_ACK, DMR_FID_ETSI, 0, OUR_ID, RADIO_B);
	runMainTask(5000);
	CHECK(countCsbk(DMR_CSBKO_CALL_ALERT, RADIO_B) == 2);
	CHECK(dmr_signal_state(NULL, NULL) == DMR_REQ_OK);
	CHECK(events[DMR_DATA_EV_CALL_ALERT_ACKED] == 1);
}

#if CONFIG_DMR_TMS
// Our text messages: to a radio with an ACK, to a group, and unanswered
static void testOurSms(void)
{
	reset();
	CHECK(sms_send(RADIO_A, false, "hello A", true, DMR_SMS_FORMAT_TMS));
	CHECK(!sms_send(RADIO_B, false, "busy", true, DMR_SMS_FORMAT_TMS));
	runMainTask(200);
	CHECK((sentCount == 1) && (sent[0].kind == 'S') && (sent[0].dst == RADIO_A));
	CHECK(sms_send_state() == DMR_REQ_SENDING);
	uint8_t seq = sent[0].seq;
	// The ACK from A, as A's radio sends it
	{
		dmrBurst_t bursts[DMR_DATA_MAX_BURSTS];
		int count = dmrDataBuildTMSAck(OUR_ID, RADIO_A, seq, 1, bursts, DMR_DATA_MAX_BURSTS);

		rxBursts(bursts, count);
	}
	runMainTask(200);
	CHECK(sms_send_state() == DMR_REQ_OK);
	CHECK(events[DMR_DATA_EV_SMS_SENT] == 1);
	CHECK(stats.smsAcked == 1);

	// To a group: sent, no ACK expected
	reset();
	CHECK(sms_send(91, true, "to the group", true, DMR_SMS_FORMAT_TMS));
	runMainTask(200);
	CHECK((sentCount == 1) && (sent[0].kind == 'S') && (sent[0].dst == 91));
	CHECK(sms_send_state() == DMR_REQ_OK);

	// No ACK: sent SMS_TRIES times with the same sequence number, then failed
	reset();
	CHECK(sms_send(RADIO_B, false, "anyone?", true, DMR_SMS_FORMAT_TMS));
	runMainTask(SMS_TRIES * SMS_ACK_MS + 1000);
	CHECK(sentCount == SMS_TRIES);
	CHECK((sent[0].seq == sent[1].seq) && (sent[1].kind == 'S'));
	CHECK(sms_send_state() == DMR_REQ_FAILED);
	CHECK(events[DMR_DATA_EV_SMS_FAILED] == 1);

	// The channel never clears: given up after START_TIMEOUT_MS each try, then failed
	reset();
	channelClear = false;
	CHECK(sms_send(RADIO_B, false, "busy channel", false, DMR_SMS_FORMAT_TMS));
	runMainTask(START_TIMEOUT_MS + 2000);
	CHECK(sentCount == 0);
	CHECK(sms_send_state() == DMR_REQ_FAILED);
	CHECK(stats.txGaveUp == 1);
	channelClear = true;

	// The stack drops the list (no repeater): an unacknowledged message fails
	reset();
	txAutoDone = false;
	CHECK(sms_send(RADIO_B, false, "dropped", false, DMR_SMS_FORMAT_TMS));
	runMainTask(100);
	CHECK(dmrDataServiceIsBusy());
	dmrDataServiceTxDone(2);
	txOpen = false;
	runMainTask(100);
	CHECK(!dmrDataServiceIsBusy());
	CHECK(sms_send_state() == DMR_REQ_FAILED);
	CHECK(stats.txFailed == 1);
	txAutoDone = true;

	// The inbox through the API
	reset();
	receiveTMS(RADIO_A, "inbox one", 30);
	runMainTask(500);
	dmrDataMessage_t m;
	CHECK(sms_inbox_get(0, &m) && (strcmp(m.text, "inbox one") == 0) && m.unread && (m.src == RADIO_A));
	int unread = sms_inbox_unread();
	uint32_t serial = dmr_data_serial();
	sms_inbox_mark_read(0);
	CHECK(sms_inbox_unread() == unread - 1);
	CHECK(dmr_data_serial() != serial);
	CHECK(!sms_inbox_get(DMR_DATA_INBOX_SIZE, &m));

	// Delete (the UI's message actions): from the middle of a full, wrapped ring, then the newest; a new message
	// goes in front of the rest
	reset();
	static const char *const texts[] = { "m0", "m1", "m2", "m3", "m4" };
	for (int i = 0; i < 5; i++)
	{
		receiveTMS(RADIO_A, texts[i], (uint8_t)(40 + i));
		runMainTask(500);
	}
	CHECK(sms_inbox_count() == DMR_DATA_INBOX_SIZE);	// m4 m3 m2 m1 (m0 dropped)
	serial = dmr_data_serial();
	CHECK(sms_inbox_delete(1));				// m3
	CHECK(dmr_data_serial() != serial);
	CHECK(sms_inbox_count() == 3);
	const char *const after[] = { "m4", "m2", "m1" };
	for (int i = 0; i < 3; i++)
	{
		CHECK(sms_inbox_get(i, &m) && (strcmp(m.text, after[i]) == 0));
	}
	CHECK(sms_inbox_delete(0));				// m4
	CHECK(!sms_inbox_delete(2) && !sms_inbox_delete(-1));
	receiveTMS(RADIO_A, "m5", 50);
	runMainTask(500);
	const char *const now3[] = { "m5", "m2", "m1" };
	CHECK(sms_inbox_count() == 3);
	for (int i = 0; i < 3; i++)
	{
		CHECK(sms_inbox_get(i, &m) && (strcmp(m.text, now3[i]) == 0));
	}
	while (sms_inbox_count() > 0)
	{
		CHECK(sms_inbox_delete(0));
	}
	CHECK(!sms_inbox_get(0, &m) && sms_inbox_unread() == 0);
	receiveTMS(RADIO_A, "m6", 51);
	runMainTask(500);
	CHECK(sms_inbox_count() == 1 && sms_inbox_get(0, &m) && (strcmp(m.text, "m6") == 0));
}

// Data blocks long after their header are noise (gotcha 5.4); group data to the listen list is ours
static void testBlockWindow(void)
{
	dmrBurst_t bursts[DMR_DATA_MAX_BURSTS];
	int count = dmrDataBuildTMS(false, OUR_ID, RADIO_A, "late blocks", 31, true, DT_RATE_12_DATA, 0, bursts, DMR_DATA_MAX_BURSTS);
	int before = dmrDataServiceMessageCount() < DMR_DATA_INBOX_SIZE ? dmrDataServiceMessageCount() : 0;
	uint32_t received = stats.smsReceived;

	reset();
	millis += 60;
	dmrDataServiceRxBurst(&bursts[0], millis);
	millis += 500;
	for (int i = 1; i < count; i++)
	{
		dmrDataServiceRxBurst(&bursts[i], millis);
		millis += 60;
	}
	runMainTask(500);
	CHECK(stats.smsReceived == received);
	CHECK(stats.rxBlocksStale >= 1);
	(void)before;

	// Group 91 is in the listen list: kept, not ACKed (group)
	count = dmrDataBuildTMS(true, 91, RADIO_A, "to 91", 32, false, DT_RATE_34_DATA, 1, bursts, DMR_DATA_MAX_BURSTS);
	rxBursts(bursts, count);
	runMainTask(500);
	CHECK(stats.smsReceived == received + 1);
	CHECK(sentCount == 0);
	// Group 92 isn't
	count = dmrDataBuildTMS(true, 92, RADIO_A, "to 92", 33, false, DT_RATE_1_DATA, 1, bursts, DMR_DATA_MAX_BURSTS);
	rxBursts(bursts, count);
	runMainTask(500);
	CHECK(stats.smsReceived == received + 1);
}

// Not a channel we may send on (FM, RX only, hotspot): requests fail at once, replies aren't sent
static void testNotOnChannel(void)
{
	reset();
	ch.canTransmit = false;
	CHECK(dmr_signal_radio_check(RADIO_A));// queued in the mailbox ...
	runMainTask(100);
	CHECK(dmr_signal_state(NULL, NULL) == DMR_REQ_FAILED);// ... and failed at the next tick
	receiveTMS(RADIO_B, "no reply here", 3);
	runMainTask(6000);
	CHECK(sentCount == 0);
	ch.canTransmit = true;
}

// The reply queue and duplicate handling, with TMS messages
static void testTmsReplies(void)
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
	CHECK(events[DMR_DATA_EV_SMS_RECEIVED] == 2);
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
	CHECK(events[DMR_DATA_EV_SMS_RECEIVED] == 1);

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
	CHECK(events[DMR_DATA_EV_SMS_RECEIVED] == 4);

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
	CHECK(events[DMR_DATA_EV_SMS_RECEIVED] == 1);
}
#endif	// CONFIG_DMR_TMS

// A built packet handed to the service whole (as the reassembly gives it), optionally as confirmed data
static void receiveBuilt(const dmrBurst_t *bursts, int count, bool confirmed, uint8_t sendSeq)
{
	static dmrDataPacket_t packet;
	dmrDataRxResult_t result = DMR_DATA_RX_NONE;

	dmrDataRxReset();
	for (int i = 0; (i < count) && (result != DMR_DATA_RX_PACKET); i++)
	{
		result = dmrDataRxBurst(&bursts[i]);
	}
	CHECK(result == DMR_DATA_RX_PACKET);
	packet = dmrDataRxPacket;
	if (confirmed)
	{
		packet.dpf = DMR_DPF_CONFIRMED;
		packet.responseRequested = true;
		packet.sendSeq = sendSeq;
	}
	millis += 60;
	now = millis;
	handlePacket(&packet);
	dmrDataRxReset();
}

static void receiveIPText(bool group, uint32_t dst, uint32_t src, const char *text, bool confirmed)
{
	dmrBurst_t bursts[DMR_DATA_MAX_BURSTS];
	int count = dmrDataBuildIPText(group, dst, src, text, DT_RATE_12_DATA, 1, bursts, DMR_DATA_MAX_BURSTS);

	CHECK(count > 0);
	receiveBuilt(bursts, count, confirmed, 3);
}

// ETSI IP text (TS 102 361-3 5.7.1): the default format, sent and received
static void testIpText(void)
{
	dmrDataMessage_t m;
	dmrBurst_t bursts[DMR_DATA_MAX_BURSTS];
	dmrDataUDP_t udp;

	// The default (docs/spec/dmr-data.md 8)
	CHECK(settings.smsFormat == DMR_SMS_FORMAT_ETSI_IP);

	// The builder: UTF-16BE, UDP 5016 to 5016, full IPv4 header (SAP 4), unconfirmed
	int count = dmrDataBuildIPText(false, RADIO_A, OUR_ID, "Hi\xE9", DT_RATE_12_DATA, 0, bursts, DMR_DATA_MAX_BURSTS);
	dmrDataRxResult_t r = DMR_DATA_RX_NONE;

	dmrDataRxReset();
	for (int i = 0; (i < count) && (r != DMR_DATA_RX_PACKET); i++)
	{
		r = dmrDataRxBurst(&bursts[i]);
	}
	CHECK((r == DMR_DATA_RX_PACKET) && (dmrDataRxPacket.sap == DMR_SAP_IP) && (dmrDataRxPacket.dpf == DMR_DPF_UNCONFIRMED));
	CHECK(dmrDataGetUDP(&dmrDataRxPacket, &udp) && (udp.srcPort == 5016) && (udp.dstPort == 5016) && (udp.length == 6));
	CHECK((udp.payload[0] == 0x00) && (udp.payload[1] == 'H') && (udp.payload[4] == 0x00) && (udp.payload[5] == 0xE9));
	dmrDataRxReset();
	CHECK(dmrDataBuildIPText(false, RADIO_A, OUR_ID, "", DT_RATE_12_DATA, 0, bursts, DMR_DATA_MAX_BURSTS) == 0);

	// The decoder: big endian, a byte order mark either way, characters outside Latin-1
	char text[32];
	static const uint8_t be[] = { 0x00, 'O', 0x00, 'K' };
	static const uint8_t bomLe[] = { 0xFF, 0xFE, 'l', 0x00, 'e', 0x00 };
	static const uint8_t bomBe[] = { 0xFE, 0xFF, 0x00, 'b', 0x04, 0x10 };
	CHECK(dmrDataDecodeIPText(be, sizeof(be), text, sizeof(text)) && (strcmp(text, "OK") == 0));
	CHECK(dmrDataDecodeIPText(bomLe, sizeof(bomLe), text, sizeof(text)) && (strcmp(text, "le") == 0));
	CHECK(dmrDataDecodeIPText(bomBe, sizeof(bomBe), text, sizeof(text)) && (strcmp(text, "b?") == 0));
	CHECK(!dmrDataDecodeIPText(be, 1, text, sizeof(text)));

	// Ours, in the default format: one unconfirmed packet, done once on air (no ACK of its own)
	reset();
	CHECK(sms_send(RADIO_A, false, "hello ETSI", true, DMR_SMS_FORMAT_DEFAULT));
	runMainTask(300);
	CHECK((sentCount == 1) && (sent[0].kind == 'I') && (sent[0].dst == RADIO_A) && sent[0].extra &&
		  (strcmp(sent[0].text, "hello ETSI") == 0));
	CHECK(sms_send_state() == DMR_REQ_OK);
	CHECK((events[DMR_DATA_EV_SMS_SENT] == 1) && (stats.ipTextSent >= 1));
	runMainTask(SMS_ACK_MS + 1000);
	CHECK(sentCount == 1);// never sent again

	// To a group
	reset();
	CHECK(sms_send(91, true, "group ETSI", false, DMR_SMS_FORMAT_ETSI_IP));
	runMainTask(300);
	CHECK((sentCount == 1) && (sent[0].kind == 'I') && (sent[0].dst == 91) && sent[0].group);

	// Received: into the inbox in its format, nothing sent back
	reset();
	uint32_t received = stats.ipTextReceived;
	receiveIPText(false, OUR_ID, RADIO_B, "from B", false);
	runMainTask(2000);
	CHECK(sms_inbox_get(0, &m) && (strcmp(m.text, "from B") == 0) && (m.format == DMR_SMS_FORMAT_ETSI_IP) &&
		  (m.src == RADIO_B) && m.unread);
	CHECK((sentCount == 0) && (stats.ipTextReceived == received + 1) && (events[DMR_DATA_EV_SMS_RECEIVED] == 1));

	// The same again within the duplicate window: kept once
	receiveIPText(false, OUR_ID, RADIO_B, "from B", false);
	runMainTask(500);
	CHECK(stats.ipTextReceived == received + 1);

	// Confirmed: the ETSI response, and kept
	reset();
	receiveIPText(false, OUR_ID, RADIO_A, "confirmed IP text", true);
	runMainTask(2000);
	CHECK((sentCount == 1) && (sent[0].kind == 'R') && (sent[0].dst == RADIO_A));
	CHECK(sms_inbox_get(0, &m) && (strcmp(m.text, "confirmed IP text") == 0));

	// To a group we listen to: kept; to one we don't: not
	reset();
	receiveIPText(true, 91, RADIO_A, "to 91 in IP", false);
	receiveIPText(true, 92, RADIO_A, "to 92 in IP", false);
	runMainTask(500);
	CHECK(sms_inbox_get(0, &m) && (strcmp(m.text, "to 91 in IP") == 0) && m.group && (sentCount == 0));

	// A stored TMS default: sent as TMS where it's built, else as ETSI IP text; an explicit TMS refused without it
	reset();
	settings.smsFormat = DMR_SMS_FORMAT_TMS;
	CHECK(sms_send(RADIO_A, false, "stored default", false, DMR_SMS_FORMAT_DEFAULT));
	runMainTask(300);
	CHECK((sentCount == 1) && (sent[0].kind == (CONFIG_DMR_TMS ? 'S' : 'I')));
	CHECK(sms_send(RADIO_A, false, "explicit", false, DMR_SMS_FORMAT_TMS) == (bool)CONFIG_DMR_TMS);
	settings.smsFormat = DMR_SMS_FORMAT_ETSI_IP;
	runMainTask(2000);
}

// Control station passthrough (docs/spec/usb-network.md 5): IP based packets to us are the PC's; the link layer stays
static void testPassthrough(void)
{
	dmrDataMessage_t m;

	// A running trigger and a queued request are dropped when the PC takes over
	reset();
	settings.hasGps = true;
	haveLocation = true;
	receiveLrrp("090922041122334434313c");
	runMainTask(8000);
	CHECK(dmrDataServiceLrrpTriggerCount() == 1);
	dmrDataServiceSetPassthrough(true);
	CHECK(dmrDataServicePassthrough() == (bool)CONFIG_DMR_CONTROL_STATION);
	CHECK(dmrDataServiceLrrpTriggerCount() == 0);

	// LRRP requests: not answered
	sentCount = 0;
	uint32_t yielded = stats.ptYielded;
	receiveLrrp("050922040a0b0c0d516257");
	runMainTask(70000);
	CHECK(countLrrp(0x07) == 0);
	CHECK(countLrrp(0x0D) == 0);
	CHECK(stats.ptYielded == yielded + 1);

	// IP text to us: not kept, nothing sent; confirmed: the ETSI response still goes (the PC can't send it)
	int count = sms_inbox_count();
	uint32_t received = stats.smsReceived;
	reset();
	receiveIPText(false, OUR_ID, RADIO_A, "for the PC", false);
	receiveIPText(false, OUR_ID, RADIO_B, "for the PC, confirmed", true);
	runMainTask(3000);
	CHECK((stats.smsReceived == received) && (sms_inbox_count() == count));
	CHECK((sentCount == 1) && (sent[0].kind == 'R') && (sent[0].dst == RADIO_B));

	// ETSI defined short data text has no IP header: still the radio's (kept, response sent)
	reset();
	{
		dmrBurst_t bursts[DMR_DATA_MAX_BURSTS];
		int n = dmrDataBuildEtsiText(false, OUR_ID, RADIO_A, "short data", true, 1, bursts, DMR_DATA_MAX_BURSTS);

		receiveBuilt(bursts, n, false, 0);
	}
	runMainTask(2000);
	CHECK(sms_inbox_get(0, &m) && (strcmp(m.text, "short data") == 0) && (m.format == DMR_SMS_FORMAT_ETSI));
	CHECK((sentCount == 1) && (sent[0].kind == 'R'));

	// CSBK signaling stays: a call alert is acknowledged
	reset();
	receiveCSBK(DMR_CSBKO_CALL_ALERT, DMR_FID_ETSI, 0, OUR_ID, RADIO_A);
	runMainTask(500);
	CHECK((sentCount == 1) && (sent[0].kind == 'C') && (sent[0].seq == DMR_CSBKO_CALL_ALERT_ACK));

	// Our own ARS registration: suppressed, shown as off
	reset();
	settings.arsServer = ARS_SERVER;
	runMainTask(30000);
	CHECK(countSent('A', DMR_ARS_DEVICE_REGISTRATION) == 0);
	CHECK(dmrDataServiceArsState() == DMR_ARS_STATE_OFF);
	CHECK(!dmrDataServicePowerOff());

#if CONFIG_DMR_TMS
	// TMS to us: neither kept nor ACKed
	reset();
	{
		dmrBurst_t bursts[DMR_DATA_MAX_BURSTS];
		int n = dmrDataBuildTMS(false, OUR_ID, RADIO_A, "TMS for the PC", 12, true, DT_RATE_12_DATA, 1, bursts,
								DMR_DATA_MAX_BURSTS);

		rxBursts(bursts, n);
	}
	runMainTask(3000);
	CHECK((sentCount == 0) && (stats.smsReceived == received + 1));

	// Our own message's ACK is still noted (it sends nothing)
	reset();
	CHECK(sms_send(RADIO_A, false, "ours", true, DMR_SMS_FORMAT_TMS));
	runMainTask(300);
	CHECK((sentCount == 1) && (sent[0].kind == 'S'));
	{
		dmrBurst_t bursts[DMR_DATA_MAX_BURSTS];
		int n = dmrDataBuildTMSAck(OUR_ID, RADIO_A, sent[0].seq, 1, bursts, DMR_DATA_MAX_BURSTS);

		rxBursts(bursts, n);
	}
	runMainTask(300);
	CHECK(sms_send_state() == DMR_REQ_OK);
#endif

	// Off again: the radio's services are back (the registration after the settle time)
	dmrDataServiceSetPassthrough(false);
	sentCount = 0;
	runMainTask(10000);
	CHECK(countSent('A', DMR_ARS_DEVICE_REGISTRATION) == 1);
	reset();
	receiveIPText(false, OUR_ID, RADIO_A, "back to the radio", false);
	runMainTask(500);
	CHECK(sms_inbox_get(0, &m) && (strcmp(m.text, "back to the radio") == 0));
	settings.arsServer = 0;
	settings.hasGps = false;
	haveLocation = false;
	runMainTask(10000);
}

#if !CONFIG_DMR_TMS
// Motorola TMS left out (CONFIG_DMR_TMS=0): a TMS heard is neither decoded, kept nor ACKed; confirmed data still gets
// its ETSI response (docs/spec/dmr-data.md 5.1)
static void testTmsOff(void)
{
	static const uint8_t tms[] = { 0x00, 0x08, 0xE0, 0x00, 0x85, 0x04, 'h', 0x00, 'i', 0x00 };
	dmrBurst_t bursts[DMR_DATA_MAX_BURSTS];
	int n = dmrDataBuildUDP(false, OUR_ID, RADIO_A, DMR_UDP_PORT_TMS, DMR_UDP_PORT_TMS, tms, sizeof(tms), DT_RATE_12_DATA,
							1, bursts, DMR_DATA_MAX_BURSTS);
	uint32_t received = stats.smsReceived;

	reset();
	rxBursts(bursts, n);
	runMainTask(3000);
	CHECK((sentCount == 0) && (stats.smsReceived == received) && (stats.tmsAcksSent == 0));
	receiveBuilt(bursts, n, true, 6);
	runMainTask(3000);
	CHECK((sentCount == 1) && (sent[0].kind == 'R') && (stats.smsReceived == received));
	CHECK(!sms_send(RADIO_A, false, "no TMS here", true, DMR_SMS_FORMAT_TMS));
}
#endif

int main(void)
{
	dmrDataServiceInit(&fakeEnv);

#if CONFIG_DMR_TMS
	testTmsReplies();
#endif
	testARS();
	testLrrp();
	testSignaling();
#if CONFIG_DMR_TMS
	testOurSms();
	testBlockWindow();
	testNotOnChannel();
#endif
	testIpText();
	testPassthrough();
#if !CONFIG_DMR_TMS
	testTmsOff();
#endif

	printf(failures ? "# FAILED (%d failures)\n" : "# PASSED (%d failures)\n", failures);
	return failures ? 1 : 0;
}
