// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
/*
 * DMR packet data (ETSI TS 102 361-1 clause 8 / 9)
 */

#include <string.h>
#include "dmr/dmrData.h"

// Motorola CAI network: individual IDs are 12.x.y.z, groups 225.x.y.z
#define CAI_NETWORK_INDIVIDUAL		0x0C
#define CAI_NETWORK_GROUP			0xE1

// Compressed UDP/IP (TS 102 361-3) port IDs
#define UDPC_PORT_ID_ETSI_TEXT		1
#define UDPC_PORT_ID_LIP			2
#define UDPC_PORT_ID_MOTOROLA_TMS	98
#define DMR_UDP_PORT_LIP			5017
// IP address IDs (Tables 7.15 / 7.16)
#define UDPC_ID_RADIO_NETWORK		0
#define UDPC_ID_GROUP_NETWORK		2

DMR_DATA_BUFFER dmrDataPacket_t dmrDataRxPacket;

static uint16_t ipIdentification = 0;
DMR_DATA_BUFFER static uint8_t txPacket[DMR_DATA_MAX_PACKET];

// RX reassembly state
static bool    rxCollecting = false;
static uint8_t rxBlocksReceived;
static uint16_t rxBytes;

static void putId(uint8_t *p, uint32_t id)
{
	p[0] = (id >> 16) & 0xFF;
	p[1] = (id >> 8) & 0xFF;
	p[2] = id & 0xFF;
}

static uint32_t getId(const uint8_t *p)
{
	return (p[0] << 16) | (p[1] << 8) | p[2];
}

static void putU16(uint8_t *p, uint16_t v)
{
	p[0] = v >> 8;
	p[1] = v & 0xFF;
}

static void putU32(uint8_t *p, uint32_t v)
{
	putU16(p, v >> 16);
	putU16(&p[2], v & 0xFFFF);
}

static uint32_t getU32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

uint32_t dmrDataIdAddress(bool group, uint32_t id)
{
	return ((uint32_t)(group ? CAI_NETWORK_GROUP : CAI_NETWORK_INDIVIDUAL) << 24) | (id & 0xFFFFFF);
}

bool dmrDataIpIsId(uint32_t ip, bool group, uint32_t id)
{
	return ip == dmrDataIdAddress(group, id);
}

// Clause B.3.9: the octets are taken in pairs, least significant octet of each 16 bit word first,
// MSB first, generator 0x04C11DB7, initial remainder zero.
// A zero initial value makes the direct form equivalent to the spec's augmented form.
uint32_t dmrDataCRC32(const uint8_t *data, int length)
{
	uint32_t crc = 0;
	int paddedLength = (length + 1) & ~1;

	for (int i = 0; i < paddedLength; i++)
	{
		int idx = i ^ 1;// swap the octets of each 16 bit word
		crc ^= (uint32_t)((idx < length) ? data[idx] : 0) << 24;

		for (int b = 0; b < 8; b++)
		{
			crc = (crc & 0x80000000) ? ((crc << 1) ^ 0x04C11DB7) : (crc << 1);
		}
	}

	return crc;
}

// CRC-CCITT of the first 10 octets (B.3.7: polynomial 0x1021, initial value 0, inverted), stored MSB first, then masked
static uint16_t crc16(const uint8_t *buf, uint8_t mask)
{
	uint16_t crc = 0;

	for (int i = 0; i < 10; i++)
	{
		crc ^= buf[i] << 8;
		for (int bit = 0; bit < 8; bit++)
		{
			crc = (crc & 0x8000) ? ((crc << 1) ^ 0x1021) : (crc << 1);
		}
	}

	return ~crc ^ ((mask << 8) | mask);
}

static void addCRC16(uint8_t *buf, uint8_t mask)
{
	uint16_t crc = crc16(buf, mask);

	buf[10] = crc >> 8;
	buf[11] = crc & 0xFF;
}

static bool checkCRC16(const uint8_t *in, uint8_t mask)
{
	uint16_t crc = crc16(in, mask);

	return (in[10] == (crc >> 8)) && (in[11] == (crc & 0xFF));
}

bool dmrDataCheckCRC16(const uint8_t *burst12, uint8_t mask)
{
	return checkCRC16(burst12, mask);
}

static void setBurst(dmrBurst_t *burst, uint8_t dataType, const uint8_t *payload, uint8_t length)
{
	burst->dataType = dataType;
	burst->length = length;
	burst->flags = DMR_BURST_FLAG_MCU_CRC;
	memcpy(burst->payload, payload, length);
}

int dmrDataBuildCSBK(const uint8_t csbk[10], dmrBurst_t *out)
{
	uint8_t buf[12];

	memcpy(buf, csbk, 10);
	addCRC16(buf, 0xA5);
	setBurst(out, DT_CSBK, buf, 12);

	return 1;
}

// Call alert and radio check, as MMDVMHost decodes them: target ID in bytes 4-6, source in 7-9. The radio check
// answer is the request echoed back with byte 3 cleared, so its first ID is the radio answering
int dmrDataBuildCSBKAnswer(const uint8_t *csbk, uint32_t src, dmrBurst_t *out)
{
	uint8_t opcode = csbk[0] & 0x3F;
	uint8_t buf[10];

	if (((csbk[1] != DMR_FID_ETSI) && (csbk[1] != DMR_FID_MOTOROLA)) || (getId(&csbk[4]) != src) || (getId(&csbk[7]) == src))
	{
		return 0;
	}

	memcpy(buf, csbk, 10);
	buf[0] = 0x80;// Last Block, not protected

	if ((opcode == DMR_CSBKO_RADIO_CHECK) && (csbk[3] == 0x80))
	{
		buf[0] |= DMR_CSBKO_RADIO_CHECK;
		buf[3] = 0x00;
	}
	else if (opcode == DMR_CSBKO_CALL_ALERT)
	{
		buf[0] |= DMR_CSBKO_CALL_ALERT_ACK;
		memcpy(&buf[4], &csbk[7], 3);// to the caller
		putId(&buf[7], src);
	}
	else
	{
		return 0;
	}

	return dmrDataBuildCSBK(buf, out);
}

// User data octets in an unconfirmed block of the type, 0 if it isn't a data block type
int dmrDataBlockLength(uint8_t blockType)
{
	switch (blockType)
	{
		case DT_RATE_12_DATA:
			return 12;
		case DT_RATE_34_DATA:
			return 18;
		case DT_RATE_1_DATA:
			return 24;
		default:
			return 0;
	}
}

int dmrDataBuildPreambles(bool group, uint32_t dst, uint32_t src, int count, int follow, dmrBurst_t *out)
{
	uint8_t buf[10];

	for (int i = 0; i < count; i++)
	{
		int cbf = count - 1 - i + follow;

		buf[0] = 0x80 | DMR_CSBKO_PREAMBLE;// Last Block
		buf[1] = 0x00;// FID
		buf[2] = 0x80 | (group ? 0x40 : 0x00);// Data follows, G/I
		buf[3] = (uint8_t)((cbf > 255) ? 255 : cbf);// CSBK blocks to follow
		putId(&buf[4], dst);
		putId(&buf[7], src);
		dmrDataBuildCSBK(buf, out++);
	}
	return (count > 0) ? count : 0;
}

int dmrDataPacketBursts(int length, uint8_t blockType)
{
	int blockLen = dmrDataBlockLength(blockType);

	if ((blockLen == 0) || (length <= 0))
	{
		return 0;
	}
	int blocks = (length + 4 + blockLen - 1) / blockLen;

	if ((((blocks * blockLen) - 4 - length) > 31) || (blocks > 127) || ((blocks * blockLen) > DMR_DATA_MAX_PACKET))
	{
		return 0;
	}
	return 1 + blocks;
}

