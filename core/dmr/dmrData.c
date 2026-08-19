// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
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
#define UDPC_PORT_ID_MOTOROLA_TMS	98
#define DMR_UDP_PORT_ETSI_TEXT		5016

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

// The IPv4 header (Motorola CAI addresses) in front of the length bytes of layer 4 already at ipPacket[20]. A UDP
// checksum covers the addresses, so it is worked out here; ICMP and SCTP checksums don't, they are kept
static int buildIPPacket(bool group, uint32_t dst, uint32_t src, uint8_t protocol, int length, uint8_t blockType,
							int preambles, dmrBurst_t *out, int maxBursts)
{
	int total = 20 + length;

	memset(ipPacket, 0, 20);
	ipPacket[0] = 0x45;// IPv4, 20 byte header
	putU16(&ipPacket[2], total);
	putU16(&ipPacket[4], ipIdentification++);
	ipPacket[8] = group ? 1 : 64;// TTL
	ipPacket[9] = protocol;
	ipPacket[12] = CAI_NETWORK_INDIVIDUAL;
	putId(&ipPacket[13], src);
	ipPacket[16] = group ? CAI_NETWORK_GROUP : CAI_NETWORK_INDIVIDUAL;
	putId(&ipPacket[17], dst);
	putU16(&ipPacket[10], ~onesComplementSum(0, ipPacket, 20));

	if (protocol == DMR_IP_PROTO_UDP)
	{
		// Over the pseudo header (addresses, protocol, UDP length) and the datagram
		putU16(&ipPacket[26], 0);
		uint16_t sum = onesComplementSum(DMR_IP_PROTO_UDP + length, &ipPacket[12], 8);
		sum = ~onesComplementSum(sum, &ipPacket[20], length);
		putU16(&ipPacket[26], (sum == 0) ? 0xFFFF : sum);
	}

	return dmrDataBuildPacket(DMR_DPF_UNCONFIRMED, DMR_SAP_IP, group, dst, src, ipPacket, total, blockType, preambles, out, maxBursts);
}

int dmrDataBuildIP(bool group, uint32_t dst, uint32_t src, uint8_t protocol, const uint8_t *l4, int length,
					uint8_t blockType, int preambles, dmrBurst_t *out, int maxBursts)
{
	if ((length < ((protocol == DMR_IP_PROTO_UDP) ? 8 : 1)) || ((20 + length) > (DMR_DATA_MAX_PACKET - 16)))
	{
		return 0;
	}

	memmove(&ipPacket[20], l4, length);
	if (protocol == DMR_IP_PROTO_UDP)
	{
		putU16(&ipPacket[24], length);
	}
	return buildIPPacket(group, dst, src, protocol, length, blockType, preambles, out, maxBursts);
}

int dmrDataBuildUDP(bool group, uint32_t dst, uint32_t src, uint16_t srcPort, uint16_t dstPort,
						const uint8_t *payload, int length, uint8_t blockType, int preambles, dmrBurst_t *out, int maxBursts)
{
	if ((length < 0) || ((28 + length) > (DMR_DATA_MAX_PACKET - 16)))
	{
		return 0;
	}

	memmove(&ipPacket[28], payload, length);
	putU16(&ipPacket[20], srcPort);
	putU16(&ipPacket[22], dstPort);
	putU16(&ipPacket[24], 8 + length);
	return buildIPPacket(group, dst, src, DMR_IP_PROTO_UDP, 8 + length, blockType, preambles, out, maxBursts);
}

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

// Figure 8.5 response header: class 0, type 1 (ACK), status = N(S) of the packet being acknowledged
int dmrDataBuildResponseAck(uint8_t sap, uint32_t dst, uint32_t src, uint8_t sendSeq, dmrBurst_t *out)
{
	uint8_t buf[12];

	buf[0] = DMR_DPF_RESPONSE;
	buf[1] = sap << 4;
	putId(&buf[2], dst);
	putId(&buf[5], src);
	buf[8] = 0x00;
	buf[9] = (0x01 << 3) | (sendSeq & 0x07);
	addCRC16(buf, 0xCC);
	setBurst(out, DT_DATA_HEADER, buf, 12);

	return 1;
}

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

void dmrDataRxReset(void)
{
	rxCollecting = false;
}

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

	switch (p->dpf)
	{
		case DMR_DPF_UNCONFIRMED:
		case DMR_DPF_CONFIRMED:
			p->blocksToFollow = h[8] & 0x7F;
			p->padOctets = ((h[0] & 0x10) ? 0x10 : 0x00) | (h[1] & 0x0F);
			p->sendSeq = (h[9] >> 4) & 0x07;
			break;
		case DMR_DPF_DEFINED_SHORT:
		case DMR_DPF_RAW_SHORT:
			// Appended blocks are split over octets 0 and 1, octet 9 holds the bit padding
			p->blocksToFollow = (((h[0] >> 4) & 0x03) << 4) | (h[1] & 0x0F);
			p->padOctets = h[9] / 8;
			break;
		case DMR_DPF_RESPONSE:
			p->blocksToFollow = h[8] & 0x7F;
			p->padOctets = 0;
			break;
		case DMR_DPF_UDT:
			p->blocksToFollow = (h[8] & 0x03) + 1;// appended blocks are stored minus one
			p->padOctets = 0;
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
	uint16_t port;

	switch (portId)
	{
		case 0:
			port = ((*ptr)[0] << 8) | (*ptr)[1];
			*ptr += 2;
			return port;
		case UDPC_PORT_ID_ETSI_TEXT:
			return DMR_UDP_PORT_ETSI_TEXT;
		case UDPC_PORT_ID_MOTOROLA_TMS:
			return DMR_UDP_PORT_TMS;
		default:
			return 0x8000 | portId;// Unknown, but still reported to the caller
	}
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

		if ((len < 28) || ((d[0] >> 4) != 4) || (d[9] != 17) || (len < (ihl + 8)))
		{
			return false;
		}

		int udpLen = (d[ihl + 4] << 8) | d[ihl + 5];

		udp->srcPort = (d[ihl] << 8) | d[ihl + 1];
		udp->dstPort = (d[ihl + 2] << 8) | d[ihl + 3];
		udp->payload = &d[ihl + 8];
		udp->length = len - ihl - 8;
		if ((udpLen >= 8) && ((udpLen - 8) < udp->length))
		{
			udp->length = udpLen - 8;
		}
	}
	else if (packet->sap == DMR_SAP_UDPIP_COMPRESSION)
	{
		if (len < 5)
		{
			return false;
		}

		const uint8_t *ptr = &d[5];
		udp->srcPort = compressedPort(d[3] & 0x7F, &ptr);
		udp->dstPort = compressedPort(d[4] & 0x7F, &ptr);
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
	int pduLength = (payload[0] << 8) | payload[1];

	if ((length < 3) || (pduLength < 1) || ((pduLength + 2) > length) || ((payload[2] & 0x10) == 0))
	{
		return -1;// too short, or not a control PDU
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
