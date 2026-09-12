// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
/*
 * DMR packet data (ETSI TS 102 361-1 clause 8 / 9)
 */

#ifndef ODR_DMRDATA_H
#define ODR_DMRDATA_H

#include <stdbool.h>
#include <stdint.h>
#include "config.h"

// Big buffers only the CPU touches: in the board's CPU-only RAM where it has one (config.h ODR_CPU_RAM)
#define DMR_DATA_BUFFER ODR_CPU_RAM

// ETSI TS 102 361-1 data types (same values as the hotspot's DT_xxx)
#ifndef DT_CSBK
#define DT_VOICE_PI_HEADER		0x00U
#define DT_VOICE_LC_HEADER		0x01U
#define DT_TERMINATOR_WITH_LC	0x02U
#define DT_CSBK					0x03U
#define DT_MBC_HEADER			0x04U
#define DT_MBC_CONTINUATION		0x05U
#define DT_DATA_HEADER			0x06U
#define DT_RATE_12_DATA			0x07U
#define DT_RATE_34_DATA			0x08U
#define DT_IDLE					0x09U
#define DT_RATE_1_DATA			0x0AU
#endif

// This module has no hardware dependencies, so it can be unit tested on a host (see tests/host)

// Largest burst info field: Rate 1 data is 24 bytes, Rate 3/4 is 18, everything else is 12
#define DMR_BURST_PAYLOAD_MAX		24
#define DMR_DATA_MAX_BURSTS			40
#define DMR_DATA_MAX_PACKET			480			// 40 Rate 1/2 blocks

// Burst flags
#define DMR_BURST_FLAG_MCU_CRC		0x01		// TX: payload already holds its CRC / mask, the HR-C6000 must send it verbatim
#define DMR_BURST_FLAG_RX_CRC_ERROR	0x02		// RX: the HR-C6000 reported a CRC error for this burst

typedef struct
{
	uint8_t dataType;							// DT_xxx from dmrDefines.h
	uint8_t length;
	uint8_t flags;
	uint8_t payload[DMR_BURST_PAYLOAD_MAX];
} dmrBurst_t;

// Data packet formats (DPF) and service access points (SAP)
#define DMR_DPF_UDT					0x00
#define DMR_DPF_RESPONSE			0x01
#define DMR_DPF_UNCONFIRMED			0x02
#define DMR_DPF_CONFIRMED			0x03
#define DMR_DPF_DEFINED_SHORT		0x0D
#define DMR_DPF_RAW_SHORT			0x0E
#define DMR_DPF_PROPRIETARY			0x0F

#define DMR_SAP_UDT					0x00
#define DMR_SAP_UDPIP_COMPRESSION	0x03
#define DMR_SAP_IP					0x04
#define DMR_SAP_PROPRIETARY			0x09
#define DMR_SAP_SHORT_DATA			0x0A

#define DMR_CSBKO_CALL_ALERT		0x1F
#define DMR_CSBKO_CALL_ALERT_ACK	0x20
#define DMR_CSBKO_RADIO_CHECK		0x24		// byte 3 0x80: request, else the answer
#define DMR_CSBKO_EMERGENCY_ALARM	0x27		// emergency alarm (docs/spec/dmr-emergency.md 2)
#define DMR_CSBKO_PREAMBLE			0x3D

// Feature set IDs whose call alert and radio check CSBKs are answered
#define DMR_FID_ETSI				0x00
#define DMR_FID_MOTOROLA			0x10

// IP protocols
#define DMR_IP_PROTO_ICMP			1
#define DMR_IP_PROTO_UDP			17
#define DMR_IP_PROTO_SCTP			132

#define DMR_UDP_PORT_LRRP			4001
#define DMR_UDP_PORT_ARS			4005
#define DMR_UDP_PORT_TMS			4007

typedef struct
{
	uint8_t  dpf;
	uint8_t  sap;
	bool     group;
	bool     responseRequested;
	uint32_t dst;
	uint32_t src;
	uint8_t  blocksToFollow;
	uint8_t  padOctets;
	uint8_t  sendSeq;						// N(S), confirmed only
	uint8_t  ddFormat;						// defined short data: the DD format (6 bits); UDT: the UDT format
	uint8_t  padBits;						// defined short data: the bit padding; UDT: the pad nibbles x 4
	uint8_t  response;						// response packets: class (7-6), type (5-3), status (2-0)
	uint8_t  dataType;						// DT of the blocks
	uint16_t length;						// user data length
	uint8_t  data[DMR_DATA_MAX_PACKET];
} dmrDataPacket_t;