// Unconfirmed packet of Rate 1/2, Rate 3/4 or Rate 1 blocks, preceded by preamble CSBKs
int dmrDataBuildPacket(uint8_t dpf, uint8_t sap, bool group, uint32_t dst, uint32_t src,
						const uint8_t *data, int length, uint8_t blockType, int preambles, dmrBurst_t *out, int maxBursts)
{
	int blockLen = dmrDataBlockLength(blockType);

	if (blockLen == 0)
	{
		return 0;
	}

	int blocks = (length + 4 + blockLen - 1) / blockLen;
	int padOctets = (blocks * blockLen) - 4 - length;
	int bursts = preambles + 1 + blocks;
	uint8_t buf[12];

	if ((length <= 0) || (padOctets > 31) || (blocks > 127) || (bursts > maxBursts) || ((blocks * blockLen) > DMR_DATA_MAX_PACKET))
	{
		return 0;
	}

	out += dmrDataBuildPreambles(group, dst, src, preambles, 1 + blocks, out);

	// Figure 8.3, unconfirmed data header
	buf[0] = (group ? 0x80 : 0x00) | ((padOctets & 0x10) ? 0x10 : 0x00) | (dpf & 0x0F);
	buf[1] = (sap << 4) | (padOctets & 0x0F);
	putId(&buf[2], dst);
	putId(&buf[5], src);
	buf[8] = 0x80 | blocks;// Full message
	buf[9] = 0x00;// FSN: unconfirmed single fragment
	addCRC16(buf, 0xCC);
	setBurst(out++, DT_DATA_HEADER, buf, 12);

	memcpy(txPacket, data, length);
	memset(&txPacket[length], 0, padOctets);
	uint32_t crc = dmrDataCRC32(txPacket, length + padOctets);
	uint8_t *crcPos = &txPacket[length + padOctets];
	// Least significant CRC octet first (figure B.8C)
	crcPos[0] = crc & 0xFF;
	crcPos[1] = (crc >> 8) & 0xFF;
	crcPos[2] = (crc >> 16) & 0xFF;
	crcPos[3] = (crc >> 24) & 0xFF;

	for (int i = 0; i < blocks; i++)
	{
		setBurst(out++, blockType, &txPacket[i * blockLen], blockLen);
	}

	return bursts;
}

// Writes value's low bits (width of them) at bit position pos of buf, MSB first
static void putBits(uint8_t *buf, int pos, int width, uint32_t value)
{
	for (int i = width - 1; i >= 0; i--, pos++)
	{
		if (value & (1u << i))
		{
			buf[pos >> 3] |= (0x80 >> (pos & 7));
		}
	}
}

int dmrDataBuildUDTLocation(bool group, uint32_t dst, uint32_t src, const dmrDataLocation_t *location, int preambles,
							dmrBurst_t *out, int maxBursts)
{
	int bursts = preambles + 2;
	uint8_t buf[12];

	if (bursts > maxBursts)
	{
		return 0;
	}

	for (int i = 0; i < preambles; i++)
	{
		buf[0] = 0x80 | DMR_CSBKO_PREAMBLE;// Last Block
		buf[1] = 0x00;// FID
		buf[2] = 0x80 | (group ? 0x40 : 0x00);// Data follows, G/I
		buf[3] = bursts - 1 - i;// CSBK blocks to follow
		putId(&buf[4], dst);
		putId(&buf[7], src);
		dmrDataBuildCSBK(buf, out++);
	}

	// UDT header (TS 102 361-4): G/I, DPF 0, SAP 0, format, addresses, no pad nibbles, one appended block, opcode
	buf[0] = (group ? 0x80 : 0x00) | DMR_DPF_UDT;
	buf[1] = (DMR_SAP_UDT << 4) | DMR_UDT_FORMAT_NMEA;
	putId(&buf[2], dst);
	putId(&buf[5], src);
	buf[8] = 0x00;// pad nibbles 0, appended blocks - 1 = 0
	buf[9] = DMR_UDT_OPCODE;// SF 0, PF 0
	addCRC16(buf, 0xCC);
	setBurst(out++, DT_DATA_HEADER, buf, 12);

	// The appended block: short NMEA location, then the CRC of the 10 octets
	memset(buf, 0, sizeof(buf));
	putBits(buf, 0, 1, 0);// C: not encrypted
	putBits(buf, 1, 1, location->north ? 1 : 0);
	putBits(buf, 2, 1, location->east ? 1 : 0);
	putBits(buf, 3, 1, location->fixValid ? 1 : 0);
	putBits(buf, 4, 7, (location->speedKnots > DMR_LOCATION_SPEED_UNKNOWN) ? DMR_LOCATION_SPEED_UNKNOWN : location->speedKnots);
	putBits(buf, 11, 7, location->latDegrees);
	putBits(buf, 18, 6, location->latMinutes10000 / 10000);
	putBits(buf, 24, 14, location->latMinutes10000 % 10000);
	putBits(buf, 38, 8, location->lonDegrees);
	putBits(buf, 46, 6, location->lonMinutes10000 / 10000);
	putBits(buf, 52, 14, location->lonMinutes10000 % 10000);
	putBits(buf, 66, 5, location->utcHours);
	putBits(buf, 71, 6, location->utcMinutes);
	putBits(buf, 77, 3, location->utcSeconds / 10);
	addCRC16(buf, 0x00);
	setBurst(out++, DT_RATE_12_DATA, buf, 12);

	return bursts;
}

static uint16_t onesComplementSum(uint32_t sum, const uint8_t *data, int length)
{
	for (int i = 0; i < length; i += 2)
	{
		sum += (data[i] << 8) | (((i + 1) < length) ? data[i + 1] : 0);
	}

	while (sum >> 16)
	{
		sum = (sum & 0xFFFF) + (sum >> 16);
	}

	return sum;
}

DMR_DATA_BUFFER static uint8_t ipPacket[DMR_DATA_MAX_PACKET];
#define IP_PDU_MAX		(DMR_DATA_MAX_PACKET - 16)	// user data that still fits the blocks with padding and the CRC-32

static uint8_t udpCompression = DMR_UDPC_AUTO;
static dmrDataIpTxStats_t ipTxStats;

static uint16_t getU16(const uint8_t *p)
{
	return (uint16_t)((p[0] << 8) | p[1]);
}

// The TTL on the air: 1 to a talkgroup whatever was asked (the air is the group's one hop: a receiving gateway must not
// route it on), else the one given or the radio's own, 64
static uint8_t ipTtl(bool group, uint8_t ttl)
{
	return group ? 1 : (ttl != DMR_IP_TTL_DEFAULT) ? ttl : 64;
}

// The IPv4 header in front of the length bytes of layer 4 already at ipPacket[20]. A UDP checksum covers the addresses,
// so it is worked out here over the ones in the header; ICMP and SCTP checksums don't, they are kept
static int ipHeader(uint32_t dstIp, uint32_t srcIp, uint8_t ttl, uint8_t protocol, int length, uint16_t ipId)
{
	int total = 20 + length;

	memset(ipPacket, 0, 20);
	ipPacket[0] = 0x45;// IPv4, 20 byte header
	putU16(&ipPacket[2], total);
	putU16(&ipPacket[4], ipId);
	ipPacket[8] = ttl;
	ipPacket[9] = protocol;
	putU32(&ipPacket[12], srcIp);
	putU32(&ipPacket[16], dstIp);
	putU16(&ipPacket[10], ~onesComplementSum(0, ipPacket, 20));

	if (protocol == DMR_IP_PROTO_UDP)
	{
		// Over the pseudo header (addresses, protocol, UDP length) and the datagram
		putU16(&ipPacket[26], 0);
		uint16_t sum = onesComplementSum(DMR_IP_PROTO_UDP + length, &ipPacket[12], 8);
		sum = ~onesComplementSum(sum, &ipPacket[20], length);
		putU16(&ipPacket[26], (sum == 0) ? 0xFFFF : sum);
	}

	return total;
}

// Compressed UDP/IPv4 port IDs (TS 102 361-3 Tables 7.17 / 7.18): the standard's two, and the manufacturer specific 98
// MOTOTRBO uses for TMS (seen in an over the air capture, dmrDataTest)
static const struct { uint8_t id; uint16_t port; } udpcPorts[] = {
	{ UDPC_PORT_ID_ETSI_TEXT, DMR_UDP_PORT_ETSI_TEXT },
	{ UDPC_PORT_ID_LIP, DMR_UDP_PORT_LIP },
	{ UDPC_PORT_ID_MOTOROLA_TMS, DMR_UDP_PORT_TMS },
};

static uint8_t udpcPortId(uint16_t port)
{
	for (unsigned i = 0; i < sizeof(udpcPorts) / sizeof(udpcPorts[0]); i++)
	{
		if (udpcPorts[i].port == port)
		{
			return udpcPorts[i].id;
		}
	}
	return 0;// in an extended header
}

void dmrDataSetUdpCompression(uint8_t mode)
{
	udpCompression = (mode <= DMR_UDPC_ALL) ? mode : DMR_UDPC_AUTO;
}

