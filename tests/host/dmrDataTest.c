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

	// RadioDesk's test vector (device ID "11"), and the 12 byte PDU hbnet recognises a MOTOTRBO radio's by (000cf0)
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
	testFrames();
	testCodedFrames();

	printf("# %s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
	return failures ? 1 : 0;
}