typedef enum
{
	DMR_DATA_RX_NONE,							// burst consumed, nothing complete yet
	DMR_DATA_RX_PACKET,							// a complete packet is in dmrDataRxPacket
	DMR_DATA_RX_CSBK,							// a non preamble CSBK is in the burst passed in
	DMR_DATA_RX_ERROR							// header or packet CRC failed, reassembly reset
} dmrDataRxResult_t;

extern dmrDataPacket_t dmrDataRxPacket;

uint32_t dmrDataCRC32(const uint8_t *data, int length);
// The CRC-CCITT of a 12-octet CSBK / header / MBC info field (octets 10-11) with its mask (CSBK 0xA5, data header
// 0xCC, MBC 0xAA, as one octet repeated)
bool dmrDataCheckCRC16(const uint8_t *burst12, uint8_t mask);

// TX builders. Each returns the number of bursts written to out, or 0 if the data does not fit
int dmrDataBuildCSBK(const uint8_t csbk[10], dmrBurst_t *out);
// The answer from us (ID src) to a received CSBK (its 10 bytes, CRC checked): the ack of a call alert, or the
// answer to a radio check. 0 if the CSBK isn't one of those, or isn't addressed to us
int dmrDataBuildCSBKAnswer(const uint8_t *csbk, uint32_t src, dmrBurst_t *out);
// blockType: DT_RATE_12_DATA, DT_RATE_34_DATA or DT_RATE_1_DATA
int dmrDataBlockLength(uint8_t blockType);
int dmrDataBuildPacket(uint8_t dpf, uint8_t sap, bool group, uint32_t dst, uint32_t src,
						const uint8_t *data, int length, uint8_t blockType, int preambles, dmrBurst_t *out, int maxBursts);
int dmrDataBuildUDP(bool group, uint32_t dst, uint32_t src, uint16_t srcPort, uint16_t dstPort,
						const uint8_t *payload, int length, uint8_t blockType, int preambles, dmrBurst_t *out, int maxBursts);
// An IPv4 packet of any protocol from layer 4 on (the header and its checksum included; a UDP checksum is redone)
int dmrDataBuildIP(bool group, uint32_t dst, uint32_t src, uint8_t protocol, const uint8_t *l4, int length,
						uint8_t blockType, int preambles, dmrBurst_t *out, int maxBursts);
int dmrDataBuildTMS(bool group, uint32_t dst, uint32_t src, const char *text, uint8_t seq, bool ackRequested,
						uint8_t blockType, int preambles, dmrBurst_t *out, int maxBursts);
int dmrDataBuildResponseAck(uint8_t sap, uint32_t dst, uint32_t src, uint8_t sendSeq, dmrBurst_t *out);
// The ACK class / type of a response packet (dmrDataPacket_t.response): class 0, type 1
#define DMR_RESPONSE_IS_ACK(r)		(((r) & 0xF8) == 0x08)
// seq: the 7 bit sequence number of the message being acknowledged
int dmrDataBuildTMSAck(uint32_t dst, uint32_t src, uint8_t seq, int preambles, dmrBurst_t *out, int maxBursts);

// A position for the location reports, as NMEA gives it
typedef struct
{
	bool     fixValid;
	bool     north;
	bool     east;
	uint8_t  latDegrees;
	uint8_t  lonDegrees;
	uint32_t latMinutes10000;				// minutes in ten thousandths, 0 - 599999
	uint32_t lonMinutes10000;
	uint16_t speedKnots;					// DMR_LOCATION_SPEED_UNKNOWN when not known
	uint8_t  utcHours;
	uint8_t  utcMinutes;
	uint8_t  utcSeconds;
} dmrDataLocation_t;
#define DMR_LOCATION_SPEED_UNKNOWN		127

// ETSI TS 102 361-4 UDT location report: a UDT header (format 5, NMEA) and one Rate 1/2 block with the short NMEA
// format (80 bits: C, N/S, E/W, Q, speed in knots, latitude and longitude in degrees and minutes, UTC to ten
// seconds), its CRC-CCITT unmasked, after the preamble CSBKs (none straight after a call of ours: receivers are in sync)
#define DMR_UDT_FORMAT_NMEA				0x05
#define DMR_UDT_OPCODE					0x1A	// as RadioDesk's builder (C_UDTHU)
int dmrDataBuildUDTLocation(bool group, uint32_t dst, uint32_t src, const dmrDataLocation_t *location, int preambles,
							dmrBurst_t *out, int maxBursts);