uint8_t dmrDataGetUdpCompression(void)
{
	return CONFIG_DMR_UDP_COMPRESS ? udpCompression : DMR_UDPC_OFF;
}

static bool isAppPort(uint16_t port)
{
	return (port >= DMR_APP_PORT_FIRST) && (port <= DMR_APP_PORT_LAST);
}

bool dmrDataUdpCompress(uint16_t srcPort, uint16_t dstPort)
{
	switch (dmrDataGetUdpCompression())
	{
		case DMR_UDPC_ALL:
			return true;
		case DMR_UDPC_AUTO:
			return !isAppPort(srcPort) && !isAppPort(dstPort);
		default:
			return false;
	}
}

int dmrDataCompressUDPHeader(uint16_t ipId, bool group, uint16_t srcPort, uint16_t dstPort, uint8_t out[DMR_UDPC_HEADER_MAX])
{
	uint8_t spid = udpcPortId(srcPort);
	uint8_t dpid = udpcPortId(dstPort);
	int n = 5;

	putU16(&out[0], ipId);
	out[2] = (UDPC_ID_RADIO_NETWORK << 4) | (group ? UDPC_ID_GROUP_NETWORK : UDPC_ID_RADIO_NETWORK);// SAID, DAID
	out[3] = spid;// opcode 00: its MSB here, its LSB in the next octet
	out[4] = dpid;
	// Extended header 1 holds the source port when it has no ID, else the destination port; extended header 2 the
	// destination port when neither has one (Tables 7.20 / 7.21)
	if (spid == 0)
	{
		putU16(&out[n], srcPort);
		n += 2;
	}
	if (dpid == 0)
	{
		putU16(&out[n], dstPort);
		n += 2;
	}
	return n;
}

uint16_t dmrDataNextIpId(void)
{
	return ipIdentification++;
}

// The compressed header has no addresses, the receiver rebuilds the IDs' CAI addresses: only a packet that has those
// may go compressed. Another address (an off-net destination behind a gateway radio) needs the full header, in every
// compression mode
static bool addressesFromIds(bool group, uint32_t dst, uint32_t src, uint32_t dstIp, uint32_t srcIp)
{
	return (dstIp == dmrDataIdAddress(group, dst)) && (srcIp == dmrDataIdAddress(false, src));
}

// UDP into ipPacket: compressed header and payload, or the IPv4 and UDP headers and payload. payload may not be in
// ipPacket. Returns the length, 0 when it doesn't fit
static int encodeUDP(bool group, uint32_t dst, uint32_t src, uint32_t dstIp, uint32_t srcIp, uint8_t ttl, uint16_t srcPort,
						uint16_t dstPort, const uint8_t *payload, int length, bool compress, uint16_t ipId, uint8_t *sap)
{
	if (CONFIG_DMR_UDP_COMPRESS && compress && addressesFromIds(group, dst, src, dstIp, srcIp))
	{
		uint8_t h[DMR_UDPC_HEADER_MAX];
		int n = dmrDataCompressUDPHeader(ipId, group, srcPort, dstPort, h);

		if ((n + length) > IP_PDU_MAX)
		{
			return 0;
		}
		memmove(&ipPacket[n], payload, length);
		memcpy(ipPacket, h, n);
		*sap = DMR_SAP_UDPIP_COMPRESSION;
		return n + length;
	}

	if ((28 + length) > IP_PDU_MAX)
	{
		return 0;
	}
	memmove(&ipPacket[28], payload, length);
	putU16(&ipPacket[20], srcPort);
	putU16(&ipPacket[22], dstPort);
	putU16(&ipPacket[24], 8 + length);
	*sap = DMR_SAP_IP;
	return ipHeader(dstIp, srcIp, ipTtl(group, ttl), DMR_IP_PROTO_UDP, 8 + length, ipId);
}

int dmrDataEncodeIP(bool group, uint32_t dst, uint32_t src, uint32_t dstIp, uint32_t srcIp, uint8_t ttl, uint8_t protocol,
					const uint8_t *l4, int length, bool compress, uint16_t ipId, const uint8_t **pdu, uint8_t *sap)
{
	*pdu = ipPacket;
	if (protocol == DMR_IP_PROTO_UDP)
	{
		if (length < 8)
		{
			return 0;
		}
		int udpLength = getU16(&l4[4]);

		// The compressed header has no length: the receiver takes it from the packet, so only a datagram whose UDP
		// length agrees with what we have goes compressed (octets past the UDP length are dropped, as padding)
		bool fits = CONFIG_DMR_UDP_COMPRESS && compress && (udpLength >= 8) && (udpLength <= length) &&
				addressesFromIds(group, dst, src, dstIp, srcIp);

		return encodeUDP(group, dst, src, dstIp, srcIp, ttl, getU16(&l4[0]), getU16(&l4[2]), &l4[8],
							fits ? (udpLength - 8) : (length - 8), fits, ipId, sap);
	}

	if ((length < 1) || ((20 + length) > IP_PDU_MAX))
	{
		return 0;
	}
	memmove(&ipPacket[20], l4, length);
	*sap = DMR_SAP_IP;
	return ipHeader(dstIp, srcIp, ipTtl(group, ttl), protocol, length, ipId);
}

static int blocksFor(int length, int perBlock)
{
	return (perBlock > 0) ? ((length + 4 + perBlock - 1) / perBlock) : 0;
}

// The user data with the full header is 28 octets longer than the compressed one's 5 + extended headers
static int fullUDPLength(const uint8_t *pdu, int length)
{
	return length - 5 - (((pdu[3] & 0x7F) == 0) ? 2 : 0) - (((pdu[4] & 0x7F) == 0) ? 2 : 0) + 28;
}

void dmrDataCountIpTx(uint8_t sap, const uint8_t *pdu, int length, uint8_t blockType, bool confirmed)
{
	if (sap != DMR_SAP_UDPIP_COMPRESSION)
	{
		ipTxStats.sap4++;
		return;
	}
	int per = confirmed ? dmrDataConfirmedBlockData(blockType) : dmrDataBlockLength(blockType);
	int full = fullUDPLength(pdu, length);

	ipTxStats.sap3++;
	ipTxStats.octetsSaved += full - length;
	ipTxStats.blocksSaved += blocksFor(full, per) - blocksFor(length, per);
}

void dmrDataGetIpTxStats(dmrDataIpTxStats_t *stats)
{
	*stats = ipTxStats;
}

void dmrDataResetIpTxStats(void)
{
	memset(&ipTxStats, 0, sizeof(ipTxStats));
}

int dmrDataBuildIP(bool group, uint32_t dst, uint32_t src, uint8_t protocol, const uint8_t *l4, int length,
					uint8_t blockType, int preambles, dmrBurst_t *out, int maxBursts)
{
	const uint8_t *pdu;
	uint8_t sap;
	bool compress = (protocol == DMR_IP_PROTO_UDP) && (length >= 8) && dmrDataUdpCompress(getU16(&l4[0]), getU16(&l4[2]));
	int n = dmrDataEncodeIP(group, dst, src, dmrDataIdAddress(group, dst), dmrDataIdAddress(false, src), DMR_IP_TTL_DEFAULT,
							protocol, l4, length, compress, dmrDataNextIpId(), &pdu, &sap);

	if (n <= 0)
	{
		return 0;
	}
	int bursts = dmrDataBuildPacket(DMR_DPF_UNCONFIRMED, sap, group, dst, src, pdu, n, blockType, preambles, out, maxBursts);
	if (bursts > 0)
	{
		dmrDataCountIpTx(sap, pdu, n, blockType, false);
	}
	return bursts;
}

int dmrDataBuildUDP(bool group, uint32_t dst, uint32_t src, uint16_t srcPort, uint16_t dstPort,
						const uint8_t *payload, int length, uint8_t blockType, int preambles, dmrBurst_t *out, int maxBursts)
{
	uint8_t sap;

	if (length < 0)
	{
		return 0;
	}
	int n = encodeUDP(group, dst, src, dmrDataIdAddress(group, dst), dmrDataIdAddress(false, src), DMR_IP_TTL_DEFAULT,
						srcPort, dstPort, payload, length, CONFIG_DMR_UDP_COMPRESS && dmrDataUdpCompress(srcPort, dstPort),
						dmrDataNextIpId(), &sap);
	if (n <= 0)
	{
		return 0;
	}
	int bursts = dmrDataBuildPacket(DMR_DPF_UNCONFIRMED, sap, group, dst, src, ipPacket, n, blockType, preambles, out, maxBursts);
	if (bursts > 0)
	{
		dmrDataCountIpTx(sap, ipPacket, n, blockType, false);
	}
	return bursts;
}

