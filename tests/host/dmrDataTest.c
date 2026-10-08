// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
/*
 * Host unit test for the DMR packet data layer (functions/dmrData.c)
 *
 *   make -C tests/host
 *
 * Prints the generated bursts as "<dataType> <hex payload>" lines so that an external decoder
 * (e.g. check_with_radiodesk.py) can verify them independently.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "dmr/dmrData.h"
#include "hotspot/dmrDataFrame.h"
#include "fuzz.h"

static int failures = 0;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static void dumpBursts(const char *name, const dmrBurst_t *bursts, int count)
{
	printf("# %s\n", name);
	for (int i = 0; i < count; i++)
	{
		printf("%d ", bursts[i].dataType);
		for (int j = 0; j < bursts[i].length; j++)
		{
			printf("%02x", bursts[i].payload[j]);
		}
		printf("\n");
	}
}

// CRC-CCITT of a 10 byte header into its last 2 bytes (ETSI TS 102 361-1 B.3.8, before the mask)
static void addHeaderCRC(uint8_t h[12])
{
	uint16_t crc = 0;

	for (int i = 0; i < 10; i++)
	{
		crc ^= h[i] << 8;
		for (int b = 0; b < 8; b++)
		{
			crc = (crc & 0x8000) ? ((crc << 1) ^ 0x1021) : (crc << 1);
		}
	}
	crc = ~crc;
	h[10] = crc >> 8;
	h[11] = crc & 0xFF;
}

static dmrDataRxResult_t feed(const dmrBurst_t *bursts, int count)
{
	dmrDataRxResult_t result = DMR_DATA_RX_NONE;

	dmrDataRxReset();
	for (int i = 0; i < count; i++)
	{
		result = dmrDataRxBurst(&bursts[i]);
		if ((result == DMR_DATA_RX_PACKET) || (result == DMR_DATA_RX_ERROR))
		{
			CHECK(i == (count - 1));
			break;
		}
	}
	return result;
}

static void testCRC32(void)
{
	// Packet from the RadioDesk test vectors (fec.crc32_dmr): CRC of "123456789" style data is checked against
	// the python implementation in check_with_radiodesk.py, here only the self consistency is checked.
	uint8_t data[8] = { 0x45, 0x00, 0x00, 0x2A, 0x00, 0x01, 0x00, 0x00 };
	uint32_t a = dmrDataCRC32(data, 8);
	data[0] ^= 1;
	CHECK(a != dmrDataCRC32(data, 8));
}

static void testTMSRoundTrip(bool group, int preambles)
{
	dmrBurst_t bursts[DMR_DATA_MAX_BURSTS];
	const char *text = "Hello from TestText, this is a longer message to use a few blocks";
	int n = dmrDataBuildTMS(group, 235, 3141592, text, 5, false, DT_RATE_12_DATA, preambles, bursts, DMR_DATA_MAX_BURSTS);

	CHECK(n > (preambles + 1));
	CHECK(bursts[preambles].dataType == DT_DATA_HEADER);
	for (int i = 0; i < preambles; i++)
	{
		CHECK(bursts[i].dataType == DT_CSBK);
		CHECK(bursts[i].payload[3] == (n - 1 - i));
		CHECK(dmrDataRxBurst(&bursts[i]) == DMR_DATA_RX_NONE);// preambles are swallowed
	}

	CHECK(feed(bursts, n) == DMR_DATA_RX_PACKET);
	CHECK(dmrDataRxPacket.src == 3141592);
	CHECK(dmrDataRxPacket.dst == 235);
	CHECK(dmrDataRxPacket.group == group);
	CHECK(dmrDataRxPacket.dpf == DMR_DPF_UNCONFIRMED);
	CHECK(dmrDataRxPacket.sap == DMR_SAP_IP);

	dmrDataUDP_t udp;
	CHECK(dmrDataGetUDP(&dmrDataRxPacket, &udp));
	CHECK(udp.appPort == DMR_UDP_PORT_TMS);
	CHECK(udp.length == (int)(6 + (2 * strlen(text))));

	dmrDataTMS_t tms;
	CHECK(dmrDataDecodeTMS(udp.payload, udp.length, &tms));
	CHECK(!tms.isAck);
	CHECK(tms.seq == 5);
	CHECK(strcmp(tms.text, text) == 0);

	// A corrupted block must fail the packet CRC
	bursts[n - 2].payload[3] ^= 0x10;
	CHECK(feed(bursts, n) == DMR_DATA_RX_ERROR);
	bursts[n - 2].payload[3] ^= 0x10;

	// A corrupted header must fail the header CRC
	bursts[preambles].payload[4] ^= 0x01;
	CHECK(dmrDataRxBurst(&bursts[preambles]) == DMR_DATA_RX_ERROR);
	bursts[preambles].payload[4] ^= 0x01;

	dumpBursts(group ? "tms-group" : "tms-private", bursts, n);
}

static void testPadding(uint8_t blockType)
{
	// Every user data length from 1 to a full packet must round trip, which exercises all the pad octet counts
	dmrBurst_t bursts[DMR_DATA_MAX_BURSTS];
	uint8_t data[DMR_DATA_MAX_PACKET];
	int blockLen = dmrDataBlockLength(blockType);
	int longest = 0;

	for (int i = 0; i < (int)sizeof(data); i++)
	{
		data[i] = rand();
	}

	for (int len = 1; len <= (int)sizeof(data); len++)
	{
		int n = dmrDataBuildPacket(DMR_DPF_UNCONFIRMED, 0x09, false, 1, 2, data, len, blockType, 0, bursts, DMR_DATA_MAX_BURSTS);
		int blocks = (len + 4 + blockLen - 1) / blockLen;

		if (n == 0)
		{
			CHECK(((blocks + 1) > DMR_DATA_MAX_BURSTS) || ((blocks * blockLen) > DMR_DATA_MAX_PACKET));
			break;
		}
		CHECK(n == (blocks + 1));
		CHECK(bursts[1].dataType == blockType && bursts[1].length == blockLen);
		CHECK(feed(bursts, n) == DMR_DATA_RX_PACKET);
		CHECK(dmrDataRxPacket.length == len && dmrDataRxPacket.dataType == blockType);
		CHECK(memcmp(dmrDataRxPacket.data, data, len) == 0);
		longest = len;
	}
	CHECK(longest >= 400);
	CHECK(dmrDataBuildPacket(DMR_DPF_UNCONFIRMED, 0x09, false, 1, 2, data, 10, DT_CSBK, 0, bursts, DMR_DATA_MAX_BURSTS) == 0);
}

// TMS with Rate 3/4 and Rate 1 blocks, printed for the external decoder
static void testTMSRates(void)
{
	dmrBurst_t bursts[DMR_DATA_MAX_BURSTS];
	const char *text = "Rate test from TestText, long enough for a few blocks of each size";

	for (int r = 0; r < 2; r++)
	{
		uint8_t blockType = r ? DT_RATE_1_DATA : DT_RATE_34_DATA;
		int n = dmrDataBuildTMS(false, 235, 3141592, text, 3, false, blockType, 1, bursts, DMR_DATA_MAX_BURSTS);
		dmrDataUDP_t udp;
		dmrDataTMS_t tms;

		CHECK(n > 2);
		CHECK(feed(bursts, n) == DMR_DATA_RX_PACKET);
		CHECK(dmrDataGetUDP(&dmrDataRxPacket, &udp) && dmrDataDecodeTMS(udp.payload, udp.length, &tms));
		CHECK(strcmp(tms.text, text) == 0);
		dumpBursts(r ? "tms-rate1" : "tms-rate34", bursts, n);
	}
}

// Any IP protocol: built from layer 4, received back unchanged; a UDP checksum is right for the CAI addresses
static void testIPPackets(void)
{
	dmrBurst_t bursts[DMR_DATA_MAX_BURSTS];
	uint8_t echo[24] = { 8, 0, 0x5A, 0x5A, 0x12, 0x34, 0, 7 };
	uint8_t sctp[28] = { 0x0B, 0xB8, 0x0B, 0xB9, 0xDE, 0xAD, 0xBE, 0xEF, 0x11, 0x22, 0x33, 0x44, 4, 0, 0, 16 };
	const uint8_t protocols[2] = { DMR_IP_PROTO_ICMP, DMR_IP_PROTO_SCTP };
	const uint8_t *messages[2] = { echo, sctp };
	const int lengths[2] = { sizeof(echo), sizeof(sctp) };
	dmrDataIP_t ip;
	dmrDataUDP_t udp;

	for (int i = 8; i < (int)sizeof(echo); i++) echo[i] = i;
	for (int i = 16; i < (int)sizeof(sctp); i++) sctp[i] = 0xA0 + i;

	for (int m = 0; m < 2; m++)
	{
		int n = dmrDataBuildIP(m == 1, 235, 3141592, protocols[m], messages[m], lengths[m], DT_RATE_12_DATA, 1, bursts, DMR_DATA_MAX_BURSTS);

		CHECK(n > 2);
		CHECK(feed(bursts, n) == DMR_DATA_RX_PACKET);
		CHECK(dmrDataRxPacket.sap == DMR_SAP_IP && dmrDataRxPacket.group == (m == 1));
		CHECK(dmrDataGetIP(&dmrDataRxPacket, &ip) && ip.protocol == protocols[m]);
		CHECK(ip.length == lengths[m] && memcmp(ip.payload, messages[m], lengths[m]) == 0);
		CHECK(!dmrDataGetUDP(&dmrDataRxPacket, &udp));
		dumpBursts(m ? "ip-sctp" : "ip-icmp", bursts, n);
	}

	// UDP from layer 4: the length and checksum are redone
	uint8_t datagram[14] = { 0x0F, 0xA1, 0x0F, 0xA7, 0xFF, 0xFF, 0x12, 0x34, 'h', 'e', 'l', 'l', 'o', '!' };
	int n = dmrDataBuildIP(false, 235, 3141592, DMR_IP_PROTO_UDP, datagram, sizeof(datagram), DT_RATE_34_DATA, 0, bursts, DMR_DATA_MAX_BURSTS);
	CHECK(feed(bursts, n) == DMR_DATA_RX_PACKET);
	CHECK(dmrDataGetUDP(&dmrDataRxPacket, &udp) && udp.srcPort == 4001 && udp.dstPort == 4007 && udp.length == 6);
	const uint8_t *d = dmrDataRxPacket.data;
	uint32_t sum = 17 + sizeof(datagram);
	for (int i = 12; i < 20; i += 2) sum += (d[i] << 8) | d[i + 1];
	for (int i = 20; i < 20 + (int)sizeof(datagram); i += 2) sum += (d[i] << 8) | d[i + 1];
	while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
	CHECK(sum == 0xFFFF && d[24] == 0 && d[25] == sizeof(datagram));

	// A fragment isn't taken
	dmrDataRxPacket.data[6] |= 0x20;// more fragments
	CHECK(!dmrDataGetIP(&dmrDataRxPacket, &ip));
	CHECK(dmrDataBuildIP(false, 235, 1, DMR_IP_PROTO_UDP, datagram, 4, DT_RATE_12_DATA, 0, bursts, DMR_DATA_MAX_BURSTS) == 0);// too short
}

// Raw data (the network adapter's IPGW_RAW_PORT): the user data of a SAP 10 packet, up to 440 bytes with 2 preambles
static void testRawPacket(void)
{
	dmrBurst_t bursts[DMR_DATA_MAX_BURSTS];
	uint8_t data[441];
	dmrDataUDP_t udp;
	dmrDataIP_t ip;

	for (int i = 0; i < (int)sizeof(data); i++) data[i] = i * 7;

	int n = dmrDataBuildPacket(DMR_DPF_UNCONFIRMED, DMR_SAP_SHORT_DATA, false, 16776962, 3141592, data, 440, DT_RATE_12_DATA, 2, bursts, DMR_DATA_MAX_BURSTS);
	CHECK(n == DMR_DATA_MAX_BURSTS);
	CHECK(dmrDataBuildPacket(DMR_DPF_UNCONFIRMED, DMR_SAP_SHORT_DATA, false, 16776962, 3141592, data, 441, DT_RATE_12_DATA, 2, bursts, DMR_DATA_MAX_BURSTS) == 0);

	n = dmrDataBuildPacket(DMR_DPF_UNCONFIRMED, DMR_SAP_SHORT_DATA, false, 16776962, 3141592, data, 2, DT_RATE_12_DATA, 2, bursts, DMR_DATA_MAX_BURSTS);
	CHECK(n == 4);
	CHECK(feed(bursts, n) == DMR_DATA_RX_PACKET);
	CHECK(dmrDataRxPacket.sap == DMR_SAP_SHORT_DATA && dmrDataRxPacket.dpf == DMR_DPF_UNCONFIRMED);
	CHECK(dmrDataRxPacket.dst == 16776962 && dmrDataRxPacket.src == 3141592 && !dmrDataRxPacket.group);
	CHECK(dmrDataRxPacket.length == 2 && memcmp(dmrDataRxPacket.data, data, 2) == 0);
	CHECK(!dmrDataGetUDP(&dmrDataRxPacket, &udp) && !dmrDataGetIP(&dmrDataRxPacket, &ip));
	dumpBursts("raw-short-data", bursts, n);
}

static void testTMSAck(void)
{
	dmrBurst_t bursts[DMR_DATA_MAX_BURSTS];
	int n = dmrDataBuildTMSAck(3141592, 235, 5, 0, bursts, DMR_DATA_MAX_BURSTS);

	CHECK(feed(bursts, n) == DMR_DATA_RX_PACKET);

	dmrDataUDP_t udp;
	dmrDataTMS_t tms;
	CHECK(dmrDataGetUDP(&dmrDataRxPacket, &udp));
	CHECK(dmrDataDecodeTMS(udp.payload, udp.length, &tms));
	CHECK(tms.isAck);
	CHECK(tms.seq == 5);
	CHECK((udp.length == 5) && (memcmp(udp.payload, "\x00\x03\x9F\x00\x05", 5) == 0));

	// A sequence number over 31 needs the second octet: the same bytes as the MOTOTRBO capture (s/n 53)
	n = dmrDataBuildTMSAck(3141592, 235, 53, 2, bursts, DMR_DATA_MAX_BURSTS);
	CHECK(feed(bursts, n) == DMR_DATA_RX_PACKET);
	CHECK(dmrDataGetUDP(&dmrDataRxPacket, &udp));
	CHECK((udp.length == 6) && (memcmp(udp.payload, "\x00\x04\x9F\x00\x95\x20", 6) == 0));
	CHECK(dmrDataDecodeTMS(udp.payload, udp.length, &tms) && tms.isAck && (tms.seq == 53));
	dumpBursts("tms-ack", bursts, n);
}

static void testRadioDeskVectors(void)
{
	// TMS samples from RadioDesk's tests (ok-dmrlib over the air capture)
	const uint8_t ahoj[] = { 0x00, 0x0D, 0xE0, 0x01, 0x01, 0x95, 0x44, 0x61, 0x00, 0x68, 0x00, 0x6F, 0x00, 0x6A, 0x00 };
	const uint8_t ack[] = { 0x00, 0x04, 0x9F, 0x00, 0x95, 0x20 };
	dmrDataTMS_t tms;

	CHECK(dmrDataDecodeTMS(ahoj, sizeof(ahoj), &tms));
	CHECK(!tms.isAck && tms.ackRequested);
	CHECK(strcmp(tms.text, "ahoj") == 0);
	CHECK(tms.seq == 85);// 0x95 0x44: low bits 0x15, bits 5-6 0x40 (with the encoding)

	CHECK(dmrDataDecodeTMS(ack, sizeof(ack), &tms));
	CHECK(tms.isAck);
	CHECK(tms.seq == 53);

	// Compressed UDP/IP header, Motorola TMS port id 98
	static dmrDataPacket_t p;
	const uint8_t compressed[] = { 0x00, 0x01, 0x00, 98, 98 };
	p.sap = 0x03;
	memcpy(p.data, compressed, sizeof(compressed));
	memcpy(&p.data[sizeof(compressed)], ahoj, sizeof(ahoj));
	p.length = sizeof(compressed) + sizeof(ahoj);

	dmrDataUDP_t udp;
	CHECK(dmrDataGetUDP(&p, &udp));
	CHECK(udp.appPort == DMR_UDP_PORT_TMS);
	CHECK(udp.length == sizeof(ahoj));
}

static void testARS(void)
{
	uint8_t pdu[DMR_ARS_MAX_PDU];
	dmrBurst_t bursts[DMR_DATA_MAX_BURSTS];
	dmrDataUDP_t udp;

	// RadioDesk's test vector (device ID "11"), and the 12 byte PDU hbnet recognizes a MOTOTRBO radio's by (000cf0)
	CHECK((dmrDataBuildARSRegistration(11, pdu) == 9) && (memcmp(pdu, "\x00\x07\xF0\x20\x02\x31\x31\x00\x00", 9) == 0));
	int length = dmrDataBuildARSRegistration(3101276, pdu);
	CHECK((length == 14) && (memcmp(pdu, "\x00\x0C\xF0\x20\x07" "3101276" "\x00\x00", 14) == 0));
	CHECK(dmrDataDecodeARS(pdu, length) == DMR_ARS_DEVICE_REGISTRATION);
	CHECK(dmrDataBuildARSRegistration(16776415, pdu) <= DMR_ARS_MAX_PDU);

	CHECK((dmrDataBuildARSDeregistration(pdu) == 3) && (memcmp(pdu, "\x00\x01\x31", 3) == 0));
	CHECK(dmrDataDecodeARS(pdu, 3) == DMR_ARS_DEVICE_DEREGISTRATION);

	// The server's acknowledgment (hbnet's data gateway sends 00 02 BF 01) and a query
	CHECK(dmrDataDecodeARS((const uint8_t *)"\x00\x02\xBF\x01", 4) == DMR_ARS_RESPONSE);
	CHECK(dmrDataDecodeARS((const uint8_t *)"\x00\x01\x34", 3) == DMR_ARS_QUERY);
	CHECK(dmrDataDecodeARS((const uint8_t *)"\x00\x05\xBF\x01", 4) == -1);// truncated
	CHECK(dmrDataDecodeARS((const uint8_t *)"\x00\x01\x0F", 3) == -1);// no control bit

	// As it goes on air: UDP 4005 both ways, to the server's individual ID
	length = dmrDataBuildARSRegistration(3101276, pdu);
	int n = dmrDataBuildUDP(false, 9990100, 3101276, DMR_UDP_PORT_ARS, DMR_UDP_PORT_ARS, pdu, length, DT_RATE_12_DATA, 2, bursts, DMR_DATA_MAX_BURSTS);
	CHECK(feed(bursts, n) == DMR_DATA_RX_PACKET);
	CHECK(!dmrDataRxPacket.group && (dmrDataRxPacket.dst == 9990100) && (dmrDataRxPacket.src == 3101276));
	CHECK(dmrDataGetUDP(&dmrDataRxPacket, &udp) && (udp.appPort == DMR_UDP_PORT_ARS) && (udp.length == length));
	dumpBursts("ars-registration", bursts, n);
}

// UDT NMEA location report: the bytes RadioDesk's decoder (radiodesk.dmr.location._decode_udt) reads as 33.76872 N,
// 84.38539 W, 22.2 km/h, 14:07:30 UTC, header and appended block CRCs good
static void testUDTLocation(void)
{
	static const char *GOLDEN[] = { "bd008003986fd42f525c3aaf", "bd008002986fd42f525c82ce", "0005986fd42f525c001a6c3c", "51886e13495171349c3b76a0" };
	static const uint8_t TYPES[] = { DT_CSBK, DT_CSBK, DT_DATA_HEADER, DT_RATE_12_DATA };
	dmrDataLocation_t loc = { .fixValid = true, .north = true, .east = false, .latDegrees = 33, .latMinutes10000 = 461234, .lonDegrees = 84,
								.lonMinutes10000 = 231234, .speedKnots = 12, .utcHours = 14, .utcMinutes = 7, .utcSeconds = 38 };
	dmrBurst_t b[8];
	int n = dmrDataBuildUDTLocation(false, 9990100, 3101276, &loc, 2, b, 8);

	CHECK(n == 4);
	for (int i = 0; i < n; i++)
	{
		char hex[64] = { 0 };

		for (int j = 0; j < b[i].length; j++)
		{
			sprintf(&hex[j * 2], "%02x", b[i].payload[j]);
		}
		CHECK((b[i].dataType == TYPES[i]) && (strcmp(hex, GOLDEN[i]) == 0));
	}

	// Straight after a call: no preambles; too small an output buffer: nothing
	CHECK(dmrDataBuildUDTLocation(true, 9, 3101276, &loc, 0, b, 8) == 2);
	CHECK(b[0].dataType == DT_DATA_HEADER);
	CHECK((b[0].payload[0] & 0x80) != 0);// group
	CHECK(dmrDataBuildUDTLocation(false, 9, 3101276, &loc, 2, b, 3) == 0);
}

static void testCSBK(void)
{
	dmrBurst_t burst;
	const uint8_t csbk[10] = { 0x9F, 0x00, 0x00, 0x00, 0x00, 0xEB, 0x00, 0x2F, 0xEF, 0xD8 };// Call alert to 235 from 3141592

	CHECK(dmrDataBuildCSBK(csbk, &burst) == 1);
	CHECK(dmrDataRxBurst(&burst) == DMR_DATA_RX_CSBK);
	burst.payload[2] ^= 1;
	CHECK(dmrDataRxBurst(&burst) == DMR_DATA_RX_ERROR);
	burst.payload[2] ^= 1;
	dumpBursts("csbk", &burst, 1);
}

// Call alert and radio check to 235 from 3141592, and the answers from 235
static void testCSBKAnswers(void)
{
	dmrBurst_t burst;
	const uint8_t alert[10] = { 0x9F, 0x10, 0x00, 0x00, 0x00, 0x00, 0xEB, 0x2F, 0xEF, 0xD8 };
	const uint8_t check[10] = { 0xA4, 0x10, 0x00, 0x80, 0x00, 0x00, 0xEB, 0x2F, 0xEF, 0xD8 };
	const uint8_t ackExpected[10] = { 0xA0, 0x10, 0x00, 0x00, 0x2F, 0xEF, 0xD8, 0x00, 0x00, 0xEB };
	const uint8_t checkExpected[10] = { 0xA4, 0x10, 0x00, 0x00, 0x00, 0x00, 0xEB, 0x2F, 0xEF, 0xD8 };
	uint8_t other[10];

	CHECK(dmrDataBuildCSBKAnswer(alert, 235, &burst) == 1);
	CHECK(burst.dataType == DT_CSBK);
	CHECK(memcmp(burst.payload, ackExpected, 10) == 0);
	CHECK(dmrDataRxBurst(&burst) == DMR_DATA_RX_CSBK);// valid CRC

	CHECK(dmrDataBuildCSBKAnswer(check, 235, &burst) == 1);
	CHECK(memcmp(burst.payload, checkExpected, 10) == 0);
	dumpBursts("radio check answer", &burst, 1);

	CHECK(dmrDataBuildCSBKAnswer(alert, 236, &burst) == 0);// someone else's
	CHECK(dmrDataBuildCSBKAnswer(alert, 3141592, &burst) == 0);// our own, from the repeater
	memcpy(other, check, 10);
	other[3] = 0x00;
	CHECK(dmrDataBuildCSBKAnswer(other, 235, &burst) == 0);// an answer isn't answered
	other[0] = 0xA0;
	CHECK(dmrDataBuildCSBKAnswer(other, 235, &burst) == 0);// nor an ack
	memcpy(other, alert, 10);
	other[1] = 0x68;
	CHECK(dmrDataBuildCSBKAnswer(other, 235, &burst) == 0);// unknown feature set
	other[1] = 0x00;
	CHECK(dmrDataBuildCSBKAnswer(other, 235, &burst) == 1);// ETSI
}

// Confirmed Rate 1/2 packet as another radio would send it: 10 data bytes per block after the DBSN / CRC-9
static void testConfirmedRx(void)
{
	uint8_t user[50];
	uint8_t packed[60];
	dmrBurst_t bursts[8];
	int blocks = 6;// 6 * 10 = 60 = 50 user + 6 pad + 4 CRC
	int pad = 6;

	for (int i = 0; i < (int)sizeof(user); i++)
	{
		user[i] = 0x30 + i;
	}
	memcpy(packed, user, sizeof(user));
	memset(&packed[50], 0, pad);
	uint32_t crc = dmrDataCRC32(packed, 56);
	packed[56] = crc; packed[57] = crc >> 8; packed[58] = crc >> 16; packed[59] = crc >> 24;

	uint8_t h[12] = { 0x40 | DMR_DPF_CONFIRMED, (DMR_SAP_IP << 4) | pad, 0x00, 0x00, 235, 0x2F, 0xEF, 0xD8, 0x80 | blocks, (5 << 4) | 0x08, 0, 0 };
	addHeaderCRC(h);
	h[10] ^= 0xCC; h[11] ^= 0xCC;
	bursts[0].dataType = DT_DATA_HEADER; bursts[0].length = 12; memcpy(bursts[0].payload, h, 12);
	for (int i = 0; i < blocks; i++)
	{
		bursts[1 + i].dataType = DT_RATE_12_DATA;
		bursts[1 + i].length = 12;
		bursts[1 + i].payload[0] = i << 1;// DBSN, CRC-9 not checked (the CRC-32 covers the data)
		bursts[1 + i].payload[1] = 0;
		memcpy(&bursts[1 + i].payload[2], &packed[i * 10], 10);
	}

	CHECK(feed(bursts, 1 + blocks) == DMR_DATA_RX_PACKET);
	CHECK(dmrDataRxPacket.dpf == DMR_DPF_CONFIRMED);
	CHECK(dmrDataRxPacket.responseRequested);
	CHECK(dmrDataRxPacket.sendSeq == 5);
	CHECK(dmrDataRxPacket.length == sizeof(user));
	CHECK(memcmp(dmrDataRxPacket.data, user, sizeof(user)) == 0);

	dmrBurst_t ack;
	CHECK(dmrDataBuildResponseAck(DMR_SAP_IP, 3141592, 235, 5, &ack) == 1);
	CHECK(feed(&ack, 1) == DMR_DATA_RX_PACKET);
	CHECK(dmrDataRxPacket.dpf == DMR_DPF_RESPONSE);
	dumpBursts("response-ack", &ack, 1);
}


static void hex(const char *h, uint8_t *out)
{
	for (int i = 0; h[2 * i]; i++)
	{
		unsigned v;
		sscanf(&h[2 * i], "%2x", &v);
		out[i] = (uint8_t)v;
	}
}

// Compressed UDP/IPv4 headers (SAP 3) on transmit, read back by the receive decoder. The vectors here and in
// testCRC9 / testConfirmedTx come from tests/host/dmr_ip_vectors.py, written from TS 102 361-1 B.3.8-B.3.10, 9.2.1
// and TS 102 361-3 7.2 apart from this code (polynomial division, not the shift register used here)
static void testSap3(void)
{
	uint8_t h[DMR_UDPC_HEADER_MAX], want[16];
	dmrBurst_t bursts[DMR_DATA_MAX_BURSTS];
	dmrDataUDP_t udp;
	dmrDataIP_t ip;

	// SNMP GET from an ephemeral port: no IDs, both ports in the extended headers (9 octets)
	CHECK(dmrDataCompressUDPHeader(0x1234, false, 50000, 161, h) == 9);
	hex("1234000000c35000a1", want);
	CHECK(memcmp(h, want, 9) == 0);
	// ETSI text both ways to a group: IDs 1, DAID 2 (5 octets)
	CHECK(dmrDataCompressUDPHeader(7, true, 5016, 5016, h) == 5);
	hex("0007020101", want);
	CHECK(memcmp(h, want, 5) == 0);
	// LIP source (ID 2), destination without an ID: in extended header 1 (7 octets)
	CHECK(dmrDataCompressUDPHeader(7, false, 5017, 6000, h) == 7);
	hex("00070002001770", want);
	CHECK(memcmp(h, want, 7) == 0);

	// A datagram from the host (layer 4) in auto mode: SAP 3, back to the same ports and payload
	uint8_t l4[8 + 60];
	l4[0] = 50000 >> 8; l4[1] = 50000 & 0xFF; l4[2] = 0; l4[3] = 161; l4[4] = 0; l4[5] = sizeof(l4); l4[6] = 0xAB; l4[7] = 0xCD;
	for (int i = 8; i < (int)sizeof(l4); i++) l4[i] = (uint8_t)(0x30 + i);
	dmrDataSetUdpCompression(DMR_UDPC_AUTO);
	dmrDataResetIpTxStats();
	int n = dmrDataBuildIP(false, 100, 10005, DMR_IP_PROTO_UDP, l4, sizeof(l4), DT_RATE_12_DATA, 2, bursts, DMR_DATA_MAX_BURSTS);
	CHECK(n == 2 + 1 + 7);// 9 + 60 + 4 = 73 octets: 7 blocks; with the full header 28 + 60 + 4 = 92: 8
	CHECK(feed(bursts, n) == DMR_DATA_RX_PACKET);
	CHECK(dmrDataRxPacket.sap == DMR_SAP_UDPIP_COMPRESSION && dmrDataRxPacket.dst == 100 && dmrDataRxPacket.src == 10005);
	CHECK(dmrDataRxPacket.length == 9 + 60 && (dmrDataRxPacket.data[2] == 0x00));
	CHECK(dmrDataGetUDP(&dmrDataRxPacket, &udp) && udp.srcPort == 50000 && udp.dstPort == 161 && udp.length == 60 &&
	      memcmp(udp.payload, &l4[8], 60) == 0);
	CHECK(!dmrDataGetIP(&dmrDataRxPacket, &ip));
	dumpBursts("udp-sap3-snmp", bursts, n);
	dmrDataIpTxStats_t st;
	dmrDataGetIpTxStats(&st);
	CHECK(st.sap3 == 1 && st.sap4 == 0 && st.octetsSaved == 19 && st.blocksSaved == 1);

	// A DMR application port keeps the full header in auto mode, goes compressed (ID 98 for TMS) in all
	uint8_t tms[8 + 6] = { 0x0F, 0xA7, 0x0F, 0xA7, 0, 14, 0, 0, 0x00, 0x04, 0x9F, 0x00, 0x95, 0x20 };
	n = dmrDataBuildIP(false, 100, 10005, DMR_IP_PROTO_UDP, tms, sizeof(tms), DT_RATE_12_DATA, 0, bursts, DMR_DATA_MAX_BURSTS);
	CHECK(feed(bursts, n) == DMR_DATA_RX_PACKET && dmrDataRxPacket.sap == DMR_SAP_IP);
	dmrDataSetUdpCompression(DMR_UDPC_ALL);
	n = dmrDataBuildIP(false, 100, 10005, DMR_IP_PROTO_UDP, tms, sizeof(tms), DT_RATE_12_DATA, 0, bursts, DMR_DATA_MAX_BURSTS);
	CHECK(feed(bursts, n) == DMR_DATA_RX_PACKET && dmrDataRxPacket.sap == DMR_SAP_UDPIP_COMPRESSION);
	CHECK(dmrDataRxPacket.length == 5 + 6 && dmrDataRxPacket.data[3] == 98 && dmrDataRxPacket.data[4] == 98);
	CHECK(dmrDataGetUDP(&dmrDataRxPacket, &udp) && udp.appPort == DMR_UDP_PORT_TMS && udp.length == 6);
	// The service's own UDP builder follows the same setting
	n = dmrDataBuildUDP(true, 9, 10005, 5016, 5016, (const uint8_t *)"hi", 2, DT_RATE_12_DATA, 0, bursts, DMR_DATA_MAX_BURSTS);
	CHECK(feed(bursts, n) == DMR_DATA_RX_PACKET && dmrDataRxPacket.sap == DMR_SAP_UDPIP_COMPRESSION &&
	      dmrDataRxPacket.group && dmrDataRxPacket.length == 7 && dmrDataRxPacket.data[2] == 0x02);
	CHECK(dmrDataGetUDP(&dmrDataRxPacket, &udp) && udp.srcPort == 5016 && udp.length == 2 && memcmp(udp.payload, "hi", 2) == 0);
	// Off: the full header
	dmrDataSetUdpCompression(DMR_UDPC_OFF);
	n = dmrDataBuildIP(false, 100, 10005, DMR_IP_PROTO_UDP, l4, sizeof(l4), DT_RATE_12_DATA, 2, bursts, DMR_DATA_MAX_BURSTS);
	CHECK(n == 2 + 1 + 8 && feed(bursts, n) == DMR_DATA_RX_PACKET && dmrDataRxPacket.sap == DMR_SAP_IP);
	CHECK(dmrDataGetUDP(&dmrDataRxPacket, &udp) && udp.srcPort == 50000 && udp.length == 60);
	// A UDP length that disagrees with the datagram can't be compressed (the receiver would take the wrong length)
	dmrDataSetUdpCompression(DMR_UDPC_AUTO);
	l4[5] = sizeof(l4) + 2;
	n = dmrDataBuildIP(false, 100, 10005, DMR_IP_PROTO_UDP, l4, sizeof(l4), DT_RATE_12_DATA, 2, bursts, DMR_DATA_MAX_BURSTS);
	CHECK(feed(bursts, n) == DMR_DATA_RX_PACKET && dmrDataRxPacket.sap == DMR_SAP_IP);
	// ... octets past a shorter UDP length are padding: dropped, the rest compressed
	l4[5] = 8 + 20;
	n = dmrDataBuildIP(false, 100, 10005, DMR_IP_PROTO_UDP, l4, sizeof(l4), DT_RATE_12_DATA, 2, bursts, DMR_DATA_MAX_BURSTS);
	CHECK(feed(bursts, n) == DMR_DATA_RX_PACKET && dmrDataRxPacket.sap == DMR_SAP_UDPIP_COMPRESSION);
	CHECK(dmrDataGetUDP(&dmrDataRxPacket, &udp) && udp.length == 20);
	l4[5] = sizeof(l4);

	// A reserved opcode isn't read
	static dmrDataPacket_t p;
	p.sap = DMR_SAP_UDPIP_COMPRESSION;
	hex("0001000101", p.data);
	p.data[3] |= 0x80;
	p.length = 8;
	CHECK(!dmrDataGetUDP(&p, &udp));
}

// The CRC-9 (B.3.10), vectors from a polynomial division of the clause's M(x) apart from this code
static void testCRC9(void)
{
	uint8_t d[22];

	memset(d, 0, sizeof(d));
	CHECK(dmrDataCRC9(d, 10, 0) == 0x1FF);
	for (int i = 0; i < 22; i++) d[i] = i;
	CHECK(dmrDataCRC9(d, 10, 0) == 0x0FC);
	CHECK(dmrDataCRC9(d, 10, 5) == 0x1C1);
	CHECK(dmrDataCRC9(d, 22, 3) == 0x142);
	memset(d, 0xFF, 16);
	CHECK(dmrDataCRC9(d, 16, 127) == 0x1D8);
}

static int dropBurst(dmrBurst_t *b, int n, int i)
{
	memmove(&b[i], &b[i + 1], (size_t)(n - i - 1) * sizeof(dmrBurst_t));
	return n - 1;
}

// Our confirmed packets (TS 102 361-1 9.2.1, 8.2.2.2) byte for byte against the vectors, and the selective retry
// round trip through the receiver: a bad block, a lost block, a CRC-32 failure with good blocks
static void testConfirmedTx(void)
{
	dmrBurst_t b[DMR_DATA_MAX_BURSTS], r[2];
	uint8_t pdu[8], want[12], flags[8];

	hex("0001000101", pdu);
	memcpy(&pdu[5], "ODR", 3);
	CHECK(dmrDataConfirmedBlocks(8, DT_RATE_12_DATA) == 2 && dmrDataConfirmedBlockData(DT_RATE_34_DATA) == 16);
	int n = dmrDataBuildConfirmed(DMR_SAP_UDPIP_COMPRESSION, 100, 10005, pdu, 8, DT_RATE_12_DATA, 5, true, 0, 1, b, DMR_DATA_MAX_BURSTS);
	CHECK(n == 4 && b[0].dataType == DT_CSBK && b[0].payload[3] == 3);
	hex("433800006400271582581c55", want);
	CHECK(b[1].dataType == DT_DATA_HEADER && memcmp(b[1].payload, want, 12) == 0);
	hex("01ef00010001014f44520000", want);
	CHECK(b[2].dataType == DT_RATE_12_DATA && b[2].length == 12 && memcmp(b[2].payload, want, 12) == 0);
	hex("03fb000000000000a4625e72", want);
	CHECK(memcmp(b[3].payload, want, 12) == 0);
	dumpBursts("confirmed-sap3", b, n);

	CHECK(feed(b, n) == DMR_DATA_RX_PACKET);
	CHECK(dmrDataRxPacket.dpf == DMR_DPF_CONFIRMED && dmrDataRxPacket.firstTry && dmrDataRxPacket.sendSeq == 5 &&
	      dmrDataRxPacket.responseRequested && dmrDataRxPacket.length == 8 && memcmp(dmrDataRxPacket.data, pdu, 8) == 0);
	uint32_t mism = dmrDataRxCrc9Mismatches();

	// A longer one at rate 3/4: block 2 arrives with a bit error, so the receiver asks for it alone
	uint8_t big[100];
	for (int i = 0; i < (int)sizeof(big); i++) big[i] = (uint8_t)(i * 13);
	n = dmrDataBuildConfirmed(DMR_SAP_IP, 100, 10005, big, sizeof(big), DT_RATE_34_DATA, 2, true, 0, 0, b, DMR_DATA_MAX_BURSTS);
	CHECK(n == 1 + 7);// 104 octets in 16: 7 blocks
	b[3].payload[9] ^= 0x10;
	CHECK(feed(b, n) == DMR_DATA_RX_RETRY);
	dmrDataRxRetryFlags(flags);
	CHECK(flags[0] == 0xFB && flags[1] == 0xFF && flags[7] == 0xFF);
	// The retry has only the asked for blocks, F clear, the same N(S)
	uint64_t missing = 0;
	for (int k = 0; k < 64; k++) if (!((flags[k / 8] >> (k % 8)) & 1)) missing |= 1ULL << k;
	n = dmrDataBuildConfirmed(DMR_SAP_IP, 100, 10005, big, sizeof(big), DT_RATE_34_DATA, 2, false, missing, 0, b, DMR_DATA_MAX_BURSTS);
	CHECK(n == 2 && (b[0].payload[8] == 0x01) && ((b[1].payload[0] >> 1) == 2));
	dmrDataRxReset();
	CHECK(dmrDataRxBurst(&b[0]) == DMR_DATA_RX_NONE);
	CHECK(dmrDataRxBurst(&b[1]) == DMR_DATA_RX_PACKET);
	CHECK(dmrDataRxPacket.length == sizeof(big) && memcmp(dmrDataRxPacket.data, big, sizeof(big)) == 0 &&
	      !dmrDataRxPacket.firstTry);
	CHECK(dmrDataRxCrc9Mismatches() == mism);
	// The selective ACK itself (header and flags block against the vector), read back by the sender
	CHECK(dmrDataBuildResponse(DMR_SAP_IP, 10005, 100, DMR_RESPONSE_SACK(2), flags, r) == 2);
	CHECK(feed(r, 2) == DMR_DATA_RX_PACKET);
	CHECK(dmrDataRxPacket.dpf == DMR_DPF_RESPONSE && dmrDataRxPacket.length == 8 &&
	      DMR_RESPONSE_CLASS(dmrDataRxPacket.response) == 2 && (dmrDataRxPacket.response & 7) == 2 &&
	      memcmp(dmrDataRxPacket.data, flags, 8) == 0);
	CHECK(dmrDataBuildResponse(DMR_SAP_UDPIP_COMPRESSION, 10005, 100, DMR_RESPONSE_SACK(5), (const uint8_t *)"\xfd\xff\xff\xff\xff\xff\xff\xff", r) == 2);
	hex("0130002715000064018533f4", want);
	CHECK(memcmp(r[0].payload, want, 12) == 0);
	hex("fdffffffffffffff00b6ae30", want);
	CHECK(r[1].dataType == DT_RATE_12_DATA && memcmp(r[1].payload, want, 12) == 0);

	// A lost block (no burst at all): the reassembly is ended from outside, and the same retry completes it
	n = dmrDataBuildConfirmed(DMR_SAP_IP, 100, 10005, big, sizeof(big), DT_RATE_34_DATA, 3, true, 0, 0, b, DMR_DATA_MAX_BURSTS);
	n = dropBurst(b, n, 5);// block 4
	CHECK(feed(b, n) == DMR_DATA_RX_NONE && dmrDataRxConfirmedActive());
	CHECK(dmrDataRxConfirmedEnd() == DMR_DATA_RX_RETRY && !dmrDataRxConfirmedActive());
	dmrDataRxRetryFlags(flags);
	CHECK(flags[0] == 0xEF);
	n = dmrDataBuildConfirmed(DMR_SAP_IP, 100, 10005, big, sizeof(big), DT_RATE_34_DATA, 3, false, 1ULL << 4, 1, b, DMR_DATA_MAX_BURSTS);
	CHECK(feed(b, n) == DMR_DATA_RX_PACKET && memcmp(dmrDataRxPacket.data, big, sizeof(big)) == 0);

	// Every CRC-9 good but the CRC-32 wrong (the sender's data changed between blocks): a NACK
	n = dmrDataBuildConfirmed(DMR_SAP_IP, 100, 10005, big, sizeof(big), DT_RATE_34_DATA, 4, true, 0, 0, b, DMR_DATA_MAX_BURSTS);
	b[2].payload[5] ^= 1;
	uint16_t c9 = dmrDataCRC9(&b[2].payload[2], 16, 1) ^ DMR_CRC9_MASK_RATE_34;
	b[2].payload[0] = (1 << 1) | (c9 >> 8);
	b[2].payload[1] = c9 & 0xFF;
	CHECK(feed(b, n) == DMR_DATA_RX_NACK);

	// A selective retry for a packet we don't hold (another packet came between): a NACK asks for the whole packet
	n = dmrDataBuildConfirmed(DMR_SAP_IP, 100, 10005, big, sizeof(big), DT_RATE_34_DATA, 6, false, 1ULL << 5, 0, b, DMR_DATA_MAX_BURSTS);
	CHECK(feed(b, n) == DMR_DATA_RX_NACK);

	// Too big for one selective ACK block (64 blocks) or the burst list
	static uint8_t huge[DMR_DATA_MAX_PACKET];
	CHECK(dmrDataBuildConfirmed(DMR_SAP_IP, 100, 10005, huge, 400, DT_RATE_12_DATA, 0, true, 0, 0, b, DMR_DATA_MAX_BURSTS) == 0);
	CHECK(dmrDataConfirmedBlocks(700, DT_RATE_12_DATA) == 0);
}

// Air time of typical packets, unconfirmed rate 1/2 with 2 preamble CSBKs (docs/spec/dmr-data.md 11.3): the bursts
// with the full and the compressed header. Printed for the spec, and checked
static int burstsFor(int userData, int perBlock)
{
	return 2 + 1 + ((userData + 4 + perBlock - 1) / perBlock);
}

static void testAirtime(void)
{
	uint8_t ars[DMR_ARS_MAX_PDU];
	int arsLen = dmrDataBuildARSRegistration(3101234, ars);
	struct { const char *what; int payload; int header3; } t[] = {
		{ "ARS registration (7 digit ID)", arsLen, 9 },
		{ "TMS, 16 characters", 6 + 32, 5 },		// ID 98 both ways
		{ "TMS ACK", 5, 5 },
		{ "LRRP immediate answer with a position", 26, 9 },
		{ "SNMP GET, 60 octets", 60, 9 },
		{ "SNMP response, 120 octets", 120, 9 },
		{ "ETSI LIP short report, 9 octets", 9, 5 },
	};

	printf("# airtime: packet, user octets, bursts SAP 4 / SAP 3 at rate 1/2 (2 preambles), confirmed rate 1/2\n");
	for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); i++)
	{
		int full = 28 + t[i].payload, comp = t[i].header3 + t[i].payload;
		printf("# airtime %s: %d / %d bursts, confirmed %d / %d\n", t[i].what, burstsFor(full, 12), burstsFor(comp, 12),
		       burstsFor(full, 10), burstsFor(comp, 10));
	}
	CHECK(arsLen == 14);
	CHECK(burstsFor(28 + 60, 12) == 11 && burstsFor(9 + 60, 12) == 10);
}

// Bursts to 33 byte MMDVM frames and back, the frames are printed for the external decoder
static void testFrames(void)
{
	dmrBurst_t bursts[DMR_DATA_MAX_BURSTS];
	int n = dmrDataBuildTMS(false, 235, 3141592, "frame test", 1, true, DT_RATE_12_DATA, 1, bursts, DMR_DATA_MAX_BURSTS);
	uint8_t frame[33];
	dmrBurst_t back;

	printf("# frames cc=7\n");
	for (int i = 0; i < n; i++)
	{
		CHECK(dmrDataBurstToFrame(&bursts[i], 7, frame));
		CHECK(dmrDataFrameToBurst(bursts[i].dataType, frame, &back));
		CHECK(memcmp(back.payload, bursts[i].payload, 12) == 0);
		printf("F %d ", bursts[i].dataType);
		for (int j = 0; j < 33; j++)
		{
			printf("%02x", frame[j]);
		}
		printf("\n");
	}
}

// Rate 3/4 (Trellis) and Rate 1 frames: round trips, the Trellis code corrects errors, and the frames are printed
// ("C dt payload frame") for an independent decoder
static void testCodedFrames(void)
{
	uint32_t seed = 12345;
	int pairsCorrected = 0;

	printf("# coded frames cc=2\n");
	for (int n = 0; n < 40; n++)
	{
		dmrBurst_t burst = { .dataType = (n & 1) ? DT_RATE_1_DATA : DT_RATE_34_DATA, .flags = DMR_BURST_FLAG_MCU_CRC };
		dmrBurst_t back;
		uint8_t frame[33];

		burst.length = (burst.dataType == DT_RATE_1_DATA) ? 24 : 18;
		for (int i = 0; i < burst.length; i++)
		{
			seed = (seed * 1103515245u) + 12345u;
			burst.payload[i] = (n == 0) ? 0 : ((n == 2) ? 0xFF : (seed >> 16));
		}

		CHECK(dmrDataBurstToFrame(&burst, 2, frame));
		CHECK(dmrDataFrameToBurst(burst.dataType, frame, &back));
		CHECK(back.length == burst.length && memcmp(back.payload, burst.payload, burst.length) == 0);

		printf("C %d ", burst.dataType);
		for (int i = 0; i < burst.length; i++)
		{
			printf("%02x", burst.payload[i]);
		}
		printf(" ");
		for (int i = 0; i < 33; i++)
		{
			printf("%02x", frame[i]);
		}
		printf("\n");

		if (burst.dataType == DT_RATE_34_DATA)
		{
			// Any one bit error in the info bits is corrected. Of two, hard decision decoding corrects most (RadioDesk's
			// decoder corrects exactly the same pairs: 421 of these 560)
			for (int e = 0; e < 196; e += 7)
			{
				uint8_t bad[33];
				int p1 = (e < 98) ? e : (e + 68);
				int p2 = ((e + 101) % 196 < 98) ? ((e + 101) % 196) : (((e + 101) % 196) + 68);

				memcpy(bad, frame, 33);
				bad[p1 >> 3] ^= 0x80 >> (p1 & 7);
				CHECK(dmrDataFrameToBurst(DT_RATE_34_DATA, bad, &back) && memcmp(back.payload, burst.payload, 18) == 0);
				bad[p2 >> 3] ^= 0x80 >> (p2 & 7);
				dmrDataFrameToBurst(DT_RATE_34_DATA, bad, &back);
				pairsCorrected += (memcmp(back.payload, burst.payload, 18) == 0);
			}
		}
	}

	printf("# Trellis: %d of 560 two bit errors corrected\n", pairsCorrected);
	CHECK(pairsCorrected == 421);

	dmrBurst_t wrong = { .dataType = DT_RATE_34_DATA, .length = 12 };
	uint8_t frame[33];
	CHECK(!dmrDataBurstToFrame(&wrong, 2, frame));
}

// Received payloads too short for their own headers: the ARS PDU's length octets read only when they are there
// (exact-size heap copies: ASan sees a read past the end), an IPv4 header length under 20 octets isn't UDP
static void testShortPayloads(void)
{
	uint8_t *one = fuzz_exact((const uint8_t *)"\x00", 1);
	uint8_t *none = fuzz_exact(NULL, 0);
	bool accepted;
	uint16_t refresh;
	CHECK(dmrDataDecodeARS(one, 1) == -1);
	CHECK(dmrDataDecodeARS(none, 0) == -1);
	CHECK(!dmrDataDecodeARSResponse(one, 1, &accepted, &refresh));
	free(one);
	free(none);
	uint8_t *two = fuzz_exact((const uint8_t *)"\x00\x02\xBF", 3);	// says 2 octets, has 1
	CHECK(dmrDataDecodeARS(two, 3) == -1);
	free(two);

	// SAP 4 with IHL 0 or 4 (0, 16 octets): not a UDP datagram, whatever follows
	dmrDataPacket_t p;
	dmrDataUDP_t udp;
	memset(&p, 0, sizeof(p));
	p.sap = DMR_SAP_IP;
	p.length = 40;
	p.data[9] = 17;
	for (int ihl = 0; ihl < 5; ihl++)
	{
		p.data[0] = 0x40 | ihl;
		CHECK(!dmrDataGetUDP(&p, &udp));
	}
	p.data[0] = 0x45;
	p.data[3] = 40;
	CHECK(dmrDataGetUDP(&p, &udp) && (udp.length == 12));
	p.data[6] = 0x20;// MF: the first fragment is not the datagram
	CHECK(!dmrDataGetUDP(&p, &udp));
}

// The decoders of what arrives over the air, on mutated payloads: text in every DD / UDT format into small text
// buffers (always terminated within them), IP / UDP layer 4 always inside the packet, TMS, ARS and IP text from
// exact-size heap copies (ODR_FUZZ_ITERS)
static void fuzzDecoders(fuzz_t *f, long iters, long *bad)
{
	static dmrDataPacket_t p;
	uint8_t seed[64] = { 0x45, 0x00, 0x00, 0x30, 0x12, 0x34, 0x40, 0x00, 0x40, 0x11, 0x00, 0x00, 0x0C, 0x00, 0x27, 0x15,
			     0x0E, 0x00, 0x00, 0x5B, 0x0F, 0xA7, 0x0F, 0xA7, 0x00, 0x1C, 0x00, 0x00, 0x00, 0x16, 0xE0, 0x00,
			     0x80, 0x04, 'h', 0, 'i', 0, 0xD8, 0x3D, 0xDE, 0x00, 0xC3, 0xA9, 0xE2, 0x82, 0xAC, 0x00 };
	static const uint8_t formats[] = { DMR_DD_7BIT, DMR_DD_ISO8859_1, 0x05, DMR_DD_UTF8, DMR_DD_UTF16, DMR_DD_UTF16BE,
					   DMR_DD_UTF16LE, DMR_UDT_FORMAT_ISO7, DMR_UDT_FORMAT_ISO8, DMR_UDT_FORMAT_UTF16BE, 0x3F };
	uint8_t buf[DMR_DATA_MAX_PACKET];

	for (long i = 0; i < iters; i++)
	{
		memcpy(buf, seed, sizeof(seed));
		int n = fuzz_mutate(f, buf, 48, sizeof(buf));
		memset(&p, 0, sizeof(p));
		memcpy(p.data, buf, n);
		p.length = n;
		p.sap = (fuzz_below(f, 2) == 0) ? DMR_SAP_IP : DMR_SAP_UDPIP_COMPRESSION;
		p.dpf = (fuzz_below(f, 2) == 0) ? DMR_DPF_DEFINED_SHORT : DMR_DPF_UDT;
		p.ddFormat = formats[fuzz_below(f, sizeof(formats))];
		p.padBits = (uint8_t)fuzz_below(f, 128);

		char text[17];
		int size = 1 + (int)fuzz_below(f, sizeof(text) - 1);
		memset(text, 0x7E, sizeof(text));
		if (dmrDataDecodeEtsiText(&p, text, size) && (memchr(text, 0, size) == NULL))
		{
			(*bad)++;
		}
		if (text[size] != 0x7E && size < (int)sizeof(text))
		{
			(*bad)++;// written past the size given
		}
		dmrDataIP_t ip;
		if (dmrDataGetIP(&p, &ip) && ((ip.payload < p.data) || (ip.length < 0) || (ip.payload + ip.length > p.data + p.length)))
		{
			(*bad)++;
		}
		dmrDataUDP_t udp;
		if (dmrDataGetUDP(&p, &udp) && ((udp.payload < p.data) || (udp.length < 0) || (udp.payload + udp.length > p.data + p.length)))
		{
			(*bad)++;
		}
		uint8_t *q = fuzz_exact(buf, n);
		dmrDataTMS_t tms;
		if (dmrDataDecodeTMS(q, n, &tms) && (memchr(tms.text, 0, sizeof(tms.text)) == NULL))
		{
			(*bad)++;
		}
		int ars = dmrDataDecodeARS(q, n);
		bool accepted;
		uint16_t refresh;
		dmrDataDecodeARSResponse(q, n, &accepted, &refresh);
		if (dmrDataDecodeIPText(q, n, text, size) && (memchr(text, 0, size) == NULL))
		{
			(*bad)++;
		}
		(*bad) += (ars < -1) || (ars > 15);
		free(q);
	}
}

// Whole packets on air with bursts mutated, dropped, repeated or retyped, headers' CRC made good again half the time,
// into the receive state machine: a finished packet's length is within DMR_DATA_MAX_PACKET and the decoders take it
static void fuzzBursts(fuzz_t *f, long iters, long *bad, long *packets)
{
	static dmrBurst_t seeds[6][DMR_DATA_MAX_BURSTS], b[DMR_DATA_MAX_BURSTS * 2];
	int count[6];
	uint8_t pdu[60];
	for (int i = 0; i < (int)sizeof(pdu); i++) pdu[i] = (uint8_t)(i * 7);
	count[0] = dmrDataBuildUDP(false, 9990100, 3101276, 4001, 4001, pdu, 30, DT_RATE_12_DATA, 0, seeds[0], DMR_DATA_MAX_BURSTS);
	count[1] = dmrDataBuildTMS(false, 3101276, 9990100, "fuzz text", 3, true, DT_RATE_34_DATA, 0, seeds[1], DMR_DATA_MAX_BURSTS);
	count[2] = dmrDataBuildEtsiText(true, 91, 3101276, "short data", false, 0, seeds[2], DMR_DATA_MAX_BURSTS);
	dmrDataLocation_t loc = { .fixValid = true, .north = true, .latDegrees = 33, .latMinutes10000 = 461230, .lonDegrees = 84,
				  .lonMinutes10000 = 231234, .speedKnots = 12 };
	count[3] = dmrDataBuildUDTLocation(false, 9990100, 3101276, &loc, 0, seeds[3], DMR_DATA_MAX_BURSTS);
	count[4] = dmrDataBuildIPText(false, 9990100, 3101276, "ip text", DT_RATE_1_DATA, 0, seeds[4], DMR_DATA_MAX_BURSTS);
	count[5] = dmrDataBuildConfirmed(DMR_SAP_IP, 9990100, 3101276, pdu, 40, DT_RATE_12_DATA, 1, true, ~0ull, 0, seeds[5], DMR_DATA_MAX_BURSTS);

	for (long i = 0; i < iters; i++)
	{
		int s = (int)fuzz_below(f, 6), n = 0;
		for (int k = 0; k < count[s] && n < (int)(sizeof(b) / sizeof(b[0])); k++)
		{
			uint32_t op = fuzz_below(f, 12);
			if (op == 0)
			{
				continue;// dropped
			}
			b[n] = seeds[s][k];
			if (op == 1 && n + 1 < (int)(sizeof(b) / sizeof(b[0])))
			{
				b[n + 1] = b[n];// repeated
				n++;
			}
			else if (op == 2)
			{
				b[n].dataType = (uint8_t)fuzz_below(f, 12);
			}
			else if (op <= 5)
			{
				int len = fuzz_mutate(f, b[n].payload, b[n].length, DMR_BURST_PAYLOAD_MAX);
				b[n].length = (uint8_t)len;
				if ((b[n].dataType == DT_DATA_HEADER) && (len >= 12) && (fuzz_below(f, 2) == 0))
				{
					addHeaderCRC(b[n].payload);
					b[n].payload[10] ^= 0xCC;
					b[n].payload[11] ^= 0xCC;
				}
			}
			n++;
		}
		dmrDataRxReset();
		for (int k = 0; k < n; k++)
		{
			dmrDataRxResult_t r = dmrDataRxBurst(&b[k]);
			if (r == DMR_DATA_RX_PACKET)
			{
				(*packets)++;
				if (dmrDataRxPacket.length > DMR_DATA_MAX_PACKET)
				{
					(*bad)++;
				}
				char text[40];
				dmrDataUDP_t udp;
				dmrDataIP_t ip;
				dmrDataDecodeEtsiText(&dmrDataRxPacket, text, sizeof(text));
				if (dmrDataGetUDP(&dmrDataRxPacket, &udp) &&
						(udp.payload + udp.length > dmrDataRxPacket.data + dmrDataRxPacket.length))
				{
					(*bad)++;
				}
				if (dmrDataGetIP(&dmrDataRxPacket, &ip) && (ip.payload + ip.length > dmrDataRxPacket.data + dmrDataRxPacket.length))
				{
					(*bad)++;
				}
			}
		}
#if CONFIG_DMR_CONFIRMED
		dmrDataRxConfirmedEnd();
#endif
	}
}

static void testFuzz(void)
{
	fuzz_t f;
	long bad = 0, packets = 0;
	fuzz_init(&f, 0x444D5244u);
	fuzzDecoders(&f, fuzz_iters(30000), &bad);
	CHECK(bad == 0);
	bad = 0;
	fuzzBursts(&f, fuzz_iters(10000), &bad, &packets);
	CHECK(bad == 0 && packets > 0);
	printf("# dmr data fuzz: %ld packets reassembled from mutated bursts\n", packets);
}

int main(void)
{
	testCRC32();
	testTMSRoundTrip(false, 0);
	testTMSRoundTrip(true, 3);
	testPadding(DT_RATE_12_DATA);
	testPadding(DT_RATE_34_DATA);
	testPadding(DT_RATE_1_DATA);
	testTMSRates();
	testIPPackets();
	testRawPacket();
	testTMSAck();
	testRadioDeskVectors();
	testARS();
	testCSBK();
	testUDTLocation();
	testCSBKAnswers();
	testConfirmedRx();
	testSap3();
	testCRC9();
	testConfirmedTx();
	testAirtime();
	testFrames();
	testCodedFrames();
	testShortPayloads();
	testFuzz();

	printf("# %s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
	return failures ? 1 : 0;
}