// ETSI short data text (defined short data, TS 102 361-1 9.2.12, DPF 13, SAP 10): the header carries the defined
// data format, rate 1/2 blocks with the CRC-32 follow. The DD formats used here (Table 9.50, docs/spec/dmr-data.md 5.2)
#define DMR_DD_BINARY					0x00
#define DMR_DD_BCD						0x01
#define DMR_DD_7BIT						0x02	// ISO/IEC 646, packed 7 bits per character
#define DMR_DD_ISO8859_1				0x03	// 0x03-0x11: ISO/IEC 8859-1 ... -16 (no -12)
#define DMR_DD_ISO8859_LAST				0x11
#define DMR_DD_UTF8						0x12
#define DMR_DD_UTF16					0x13	// with a byte order mark, big endian without one
#define DMR_DD_UTF16BE					0x14
#define DMR_DD_UTF16LE					0x15
// UDT formats that carry text (TS 102 361-4 UDT header), for reception
#define DMR_UDT_FORMAT_ISO7				0x03
#define DMR_UDT_FORMAT_ISO8				0x04
#define DMR_UDT_FORMAT_UTF16BE			0x07
#define DMR_ETSI_TEXT_MAX				140		// characters we send
// Text (Latin-1, NUL terminated) as defined short data in ISO 8859-1, with an ETSI response requested when
// responseRequested (individual only). 0 if it doesn't fit
int dmrDataBuildEtsiText(bool group, uint32_t dst, uint32_t src, const char *text, bool responseRequested,
						int preambles, dmrBurst_t *out, int maxBursts);
// The text of a received defined short data (any of the text DD formats above) or UDT text packet, as Latin-1 ('?'
// for characters outside it). False when the packet isn't one, or its format isn't text
bool dmrDataDecodeEtsiText(const dmrDataPacket_t *packet, char *text, int textSize);

// RX
void dmrDataRxReset(void);
dmrDataRxResult_t dmrDataRxBurst(const dmrBurst_t *burst);
typedef struct
{
	uint16_t srcPort;
	uint16_t dstPort;
	uint16_t appPort;					// the well known one of the two (LRRP, ARS, TMS), else the destination port
	const uint8_t *payload;
	int length;
} dmrDataUDP_t;

bool dmrDataGetUDP(const dmrDataPacket_t *packet, dmrDataUDP_t *udp);

// Layer 4 of an IP based packet (SAP 4), whatever its protocol; false for anything else and for fragments
typedef struct
{
	uint8_t protocol;
	const uint8_t *payload;
	int length;
} dmrDataIP_t;

bool dmrDataGetIP(const dmrDataPacket_t *packet, dmrDataIP_t *ip);

// Decoded TMS text message, text is converted from UTF-16LE to Latin-1 ('?' for anything else)
typedef struct
{
	bool    isAck;
	bool    ackRequested;
	uint8_t seq;							// 7 bit sequence number
	char    text[160];
} dmrDataTMS_t;

bool dmrDataDecodeTMS(const uint8_t *payload, int length, dmrDataTMS_t *tms);

// Motorola ARS (Automatic Registration Service, UDP 4005): the radio registers with a presence server, the "ARS radio
// ID". PDU types (the low 4 bits of the header octet)
#define DMR_ARS_DEVICE_REGISTRATION		0x00
#define DMR_ARS_DEVICE_DEREGISTRATION	0x01
#define DMR_ARS_QUERY					0x04		// from the server: register again
#define DMR_ARS_RESPONSE				0x0F		// from the server: the registration's acknowledgment
#define DMR_ARS_MAX_PDU					16

// The UDP payloads, as MOTOTRBO sends them. Each returns its length (at most DMR_ARS_MAX_PDU)
// Registration: 00 LL F0 20 <n> <device ID: the radio ID in decimal, n digits> 00 00 (no user ID, no password)
int dmrDataBuildARSRegistration(uint32_t radioId, uint8_t *out);
// Deregistration: 00 01 31
int dmrDataBuildARSDeregistration(uint8_t *out);
// The PDU type of an ARS payload (DMR_ARS_xxx), or -1 when it isn't one
int dmrDataDecodeARS(const uint8_t *payload, int length);
// The server's answer to a registration (DMR_ARS_RESPONSE): accepted unless its ACK/NAK bit (0x40) is set, and the
// refresh time it asks for in minutes. The octet after the header (as in hbnet's 00 02 BF 01) counts 30 minute units,
// 0 meaning no refresh expected (ok-dmrlib's ResponseSecondHeader); DMR_ARS_REFRESH_NOT_GIVEN when there's no such octet
bool dmrDataDecodeARSResponse(const uint8_t *payload, int length, bool *accepted, uint16_t *refreshMinutes);
#define DMR_ARS_REFRESH_UNIT_MINUTES	30
#define DMR_ARS_REFRESH_NOT_GIVEN		0xFFFF

#endif