#if CONFIG_DMR_TMS
// Motorola TMS simple text message: length, header (0xA0, or 0xE0 to request an ACK), no address,
// sequence number, encoding, then the text as UTF-16LE
int dmrDataBuildTMS(bool group, uint32_t dst, uint32_t src, const char *text, uint8_t seq, bool ackRequested,
						uint8_t blockType, int preambles, dmrBurst_t *out, int maxBursts)
{
	DMR_DATA_BUFFER static uint8_t tms[6 + (2 * 140)];
	int textLen = strlen(text);

	if (textLen > 140)
	{
		textLen = 140;
	}

	int length = 6 + (2 * textLen);
	putU16(&tms[0], length - 2);
	tms[2] = ackRequested ? 0xE0 : 0xA0;
	tms[3] = 0x00;
	tms[4] = 0x80 | (seq & 0x1F);
	tms[5] = 0x04;

	for (int i = 0; i < textLen; i++)
	{
		tms[6 + (2 * i)] = (uint8_t)text[i];
		tms[7 + (2 * i)] = 0x00;
	}

	return dmrDataBuildUDP(group, dst, src, DMR_UDP_PORT_TMS, DMR_UDP_PORT_TMS, tms, length, blockType, preambles, out, maxBursts);
}
#endif

// Figure 8.5 response header (G/I and A clear, class / type / status in octet 9); a selective ACK appends one rate 1/2
// block: the 64 flags in octets 0-7 (Figure 8.17), the CRC-32 of those in octets 8-11 as a last block carries it
int dmrDataBuildResponse(uint8_t sap, uint32_t dst, uint32_t src, uint8_t response, const uint8_t flags[8], dmrBurst_t *out)
{
	uint8_t buf[12];

	buf[0] = DMR_DPF_RESPONSE;
	buf[1] = sap << 4;
	putId(&buf[2], dst);
	putId(&buf[5], src);
	buf[8] = (flags != NULL) ? 1 : 0;// blocks to follow
	buf[9] = response;
	addCRC16(buf, 0xCC);
	setBurst(out, DT_DATA_HEADER, buf, 12);
	if (flags == NULL)
	{
		return 1;
	}

	memcpy(buf, flags, 8);
	uint32_t crc = dmrDataCRC32(buf, 8);
	buf[8] = crc & 0xFF;
	buf[9] = (crc >> 8) & 0xFF;
	buf[10] = (crc >> 16) & 0xFF;
	buf[11] = (crc >> 24) & 0xFF;
	setBurst(out + 1, DT_RATE_12_DATA, buf, 12);
	return 2;
}

int dmrDataBuildResponseAck(uint8_t sap, uint32_t dst, uint32_t src, uint8_t sendSeq, dmrBurst_t *out)
{
	return dmrDataBuildResponse(sap, dst, src, DMR_RESPONSE_ACK(sendSeq), NULL, out);
}

// --- Confirmed data (TS 102 361-1 8.2.2.2, 9.2.1, B.3.10) ---

int dmrDataConfirmedBlockData(uint8_t blockType)
{
	int n = dmrDataBlockLength(blockType);

	return (n > 0) ? (n - 2) : 0;// the serial number and the CRC-9 take two octets
}

int dmrDataConfirmedBlocks(int length, uint8_t blockType)
{
	int blocks = blocksFor(length, dmrDataConfirmedBlockData(blockType));

	return (blocks <= DMR_CONFIRMED_MAX_BLOCKS) ? blocks : 0;
}

static uint16_t crc9Mask(uint8_t blockType)
{
	switch (blockType)
	{
		case DT_RATE_12_DATA:
			return DMR_CRC9_MASK_RATE_12;
		case DT_RATE_34_DATA:
			return DMR_CRC9_MASK_RATE_34;
		default:
			return DMR_CRC9_MASK_RATE_1;
	}
}

// x^9 M(x) mod G9(x), G9 = x^9 + x^6 + x^4 + x^3 + 1, M(x) the data bits MSB first then the 7 serial number bits, zero
// initial remainder; plus I9(x) = all ones (B.25)
uint16_t dmrDataCRC9(const uint8_t *data, int length, uint8_t serial)
{
	uint16_t crc = 0;

	for (int i = 0; i < (length * 8) + 7; i++)
	{
		int bit = (i < (length * 8)) ? ((data[i >> 3] >> (7 - (i & 7))) & 1) : ((serial >> (6 - (i - (length * 8)))) & 1);
		int feedback = ((crc >> 8) & 1) ^ bit;

		crc = (crc << 1) & 0x1FF;
		if (feedback)
		{
			crc ^= 0x059;
		}
	}

	return crc ^ 0x1FF;
}

int dmrDataBuildConfirmed(uint8_t sap, uint32_t dst, uint32_t src, const uint8_t *pdu, int length, uint8_t blockType,
						uint8_t ns, bool firstTry, uint64_t blocks, int preambles, dmrBurst_t *out, int maxBursts)
{
	int blockLen = dmrDataBlockLength(blockType);
	int dataLen = blockLen - 2;
	int total = dmrDataConfirmedBlocks(length, blockType);

	if ((blockLen == 0) || (length <= 0) || (total == 0) || ((total * dataLen) > DMR_DATA_MAX_PACKET))
	{
		return 0;
	}

	uint64_t all = (total >= 64) ? ~0ULL : ((1ULL << total) - 1);
	uint64_t send = firstTry ? all : (blocks & all);
	int count = 0;

	for (int k = 0; k < total; k++)
	{
		count += (send >> k) & 1;
	}

	int padOctets = (total * dataLen) - 4 - length;
	int bursts = preambles + 1 + count;
	uint8_t buf[DMR_BURST_PAYLOAD_MAX];

	if ((count == 0) || (padOctets > 31) || (bursts > maxBursts))
	{
		return 0;
	}

	for (int i = 0; i < preambles; i++)
	{
		buf[0] = 0x80 | DMR_CSBKO_PREAMBLE;
		buf[1] = 0x00;
		buf[2] = 0x80;// data follows, individual
		buf[3] = bursts - 1 - i;
		putId(&buf[4], dst);
		putId(&buf[7], src);
		dmrDataBuildCSBK(buf, out++);
	}

	// C_HEAD (Table 9.10): individual, response requested, POC, DPF 3; SAP; addresses; F and the blocks in this
	// transmission (the POC stays the first try's, 9.3.22); S clear, N(S), FSN 1000 (a single fragment)
	buf[0] = 0x40 | ((padOctets & 0x10) ? 0x10 : 0x00) | DMR_DPF_CONFIRMED;
	buf[1] = (sap << 4) | (padOctets & 0x0F);
	putId(&buf[2], dst);
	putId(&buf[5], src);
	buf[8] = (firstTry ? 0x80 : 0x00) | count;
	buf[9] = ((ns & 0x07) << 4) | 0x08;
	addCRC16(buf, 0xCC);
	setBurst(out++, DT_DATA_HEADER, buf, 12);

	memmove(txPacket, pdu, length);
	memset(&txPacket[length], 0, padOctets);
	uint32_t crc = dmrDataCRC32(txPacket, length + padOctets);
	uint8_t *crcPos = &txPacket[length + padOctets];
	crcPos[0] = crc & 0xFF;
	crcPos[1] = (crc >> 8) & 0xFF;
	crcPos[2] = (crc >> 16) & 0xFF;
	crcPos[3] = (crc >> 24) & 0xFF;

	uint16_t mask = crc9Mask(blockType);
	for (int k = 0; k < total; k++)
	{
		if (!((send >> k) & 1))
		{
			continue;
		}
		const uint8_t *d = &txPacket[k * dataLen];
		uint16_t crc9 = dmrDataCRC9(d, dataLen, k) ^ mask;

		buf[0] = (k << 1) | ((crc9 >> 8) & 1);// serial number, CRC-9 MSB
		buf[1] = crc9 & 0xFF;
		memcpy(&buf[2], d, dataLen);
		setBurst(out++, blockType, buf, blockLen);
	}

	return bursts;
}

#if CONFIG_DMR_TMS
// As a MOTOTRBO radio sends it (ok-dmrlib's capture: 00 04 9F 00 95 20): first header 0x9F (more headers, control,
// type 0xF), no address, then the sequence number: its low 5 bits, and only when it needs them bits 5-6 in a second
// octet (extension bit set in the first)
int dmrDataBuildTMSAck(uint32_t dst, uint32_t src, uint8_t seq, int preambles, dmrBurst_t *out, int maxBursts)
{
	uint8_t ack[6] = { 0x00, 0x03, 0x9F, 0x00, (seq & 0x1F), 0x00 };
	int length = 5;

	seq &= 0x7F;
	if (seq > 0x1F)
	{
		ack[1] = 0x04;
		ack[4] |= 0x80;
		ack[5] = seq & 0x60;
		length = 6;
	}

	return dmrDataBuildUDP(false, dst, src, DMR_UDP_PORT_TMS, DMR_UDP_PORT_TMS, ack, length, DT_RATE_12_DATA, preambles, out, maxBursts);
}
#endif

// Defined short data header (DD_HEAD, TS 102 361-1 9.2.12, DPF 13): G/I, A (response requested), the appended
// blocks' high bits and the DPF; SAP 10 (short data) and their low bits; the addresses; the DD format (0x03, ISO
// 8859-1), S (selective ACK, 0) and F (full message, 1); the bit padding of the last block. Then rate 1/2 blocks:
// the text, zero padding, the CRC-32 as in an unconfirmed packet
int dmrDataBuildEtsiText(bool group, uint32_t dst, uint32_t src, const char *text, bool responseRequested,
						int preambles, dmrBurst_t *out, int maxBursts)
{
	const int blockLen = 12;
	int length = (int)strlen(text);

	if (length > DMR_ETSI_TEXT_MAX)
	{
		length = DMR_ETSI_TEXT_MAX;
	}

	int blocks = (length + 4 + blockLen - 1) / blockLen;
	int padOctets = (blocks * blockLen) - 4 - length;
	int bursts = preambles + 1 + blocks;
	uint8_t buf[12];

	if ((length <= 0) || (blocks > 63) || (bursts > maxBursts) || ((blocks * blockLen) > DMR_DATA_MAX_PACKET))
	{
		return 0;
	}

	for (int i = 0; i < preambles; i++)
	{
		buf[0] = 0x80 | DMR_CSBKO_PREAMBLE;// Last Block
		buf[1] = 0x00;// FID
		buf[2] = 0x80 | (group ? 0x40 : 0x00);// Data follows, G/I
		buf[3] = bursts - 1 - i;// CSBK blocks to follow
		putId(&buf[4], dst);
		putId(&buf[7], src);
		dmrDataBuildCSBK(buf, out++);
	}

	buf[0] = (group ? 0x80 : 0x00) | ((responseRequested && !group) ? 0x40 : 0x00) | (((blocks >> 4) & 0x03) << 4) |
			 DMR_DPF_DEFINED_SHORT;
	buf[1] = (DMR_SAP_SHORT_DATA << 4) | (blocks & 0x0F);
	putId(&buf[2], dst);
	putId(&buf[5], src);
	buf[8] = (DMR_DD_ISO8859_1 << 2) | 0x01;// S 0, F 1
	buf[9] = padOctets * 8;
	addCRC16(buf, 0xCC);
	setBurst(out++, DT_DATA_HEADER, buf, 12);

	memcpy(txPacket, text, length);
	memset(&txPacket[length], 0, padOctets);
	uint32_t crc = dmrDataCRC32(txPacket, length + padOctets);
	uint8_t *crcPos = &txPacket[length + padOctets];
	crcPos[0] = crc & 0xFF;
	crcPos[1] = (crc >> 8) & 0xFF;
	crcPos[2] = (crc >> 16) & 0xFF;
	crcPos[3] = (crc >> 24) & 0xFF;

	for (int i = 0; i < blocks; i++)
	{
		setBurst(out++, DT_RATE_12_DATA, &txPacket[i * blockLen], blockLen);
	}

	return bursts;
}

// ETSI IP text (TS 102 361-3 V1.3.1 5.7.1): the text as UTF-16BE (plane 0; Latin-1 here, one code unit per
// character, no byte order mark, no terminator) in a UDP datagram from and to port 5016, with the full IPv4 / UDP
// header (SAP 4, as every receiver of the IP bearer reads it; the compressed header of 5.6 is optional)
int dmrDataIPTextUDP(const char *text, uint8_t l4[DMR_ETSI_TEXT_UDP_MAX])
{
	int n = 0;

	for (; (text[n] != '\0') && (n < DMR_ETSI_TEXT_MAX); n++)
	{
		l4[8 + (2 * n)] = 0x00;
		l4[8 + (2 * n) + 1] = (uint8_t)text[n];
	}
	if (n == 0)
	{
		return 0;
	}
	putU16(&l4[0], DMR_UDP_PORT_ETSI_TEXT);
	putU16(&l4[2], DMR_UDP_PORT_ETSI_TEXT);
	putU16(&l4[4], 8 + (2 * n));
	putU16(&l4[6], 0);// computed with the IPv4 header
	return 8 + (2 * n);
}

int dmrDataBuildIPText(bool group, uint32_t dst, uint32_t src, const char *text, uint8_t blockType, int preambles,
						dmrBurst_t *out, int maxBursts)
{
	uint8_t l4[DMR_ETSI_TEXT_UDP_MAX];
	const uint8_t *pdu;
	uint8_t sap;
	int length = dmrDataIPTextUDP(text, l4);
	// Always the full header, whatever the compression setting (docs/spec/dmr-data.md 5.3)
	int n = (length > 0) ? dmrDataEncodeIP(group, dst, src, dmrDataIdAddress(group, dst), dmrDataIdAddress(false, src),
											DMR_IP_TTL_DEFAULT, DMR_IP_PROTO_UDP, l4, length, false, dmrDataNextIpId(),
											&pdu, &sap) : 0;

	if (n <= 0)
	{
		return 0;
	}
	int bursts = dmrDataBuildPacket(DMR_DPF_UNCONFIRMED, sap, group, dst, src, pdu, n, blockType, preambles, out, maxBursts);
	if (bursts > 0)
	{
		dmrDataCountIpTx(sap, pdu, n, blockType, false);
	}
	return bursts;
}

// A Unicode code point as Latin-1, '?' outside it; control characters are dropped (0)
static char latin1(uint32_t cp)
{
	if (cp < 0x20)
	{
		return 0;
	}
	return (cp <= 0xFF) ? (char)cp : '?';
}

static void textPut(char *text, int textSize, int *n, char c)
{
	if ((c != 0) && (*n < (textSize - 1)))
	{
		text[(*n)++] = c;
	}
}

// UTF-16 code units from big or little endian octet pairs; a surrogate pair is one character
static void decodeUTF16(const uint8_t *d, int length, bool bigEndian, char *text, int textSize, int *n)
{
	for (int i = 0; (i + 1) < length; i += 2)
	{
		uint16_t u = bigEndian ? ((d[i] << 8) | d[i + 1]) : ((d[i + 1] << 8) | d[i]);

		if (u == 0)
		{
			break;// padding
		}
		if ((u >= 0xD800) && (u <= 0xDBFF))
		{
			i += 2;// the low surrogate follows
			textPut(text, textSize, n, '?');
			continue;
		}
		textPut(text, textSize, n, latin1(u));
	}
}

static void decodeUTF8(const uint8_t *d, int length, char *text, int textSize, int *n)
{
	for (int i = 0; i < length; i++)
	{
		uint8_t c = d[i];

		if (c == 0)
		{
			break;
		}
		if (c < 0x80)
		{
			textPut(text, textSize, n, latin1(c));
		}
		else if (((c & 0xE0) == 0xC0) && ((i + 1) < length))
		{
			textPut(text, textSize, n, latin1(((uint32_t)(c & 0x1F) << 6) | (d[i + 1] & 0x3F)));
			i++;
		}
		else if ((c & 0xC0) != 0x80)
		{
			textPut(text, textSize, n, '?');// three or four octets: outside Latin-1
			i += ((c & 0xF0) == 0xE0) ? 2 : 3;
		}
	}
}

// Characters of 7 bits packed MSB first, bits bits in all
static void decode7bit(const uint8_t *d, int bits, char *text, int textSize, int *n)
{
	for (int pos = 0; (pos + 7) <= bits; pos += 7)
	{
		uint8_t c = 0;

		for (int b = 0; b < 7; b++)
		{
			int bit = pos + b;

			c = (c << 1) | ((d[bit >> 3] >> (7 - (bit & 7))) & 1);
		}
		if (c == 0)
		{
			break;
		}
		textPut(text, textSize, n, latin1(c));
	}
}

// CRC-CCITT over any length (B.3.7), unmasked: the UDT appended blocks' check
static uint16_t crcCCITT(const uint8_t *buf, int length)
{
	uint16_t crc = 0;

	for (int i = 0; i < length; i++)
	{
		crc ^= buf[i] << 8;
		for (int bit = 0; bit < 8; bit++)
		{
			crc = (crc & 0x8000) ? ((crc << 1) ^ 0x1021) : (crc << 1);
		}
	}

	return ~crc;
}

bool dmrDataDecodeEtsiText(const dmrDataPacket_t *packet, char *text, int textSize)
{
	const uint8_t *d = packet->data;
	int length;
	int n = 0;
	uint8_t format = packet->ddFormat;

	if (textSize <= 0)
	{
		return false;
	}

	if (packet->dpf == DMR_DPF_DEFINED_SHORT)
	{
		length = packet->length;// the padding octets are gone already; bit padding below an octet only for 7-bit
		if (format == DMR_DD_7BIT)
		{
			decode7bit(d, (length * 8) - (packet->padBits & 7), text, textSize, &n);
		}
		else if ((format >= DMR_DD_ISO8859_1) && (format <= DMR_DD_ISO8859_LAST))
		{
			for (int i = 0; (i < length) && (d[i] != 0); i++)
			{
				// ISO 8859-1 is Latin-1 itself; the other parts share only ASCII with it
				textPut(text, textSize, &n, ((d[i] < 0x80) || (format == DMR_DD_ISO8859_1)) ? latin1(d[i]) : '?');
			}
		}
		else if (format == DMR_DD_UTF8)
		{
			decodeUTF8(d, length, text, textSize, &n);
		}
		else if ((format == DMR_DD_UTF16) || (format == DMR_DD_UTF16BE) || (format == DMR_DD_UTF16LE))
		{
			bool bigEndian = (format != DMR_DD_UTF16LE);

			if ((format == DMR_DD_UTF16) && (length >= 2) && (((d[0] == 0xFF) && (d[1] == 0xFE)) || ((d[0] == 0xFE) && (d[1] == 0xFF))))
			{
				bigEndian = (d[0] == 0xFE);
				d += 2;
				length -= 2;
			}
			decodeUTF16(d, length, bigEndian, text, textSize, &n);
		}
		else
		{
			return false;
		}
	}
	else if (packet->dpf == DMR_DPF_UDT)
	{
		// The appended blocks end with their CRC-CCITT; the pad nibbles come before it
		length = packet->length - 2;
		if ((length <= 0) || (crcCCITT(d, length) != (uint16_t)((d[length] << 8) | d[length + 1])))
		{
			return false;
		}
		int bits = (length * 8) - packet->padBits;

		if (format == DMR_UDT_FORMAT_ISO7)
		{
			decode7bit(d, bits, text, textSize, &n);
		}
		else if (format == DMR_UDT_FORMAT_ISO8)
		{
			for (int i = 0; (i < (bits / 8)) && (d[i] != 0); i++)
			{
				textPut(text, textSize, &n, latin1(d[i]));
			}
		}
		else if (format == DMR_UDT_FORMAT_UTF16BE)
		{
			decodeUTF16(d, bits / 8, true, text, textSize, &n);
		}
		else
		{
			return false;
		}
	}
	else
	{
		return false;
	}

	text[n] = '\0';
	return true;
}

bool dmrDataDecodeIPText(const uint8_t *payload, int length, char *text, int textSize)
{
	int n = 0;
	bool bigEndian = true;

	if ((textSize <= 0) || (length < 2))
	{
		return false;
	}
	// UTF-16BE is the format; a byte order mark some senders put first is honored
	if (((payload[0] == 0xFE) && (payload[1] == 0xFF)) || ((payload[0] == 0xFF) && (payload[1] == 0xFE)))
	{
		bigEndian = (payload[0] == 0xFE);
		payload += 2;
		length -= 2;
	}
	decodeUTF16(payload, length, bigEndian, text, textSize, &n);
	text[n] = '\0';
	return n > 0;
}

void dmrDataRxReset(void)
{
	rxCollecting = false;
}

#if CONFIG_DMR_CONFIRMED
// A confirmed packet being received, or held for its selective retry (the blocks stay in dmrDataRxPacket.data, at
// their serial number's place). Another packet's header ends it
static struct
{
	bool held;
	bool retry;							// this transmission is a selective retry of the held packet
	bool unmatched;						// F clear but nothing held to match: a retry of a packet we lost
	uint32_t src, dst;
	uint8_t sap, ns, total, padOctets, dataType;
	uint8_t arrived;					// data blocks in this transmission so far
	uint64_t good;						// blocks received with a good CRC-9
	uint64_t filled;					// blocks with data in place (a bad CRC-9 on the first try is kept: the CRC-32 decides)
} cnf;
static uint32_t crc9Mismatches;

static uint64_t cnfAll(void)
{
	return (cnf.total >= 64) ? ~0ULL : ((1ULL << cnf.total) - 1);
}

static void confirmedHeader(const dmrDataPacket_t *p)
{
	bool retry = !p->firstTry && cnf.held && (cnf.src == p->src) && (cnf.dst == p->dst) && (cnf.sap == p->sap) &&
					(cnf.ns == p->sendSeq);

	if (!retry)
	{
		// A first try, or a whole packet sent again (F clear without one held): its blocks come in order
		cnf.src = p->src;
		cnf.dst = p->dst;
		cnf.sap = p->sap;
		cnf.ns = p->sendSeq;
		cnf.total = p->blocksToFollow;
		cnf.padOctets = p->padOctets;
		cnf.good = 0;
		cnf.filled = 0;
	}
	cnf.held = true;
	cnf.retry = retry;
	cnf.unmatched = !p->firstTry && !retry;
	cnf.arrived = 0;
}

// The blocks so far: the CRC-32 decides once every place holds a block (our CRC-9 isn't trusted over it); a failure
// with bad or missing blocks asks for those, with every block good it is a NACK
static dmrDataRxResult_t confirmedFinish(dmrDataPacket_t *p)
{
	int dataLen = dmrDataConfirmedBlockData(cnf.dataType);
	int bytes = cnf.total * dataLen;

	if ((cnf.filled == cnfAll()) && (bytes >= (4 + cnf.padOctets)))
	{
		const uint8_t *c = &p->data[bytes - 4];
		uint32_t rxCrc = c[0] | (c[1] << 8) | (c[2] << 16) | ((uint32_t)c[3] << 24);

		if (dmrDataCRC32(p->data, bytes - 4) == rxCrc)
		{
			if (cnf.good != cnfAll())
			{
				crc9Mismatches++;
			}
			cnf.held = false;
			p->length = bytes - 4 - cnf.padOctets;
			p->padOctets = cnf.padOctets;
			p->blocksToFollow = cnf.total;
			p->dataType = cnf.dataType;
			return DMR_DATA_RX_PACKET;
		}
		if (cnf.good == cnfAll())
		{
			cnf.held = false;
			return DMR_DATA_RX_NACK;
		}
	}
	if (cnf.unmatched)
	{
		// Its serial numbers needn't be ours to place: a NACK has the sender send the whole packet again
		cnf.held = false;
		return DMR_DATA_RX_NACK;
	}
	cnf.filled = cnf.good;// the bad ones are replaced by the retry
	return DMR_DATA_RX_RETRY;
}

static dmrDataRxResult_t confirmedBlock(dmrDataPacket_t *p, const dmrBurst_t *burst)
{
	int dataLen = burst->length - 2;

	if ((dataLen <= 0) || (dataLen != dmrDataConfirmedBlockData(burst->dataType)) || (cnf.total == 0) ||
			(cnf.total > DMR_CONFIRMED_MAX_BLOCKS) || ((cnf.total * dataLen) > DMR_DATA_MAX_PACKET) ||
			(cnf.filled && (burst->dataType != cnf.dataType)))
	{
		rxCollecting = false;
		cnf.held = false;
		return DMR_DATA_RX_ERROR;
	}
	cnf.dataType = burst->dataType;
	p->dataType = burst->dataType;

	uint8_t serial = burst->payload[0] >> 1;
	uint16_t rxCrc9 = ((burst->payload[0] & 1) << 8) | burst->payload[1];
	bool ok = (dmrDataCRC9(&burst->payload[2], dataLen, serial) ^ crc9Mask(burst->dataType)) == rxCrc9;
	// A block whose CRC-9 failed can't be trusted for its serial number either: in a first try (blocks in order) it
	// goes where it arrived, in a selective retry it is dropped
	int place = ok ? serial : (cnf.retry ? -1 : cnf.arrived);

	cnf.arrived++;
	if ((place >= 0) && (place < cnf.total) && !((cnf.good >> place) & 1))
	{
		memcpy(&p->data[place * dataLen], &burst->payload[2], dataLen);
		cnf.filled |= 1ULL << place;
		if (ok)
		{
			cnf.good |= 1ULL << place;
		}
	}

	if (++rxBlocksReceived < p->blocksToFollow)
	{
		return DMR_DATA_RX_NONE;
	}
	rxCollecting = false;
	return confirmedFinish(p);
}

void dmrDataRxRetryFlags(uint8_t flags[8])
{
	uint64_t f = cnf.good | ~cnfAll();

	for (int i = 0; i < 8; i++)
	{
		flags[i] = (f >> (8 * i)) & 0xFF;
	}
}

bool dmrDataRxConfirmedActive(void)
{
	return rxCollecting && (dmrDataRxPacket.dpf == DMR_DPF_CONFIRMED);
}

dmrDataRxResult_t dmrDataRxConfirmedEnd(void)
{
	if (!dmrDataRxConfirmedActive())
	{
		return DMR_DATA_RX_NONE;
	}
	rxCollecting = false;
	return (cnf.dataType != 0) ? confirmedFinish(&dmrDataRxPacket) : DMR_DATA_RX_RETRY;
}

uint32_t dmrDataRxCrc9Mismatches(void)
{
	return crc9Mismatches;
}
#else
void dmrDataRxRetryFlags(uint8_t flags[8])
{
	memset(flags, 0xFF, 8);
}

bool dmrDataRxConfirmedActive(void)
{
	return false;
}

dmrDataRxResult_t dmrDataRxConfirmedEnd(void)
{
	return DMR_DATA_RX_NONE;
}

uint32_t dmrDataRxCrc9Mismatches(void)
{
	return 0;
}
#endif

static bool parseHeader(const uint8_t *h, dmrDataPacket_t *p)
{
	p->dpf = h[0] & 0x0F;
	p->sap = h[1] >> 4;
	p->group = (h[0] & 0x80) != 0;
	p->responseRequested = (h[0] & 0x40) != 0;
	p->dst = getId(&h[2]);
	p->src = getId(&h[5]);
	p->length = 0;
	p->sendSeq = 0;
	p->ddFormat = 0;
	p->padBits = 0;
	p->response = 0;
	p->firstTry = false;

	switch (p->dpf)
	{
		case DMR_DPF_UNCONFIRMED:
		case DMR_DPF_CONFIRMED:
			p->blocksToFollow = h[8] & 0x7F;
			p->padOctets = ((h[0] & 0x10) ? 0x10 : 0x00) | (h[1] & 0x0F);
			p->sendSeq = (h[9] >> 4) & 0x07;
			p->firstTry = (h[8] & 0x80) != 0;
			break;
		case DMR_DPF_DEFINED_SHORT:
		case DMR_DPF_RAW_SHORT:
			// Appended blocks are split over octets 0 and 1, octet 9 holds the bit padding
			p->blocksToFollow = (((h[0] >> 4) & 0x03) << 4) | (h[1] & 0x0F);
			p->padOctets = h[9] / 8;
			p->padBits = h[9];
			p->ddFormat = (p->dpf == DMR_DPF_DEFINED_SHORT) ? (h[8] >> 2) : 0;
			break;
		case DMR_DPF_RESPONSE:
			p->blocksToFollow = h[8] & 0x7F;
			p->padOctets = 0;
			p->response = h[9];
			break;
		case DMR_DPF_UDT:
			p->blocksToFollow = (h[8] & 0x03) + 1;// appended blocks are stored minus one
			p->padOctets = 0;
			p->ddFormat = h[1] & 0x0F;// the UDT format
			p->padBits = (h[8] >> 3) * 4;// pad nibbles
			break;
		default:
			return false;
	}

	return true;
}

dmrDataRxResult_t dmrDataRxBurst(const dmrBurst_t *burst)
{
	dmrDataPacket_t *p = &dmrDataRxPacket;

	switch (burst->dataType)
	{
		case DT_CSBK:
			if (!checkCRC16(burst->payload, 0xA5))
			{
				return DMR_DATA_RX_ERROR;
			}
			return ((burst->payload[0] & 0x3F) == DMR_CSBKO_PREAMBLE) ? DMR_DATA_RX_NONE : DMR_DATA_RX_CSBK;

		case DT_DATA_HEADER:
			// SAP 9 in the first header means a proprietary second header follows, it counts as one of the blocks to follow
			if (rxCollecting && (rxBlocksReceived == 0) && (p->sap == DMR_SAP_PROPRIETARY))
			{
#if CONFIG_DMR_CONFIRMED
				if ((p->dpf == DMR_DPF_CONFIRMED) && !cnf.retry && (cnf.total > 0))
				{
					cnf.total--;// the second header is one of the blocks to follow
				}
#endif
				if (++rxBlocksReceived >= p->blocksToFollow)
				{
					rxCollecting = false;
				}
				return DMR_DATA_RX_NONE;
			}

			rxCollecting = false;
			if (!checkCRC16(burst->payload, 0xCC) || !parseHeader(burst->payload, p))
			{
				return DMR_DATA_RX_ERROR;
			}

			if (p->blocksToFollow == 0)
			{
				p->dataType = DT_DATA_HEADER;
				return DMR_DATA_RX_PACKET;
			}
#if CONFIG_DMR_CONFIRMED
			if (p->dpf == DMR_DPF_CONFIRMED)
			{
				confirmedHeader(p);
			}
			else
			{
				cnf.held = false;// its blocks overwrite the held ones
			}
#endif
			rxCollecting = true;
			rxBlocksReceived = 0;
			rxBytes = 0;
			return DMR_DATA_RX_NONE;

		case DT_RATE_12_DATA:
		case DT_RATE_34_DATA:
		case DT_RATE_1_DATA:
			{
				if (!rxCollecting)
				{
					return DMR_DATA_RX_NONE;
				}
#if CONFIG_DMR_CONFIRMED
				if (p->dpf == DMR_DPF_CONFIRMED)
				{
					return confirmedBlock(p, burst);
				}
#endif

				// Confirmed blocks start with the 7 bit serial number and the CRC-9
				int skip = (p->dpf == DMR_DPF_CONFIRMED) ? 2 : 0;
				int len = burst->length - skip;

				if ((len <= 0) || ((rxBytes + len) > DMR_DATA_MAX_PACKET))
				{
					rxCollecting = false;
					return DMR_DATA_RX_ERROR;
				}

				memcpy(&p->data[rxBytes], &burst->payload[skip], len);
				rxBytes += len;
				p->dataType = burst->dataType;

				if (++rxBlocksReceived < p->blocksToFollow)
				{
					return DMR_DATA_RX_NONE;
				}

				rxCollecting = false;

				if (p->dpf == DMR_DPF_UDT)
				{
					// UDT blocks end with a CRC-CCITT, leave the decoding to the user
					p->length = rxBytes;
					return DMR_DATA_RX_PACKET;
				}

				if ((rxBytes < 4) || ((rxBytes - 4) < p->padOctets))
				{
					return DMR_DATA_RX_ERROR;
				}

				const uint8_t *c = &p->data[rxBytes - 4];
				uint32_t rxCrc = c[0] | (c[1] << 8) | (c[2] << 16) | ((uint32_t)c[3] << 24);

				if (dmrDataCRC32(p->data, rxBytes - 4) != rxCrc)
				{
					return DMR_DATA_RX_ERROR;
				}

				p->length = rxBytes - 4 - p->padOctets;
				return DMR_DATA_RX_PACKET;
			}

		default:
			return DMR_DATA_RX_NONE;
	}
}

static uint16_t compressedPort(uint8_t portId, const uint8_t **ptr)
{
	if (portId == 0)
	{
		uint16_t port = getU16(*ptr);

		*ptr += 2;
		return port;
	}
	for (unsigned i = 0; i < sizeof(udpcPorts) / sizeof(udpcPorts[0]); i++)
	{
		if (udpcPorts[i].id == portId)
		{
			return udpcPorts[i].port;
		}
	}
	return 0x8000 | portId;// Unknown, but still reported to the caller
}

static bool isWellKnownPort(uint16_t port)
{
	return (port == DMR_UDP_PORT_LRRP) || (port == DMR_UDP_PORT_ARS) || (port == DMR_UDP_PORT_TMS);
}

// Layer 4 of an IP based (SAP 4) packet, not a fragment
bool dmrDataGetIP(const dmrDataPacket_t *packet, dmrDataIP_t *ip)
{
	const uint8_t *d = packet->data;
	int len = packet->length;

	if ((packet->sap != DMR_SAP_IP) || (len < 20) || ((d[0] >> 4) != 4))
	{
		return false;
	}

	int ihl = (d[0] & 0x0F) * 4;
	int total = (d[2] << 8) | d[3];

	if ((ihl < 20) || (total < ihl) || (len < ihl) || ((((d[6] << 8) | d[7]) & 0x3FFF) != 0))
	{
		return false;// bad header, or a fragment
	}

	ip->protocol = d[9];
	ip->srcIp = getU32(&d[12]);
	ip->dstIp = getU32(&d[16]);
	ip->ttl = d[8];
	ip->payload = &d[ihl];
	ip->length = ((total < len) ? total : len) - ihl;// the blocks may be padded
	return true;
}

// UDP datagram of an IP based (SAP 4) or compressed UDP/IP (SAP 3) packet
bool dmrDataGetUDP(const dmrDataPacket_t *packet, dmrDataUDP_t *udp)
{
	const uint8_t *d = packet->data;
	int len = packet->length;

	if (packet->sap == DMR_SAP_IP)
	{
		int ihl = (d[0] & 0x0F) * 4;

		// (an IHL under 5 isn't IPv4, and a fragment isn't the whole datagram: as dmrDataGetIP)
		if ((len < 28) || ((d[0] >> 4) != 4) || (ihl < 20) || (d[9] != 17) || (len < (ihl + 8)) ||
				((((d[6] << 8) | d[7]) & 0x3FFF) != 0))
		{
			return false;
		}

		int udpLen = (d[ihl + 4] << 8) | d[ihl + 5];

		udp->srcPort = (d[ihl] << 8) | d[ihl + 1];
		udp->dstPort = (d[ihl + 2] << 8) | d[ihl + 3];
		udp->srcIp = getU32(&d[12]);
		udp->dstIp = getU32(&d[16]);
		udp->ttl = d[8];
		udp->payload = &d[ihl + 8];
		udp->length = len - ihl - 8;
		if ((udpLen >= 8) && ((udpLen - 8) < udp->length))
		{
			udp->length = udpLen - 8;
		}
	}
	else if (packet->sap == DMR_SAP_UDPIP_COMPRESSION)
	{
		// Opcode 00 (the MSBs of octets 3 and 4) is the only format defined (Table 7.19)
		if ((len < 5) || ((d[3] | d[4]) & 0x80))
		{
			return false;
		}

		const uint8_t *ptr = &d[5];
		udp->srcPort = compressedPort(d[3] & 0x7F, &ptr);
		udp->dstPort = compressedPort(d[4] & 0x7F, &ptr);
		// No addresses in the header: SAID 0 / DAID 0 or 2, the source and destination IDs' (TS 102 361-3 7.2)
		udp->srcIp = dmrDataIdAddress(false, packet->src);
		udp->dstIp = dmrDataIdAddress(packet->group, packet->dst);
		udp->ttl = ipTtl(packet->group, DMR_IP_TTL_DEFAULT);
		udp->payload = ptr;
		udp->length = len - (ptr - d);
		if (udp->length < 0)
		{
			return false;
		}
	}
	else
	{
		return false;
	}

	udp->appPort = (!isWellKnownPort(udp->dstPort) && isWellKnownPort(udp->srcPort)) ? udp->srcPort : udp->dstPort;
	return true;
}

#if CONFIG_DMR_TMS
bool dmrDataDecodeTMS(const uint8_t *payload, int length, dmrDataTMS_t *tms)
{
	if (length < 4)
	{
		return false;
	}

	uint8_t h = payload[2];
	int end = ((payload[0] << 8) | payload[1]) + 2;
	int idx = 4 + payload[3];// skip the address

	if (end > length)
	{
		end = length;
	}

	tms->ackRequested = (h & 0x40) != 0;
	tms->isAck = (h & 0x1F) == 0x1F;
	tms->seq = 0;
	tms->text[0] = 0;

	// Sequence number: its low 5 bits, then (extension bit set) bits 5-6 in the octet that also has the encoding
	if ((h & 0x80) && (idx < end))
	{
		tms->seq = payload[idx] & 0x1F;
		if ((payload[idx] & 0x80) && ((idx + 1) < end))
		{
			tms->seq |= payload[idx + 1] & 0x60;
		}
	}

	if (tms->isAck)
	{
		return true;
	}

	if ((h & 0x10) || (h & 0x0F))
	{
		return false;// control PDU, not a simple text message
	}

	if (h & 0x80)
	{
		// skip the sequence number and encoding octets, each with an extension bit
		while (idx < end)
		{
			if ((payload[idx++] & 0x80) == 0)
			{
				break;
			}
		}
	}

	int n = 0;
	for (; ((idx + 1) < end) && (n < (int)(sizeof(tms->text) - 1)); idx += 2)
	{
		uint16_t c = payload[idx] | (payload[idx + 1] << 8);

		if (c == 0)
		{
			break;
		}

		if ((c == '\r') || (c == '\n'))
		{
			if (n == 0)
			{
				continue;// some radios start the text with CR LF
			}
			c = ' ';
		}

		tms->text[n++] = (c < 0x100) ? c : '?';
	}
	tms->text[n] = 0;

	return true;
}
#endif

int dmrDataBuildARSRegistration(uint32_t radioId, uint8_t *out)
{
	char digits[8];
	int n = 0;

	radioId &= 0x00FFFFFF;
	do
	{
		digits[n++] = '0' + (radioId % 10);
		radioId /= 10;
	} while (radioId != 0);

	// Header: extension bit, ACK requested, priority, control, type 0. Then its extension: 0x20 as MOTOTRBO sends it
	out[2] = 0x80 | 0x40 | 0x20 | 0x10 | DMR_ARS_DEVICE_REGISTRATION;
	out[3] = 0x20;
	out[4] = n;
	for (int i = 0; i < n; i++)
	{
		out[5 + i] = digits[n - 1 - i];
	}
	out[5 + n] = 0;// user ID length
	out[6 + n] = 0;// password length

	int length = 7 + n;
	out[0] = (length - 2) >> 8;
	out[1] = (length - 2) & 0xFF;
	return length;
}

int dmrDataBuildARSDeregistration(uint8_t *out)
{
	out[0] = 0x00;
	out[1] = 0x01;
	out[2] = 0x20 | 0x10 | DMR_ARS_DEVICE_DEREGISTRATION;// priority, control
	return 3;
}

int dmrDataDecodeARS(const uint8_t *payload, int length)
{
	if (length < 3)
	{
		return -1;// too short (the length octets aren't read then)
	}

	int pduLength = (payload[0] << 8) | payload[1];

	if ((pduLength < 1) || ((pduLength + 2) > length) || ((payload[2] & 0x10) == 0))
	{
		return -1;// cut, or not a control PDU
	}

	return payload[2] & 0x0F;
}

bool dmrDataDecodeARSResponse(const uint8_t *payload, int length, bool *accepted, uint16_t *refreshMinutes)
{
	if (dmrDataDecodeARS(payload, length) != DMR_ARS_RESPONSE)
	{
		return false;
	}

	int pduLength = (payload[0] << 8) | payload[1];

	*accepted = ((payload[2] & 0x40) == 0);
	*refreshMinutes = (((payload[2] & 0x80) != 0) && (pduLength >= 2)) ? ((payload[3] & 0x7F) * DMR_ARS_REFRESH_UNIT_MINUTES) : DMR_ARS_REFRESH_NOT_GIVEN;

	return true;
}
